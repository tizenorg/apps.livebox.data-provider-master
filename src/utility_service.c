/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org/license/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>

#include <Eina.h>

#include <dlog.h>
#include <aul.h>

#include <livebox-errno.h>
#include <packet.h>

#include <sys/smack.h>

#include "critical_log.h"
#include "service_common.h"
#include "utility_service.h"
#include "debug.h"
#include "util.h"
#include "conf.h"

#ifndef SVC_PKG
#define SVC_PKG		"com.samsung.data-provider-slave.icon"
#endif

#ifndef LAUNCH_TIMEOUT
#define LAUNCH_TIMEOUT	10.0f
#endif

static struct info {
	Eina_List *pending_list;
	Eina_List *context_list;
	struct service_context *svc_ctx;

	struct tcb *svc_daemon;
	int svc_daemon_is_launched;

	struct service_event_item *launch_timer; 
	struct service_event_item *delay_launcher;
} s_info = {
	.pending_list = NULL,
	.context_list = NULL, /*!< \WARN: This is only used for SERVICE THREAD */
	.svc_ctx = NULL, /*!< \WARN: This is only used for MAIN THREAD */

	.svc_daemon = NULL,
	.svc_daemon_is_launched = 0,

	.launch_timer = NULL,
	.delay_launcher = NULL,
};

struct pending_item {
	struct tcb *tcb;
	struct packet *packet;
};

struct context {
	struct tcb *tcb;
	double seq;
};

static int lazy_launcher_cb(struct service_context *svc_ctx, void *data);

static inline int put_reply_tcb(struct tcb *tcb, double seq)
{
	struct context *ctx;

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	ctx->tcb = tcb;
	ctx->seq = seq;

	s_info.context_list = eina_list_append(s_info.context_list, ctx);
	return LB_STATUS_SUCCESS;
}

static inline struct tcb *get_reply_tcb(double seq)
{
	Eina_List *l;
	Eina_List *n;
	struct context *ctx;
	struct tcb *tcb;

	EINA_LIST_FOREACH_SAFE(s_info.context_list, l, n, ctx) {
		if (ctx->seq != seq)
			continue;

		s_info.context_list = eina_list_remove(s_info.context_list, ctx);
		tcb = ctx->tcb;
		free(ctx);
		return tcb;
	}

	return NULL;
}

static inline int flush_pended_request(void)
{
	/*!
	 * Flush all pended requests
	 */
	struct pending_item *item;
	int ret;

	EINA_LIST_FREE(s_info.pending_list, item) {
		ret = service_common_unicast_packet(s_info.svc_daemon, item->packet);
		if (ret < 0) {
			struct packet *reply;
			reply = packet_create_reply(item->packet, "i", ret);
			if (service_common_unicast_packet(item->tcb, reply) < 0)
				ErrPrint("Unable to send packet\n");
			packet_destroy(reply);
		} else {
			put_reply_tcb(item->tcb, packet_seq(item->packet));
		}
		packet_unref(item->packet);
		free(item);
	}

	return 0;
}

static inline int put_pended_request(struct tcb *tcb, struct packet *packet)
{
	struct pending_item *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	item->tcb = tcb;
	item->packet = packet_ref(packet);
	if (!item->packet) {
		free(item);
		ErrPrint("Unable to ref packet\n");
		return LB_STATUS_ERROR_FAULT;
	}

	s_info.pending_list = eina_list_append(s_info.pending_list, item);
	return 0;
}

static int launch_timeout_cb(struct service_context *svc_ctx, void *data)
{
	struct pending_item *item;
	struct packet *reply;

	EINA_LIST_FREE(s_info.pending_list, item) {
		reply = packet_create_reply(item->packet, "i", -EFAULT);
		if (!reply) {
			ErrPrint("Unable to create a packet\n");
		} else {
			int ret;

			ret = service_common_unicast_packet(item->tcb, reply);
			if (ret < 0)
				ErrPrint("Failed to send reply packet: %d\n", ret);

			packet_destroy(reply);
		}

		packet_unref(item->packet);
		free(item);
	}

	s_info.launch_timer = NULL;
	s_info.svc_daemon_is_launched = 0;
	return -ECANCELED; /* Delete this timer */
}

static inline int launch_svc(struct service_context *svc_ctx)
{
	pid_t pid;
	int ret = LB_STATUS_SUCCESS;

	pid = aul_launch_app(SVC_PKG, NULL);
	if (pid > 0) {
		s_info.svc_daemon_is_launched = 1;
		s_info.launch_timer = service_common_add_timer(svc_ctx, LAUNCH_TIMEOUT, launch_timeout_cb, NULL);
		if (!s_info.launch_timer)
			ErrPrint("Unable to create launch timer\n");
	} else if (pid == AUL_R_ETIMEOUT || pid == AUL_R_ECOMM) {
		s_info.svc_daemon_is_launched = 1;
		CRITICAL_LOG("SVC launch failed with timeout(%d), But waiting response\n", pid);
		s_info.launch_timer = service_common_add_timer(svc_ctx, LAUNCH_TIMEOUT, launch_timeout_cb, NULL);
		if (!s_info.launch_timer)
			ErrPrint("Unable to create launch timer\n");
	} else if (pid == AUL_R_ETERMINATING) {
		/* Need time to launch app again */
		ErrPrint("Terminating now, try to launch this after few sec later\n");
		s_info.svc_daemon_is_launched = 1;
		s_info.delay_launcher = service_common_add_timer(svc_ctx, LAUNCH_TIMEOUT, lazy_launcher_cb, NULL);
		if (!s_info.delay_launcher) {
			ErrPrint("Unable to add delay launcher\n");
			ret = LB_STATUS_ERROR_FAULT;
		}
	} else {
		ErrPrint("Failed to launch an app: %s(%d)\n", SVC_PKG, pid);
		ret = LB_STATUS_ERROR_FAULT;
	}

	return ret;
}

static int lazy_launcher_cb(struct service_context *svc_ctx, void *data)
{
	s_info.svc_daemon_is_launched = launch_svc(svc_ctx) == LB_STATUS_SUCCESS;
	s_info.delay_launcher = NULL;
	return -ECANCELED;
}

static int service_thread_main(struct tcb *tcb, struct packet *packet, void *data)
{
	struct packet *reply;
	const char *cmd;
	int ret;

	if (!packet) {
		DbgPrint("TCB %p is terminated (NIL packet)\n", tcb);

		if (tcb == s_info.svc_daemon) {
			s_info.svc_daemon = NULL;
			s_info.svc_daemon_is_launched = 0;
		}

		return LB_STATUS_SUCCESS;
	}

	cmd = packet_command(packet);
	if (!cmd) {
		ErrPrint("Invalid packet\n");
		return LB_STATUS_ERROR_INVALID;
	}

	switch (packet_type(packet)) {
	case PACKET_REQ:
		if (!s_info.svc_daemon_is_launched) {
			ret = launch_svc(tcb_svc_ctx(tcb));
			if (ret != LB_STATUS_SUCCESS)
				goto reply_out;
		}

		if (!s_info.svc_daemon) {
			ret = put_pended_request(tcb, packet);
			if (ret < 0)
				goto reply_out;
		} else {
			ret = service_common_unicast_packet(s_info.svc_daemon, packet);
			if (ret <0)
				goto reply_out;

			put_reply_tcb(tcb, packet_seq(packet));
		}

		break;
	case PACKET_REQ_NOACK:
		if (!strcmp(cmd, "service_register")) {
			if (!s_info.svc_daemon_is_launched) {
				ErrPrint("Service daemon is not launched. but something tries to register a service\n");
				return LB_STATUS_ERROR_INVALID;
			}

			if (s_info.svc_daemon) {
				ErrPrint("Service daemon is already prepared\n");
				return LB_STATUS_ERROR_INVALID;
			}

			if (s_info.launch_timer) {
				service_common_del_timer(tcb_svc_ctx(tcb), s_info.launch_timer);
				s_info.launch_timer = NULL;
			}

			s_info.svc_daemon = tcb;
			flush_pended_request();
		}
		break;
	case PACKET_ACK:
		tcb = get_reply_tcb(packet_seq(packet));
		if (!tcb) {
			ErrPrint("Unable to find reply tcb\n");
		} else {
			ret = service_common_unicast_packet(tcb, packet);
			if (ret < 0)
				ErrPrint("Unable to forward the reply packet\n");
		}
		break;
	default:
		ErrPrint("Packet type is not valid[%s]\n", cmd);
		return LB_STATUS_ERROR_INVALID;
	}

	return LB_STATUS_SUCCESS;

reply_out:
	ErrPrint("Error: %d\n", ret);
	reply = packet_create_reply(packet, "i", ret);
	if (service_common_unicast_packet(tcb, reply) < 0)
		ErrPrint("Unable to send reply packet\n");
	packet_destroy(reply);
	return ret;
}

int utility_service_init(void)
{
	if (s_info.svc_ctx) {
		ErrPrint("Already initialized\n");
		return LB_STATUS_ERROR_ALREADY;
	}

	s_info.svc_ctx = service_common_create(UTILITY_SOCKET, service_thread_main, NULL);
	if (!s_info.svc_ctx) {
		ErrPrint("Unable to activate service thread\n");
		return LB_STATUS_ERROR_FAULT;
	}

	if (smack_fsetlabel(service_common_fd(s_info.svc_ctx), UTILITY_SMACK_LABEL, SMACK_LABEL_IPOUT) != 0) {
		if (errno != EOPNOTSUPP) {
			ErrPrint("Unable to set SMACK label(%d)\n", errno);
			service_common_destroy(s_info.svc_ctx);
			s_info.svc_ctx = NULL;
			return LB_STATUS_ERROR_FAULT;
		}
	}

	if (smack_fsetlabel(service_common_fd(s_info.svc_ctx), UTILITY_SMACK_LABEL, SMACK_LABEL_IPIN) != 0) {
		if (errno != EOPNOTSUPP) {
			ErrPrint("Unable to set SMACK label(%d)\n", errno);
			service_common_destroy(s_info.svc_ctx);
			s_info.svc_ctx = NULL;
			return LB_STATUS_ERROR_FAULT;
		}
	}

	DbgPrint("Successfully initiated\n");
	return LB_STATUS_SUCCESS;
}

int utility_service_fini(void)
{
	if (!s_info.svc_ctx)
		return LB_STATUS_ERROR_INVALID;

	service_common_destroy(s_info.svc_ctx);
	s_info.svc_ctx = NULL;
	DbgPrint("Successfully Finalized\n");
	return LB_STATUS_SUCCESS;
}

/* End of a file */
