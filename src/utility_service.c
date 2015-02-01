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

#include <dynamicbox_errno.h>
#include <packet.h>

#include <sys/smack.h>

#include "critical_log.h"
#include "service_common.h"
#include "utility_service.h"
#include "debug.h"
#include "util.h"
#include "conf.h"

#ifndef SVC_PKG
#define SVC_PKG		"org.tizen.data-provider-slave.icon"
#endif

#ifndef LAUNCH_TIMEOUT
#define LAUNCH_TIMEOUT	10.0f
#endif

#ifndef TTL_TIMEOUT
#define TTL_TIMEOUT	30.0f
#endif

static struct info {
	Eina_List *pending_list;
	Eina_List *context_list;
	struct service_context *svc_ctx;

	struct tcb *svc_daemon;
	int svc_daemon_is_launched;
	int svc_daemon_pid;

	struct service_event_item *launch_timer; 
	struct service_event_item *delay_launcher;
	struct service_event_item *ttl_timer;
} s_info = {
	.pending_list = NULL,
	.context_list = NULL, /*!< \WARN: This is only used for SERVICE THREAD */
	.svc_ctx = NULL, /*!< \WARN: This is only used for MAIN THREAD */

	.svc_daemon = NULL,
	.svc_daemon_is_launched = 0,
	.svc_daemon_pid = -1,

	.launch_timer = NULL,
	.delay_launcher = NULL,
	.ttl_timer = NULL,
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

static int put_reply_tcb(struct tcb *tcb, double seq)
{
	struct context *ctx;

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	ctx->tcb = tcb;
	ctx->seq = seq;

	s_info.context_list = eina_list_append(s_info.context_list, ctx);

	return DBOX_STATUS_ERROR_NONE;
}

static inline struct tcb *get_reply_tcb(double seq)
{
	Eina_List *l;
	Eina_List *n;
	struct context *ctx;
	struct tcb *tcb;

	EINA_LIST_FOREACH_SAFE(s_info.context_list, l, n, ctx) {
		if (ctx->seq != seq) {
			continue;
		}

		s_info.context_list = eina_list_remove(s_info.context_list, ctx);
		tcb = ctx->tcb;
		DbgFree(ctx);
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
		if (tcb_is_valid(s_info.svc_ctx, s_info.svc_daemon) >= 0) {
			ret = service_common_unicast_packet(s_info.svc_daemon, item->packet);
		} else {
			ret = -EFAULT;
		}

		if (ret < 0) {
			if (tcb_is_valid(s_info.svc_ctx, item->tcb) >= 0) {
				struct packet *reply;
				reply = packet_create_reply(item->packet, "i", ret);
				if (service_common_unicast_packet(item->tcb, reply) < 0) {
					ErrPrint("Unable to send packet\n");
				}
				packet_destroy(reply);
			}
		} else {
			put_reply_tcb(item->tcb, packet_seq(item->packet));
		}
		packet_unref(item->packet);
		DbgFree(item);
	}

	return 0;
}

static inline int put_pended_request(struct tcb *tcb, struct packet *packet)
{
	struct pending_item *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	item->tcb = tcb;
	item->packet = packet_ref(packet);
	if (!item->packet) {
		DbgFree(item);
		ErrPrint("Unable to ref packet\n");
		return DBOX_STATUS_ERROR_FAULT;
	}

	s_info.pending_list = eina_list_append(s_info.pending_list, item);
	return 0;
}

static int launch_timeout_cb(struct service_context *svc_ctx, void *data)
{
	struct pending_item *item;
	struct packet *reply;

	EINA_LIST_FREE(s_info.pending_list, item) {
		if (tcb_is_valid(s_info.svc_ctx, item->tcb) >= 0) {
			reply = packet_create_reply(item->packet, "i", -EFAULT);
			if (!reply) {
				ErrPrint("Unable to create a packet\n");
			} else {
				int ret;

				ret = service_common_unicast_packet(item->tcb, reply);
				if (ret < 0) {
					ErrPrint("Failed to send reply packet: %d\n", ret);
				}

				packet_destroy(reply);
			}
		} else {
			ErrPrint("TCB is already terminated\n");
		}

		packet_unref(item->packet);
		DbgFree(item);
	}

	s_info.launch_timer = NULL;
	s_info.svc_daemon_is_launched = 0;
	s_info.svc_daemon_pid = -1;
	return -ECANCELED; /* Delete this timer */
}

static int launch_svc(struct service_context *svc_ctx)
{
	pid_t pid;
	int ret = DBOX_STATUS_ERROR_NONE;

	pid = aul_launch_app(SVC_PKG, NULL);
	switch (pid) {
	case AUL_R_EHIDDENFORGUEST:	/**< App hidden for guest mode */
	case AUL_R_ENOLAUNCHPAD:	/**< no launchpad */
	case AUL_R_EILLACC:		/**< Illegal Access */
	case AUL_R_EINVAL:		/**< Invalid argument */
	case AUL_R_ENOINIT:		/**< AUL handler NOT initialized */
	case AUL_R_ERROR:		/**< General error */
		ErrPrint("Failed to launch an app: %s(%d)\n", SVC_PKG, pid);
		ret = DBOX_STATUS_ERROR_FAULT;
		break;
	case AUL_R_ETIMEOUT:		/**< Timeout */
	case AUL_R_ECOMM:		/**< Comunication Error */
	case AUL_R_ETERMINATING:	/**< application terminating */
	case AUL_R_ECANCELED:		/**< Operation canceled */
		/* Need time to launch app again */
		ErrPrint("Terminating now, try to launch this after few sec later: %s(%d)\n", SVC_PKG, pid);
		s_info.svc_daemon_is_launched = 1;
		s_info.delay_launcher = service_common_add_timer(svc_ctx, LAUNCH_TIMEOUT, lazy_launcher_cb, NULL);
		if (!s_info.delay_launcher) {
			ErrPrint("Unable to add delay launcher\n");
			ret = DBOX_STATUS_ERROR_FAULT;
		}
		break;
	case AUL_R_LOCAL:		/**< Launch by himself */
	case AUL_R_OK:			/**< General success */
	default:
		DbgPrint("Launched: %s(%d)\n", SVC_PKG, pid);
		s_info.svc_daemon_is_launched = 1;
		s_info.svc_daemon_pid = pid;
		s_info.launch_timer = service_common_add_timer(svc_ctx, LAUNCH_TIMEOUT, launch_timeout_cb, NULL);
		if (!s_info.launch_timer) {
			ErrPrint("Unable to create launch timer\n");
		}
	}

	return ret;
}

static int lazy_launcher_cb(struct service_context *svc_ctx, void *data)
{
	s_info.delay_launcher = NULL;

	(void)launch_svc(svc_ctx);
	return -ECANCELED;
}

static int ttl_timer_cb(struct service_context *svc_ctx, void *data)
{
	DbgPrint("TTL Timer is expired: PID(%d)\n", s_info.svc_daemon_pid);
	(void)aul_terminate_pid_async(s_info.svc_daemon_pid);

	s_info.ttl_timer = NULL;
	s_info.svc_daemon_is_launched = 0;
	s_info.svc_daemon_pid = -1;
	s_info.svc_daemon = NULL;
	return -ECANCELED;
}

static int service_thread_main(struct tcb *tcb, struct packet *packet, void *data)
{
	struct packet *reply;
	const char *cmd;
	int ret;

	if (!packet) {
		DbgPrint("TCB %p is terminated (NIL packet), %d\n", tcb, s_info.svc_daemon_pid);

		if (tcb == s_info.svc_daemon) {
			s_info.svc_daemon = NULL;
			s_info.svc_daemon_is_launched = 0;
			s_info.svc_daemon_pid = -1;

			if (s_info.ttl_timer) {
				service_common_del_timer(tcb_svc_ctx(tcb), s_info.ttl_timer);
				s_info.ttl_timer = NULL;
			}
		}

		return DBOX_STATUS_ERROR_NONE;
	}

	cmd = packet_command(packet);
	if (!cmd) {
		ErrPrint("Invalid packet\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	switch (packet_type(packet)) {
	case PACKET_REQ:
		if (!s_info.svc_daemon_is_launched) {
			ret = launch_svc(tcb_svc_ctx(tcb));
			if (ret != DBOX_STATUS_ERROR_NONE) {
				goto reply_out;
			}
		}

		if (!s_info.svc_daemon) {
			ret = put_pended_request(tcb, packet);
			if (ret < 0) {
				goto reply_out;
			}
		} else if (tcb_is_valid(s_info.svc_ctx, s_info.svc_daemon) >= 0) { 
			ret = service_common_unicast_packet(s_info.svc_daemon, packet);
			if (ret <0) {
				goto reply_out;
			}

			put_reply_tcb(tcb, packet_seq(packet));

			if (s_info.ttl_timer && service_common_update_timer(s_info.ttl_timer, TTL_TIMEOUT) < 0) {
				ErrPrint("Failed to update timer\n");
			}
		}

		break;
	case PACKET_REQ_NOACK:
		if (!strcmp(cmd, "service_register")) {
			if (!s_info.svc_daemon_is_launched) {
				ErrPrint("Service daemon is not launched. but something tries to register a service\n");
				return DBOX_STATUS_ERROR_INVALID_PARAMETER;
			}

			if (s_info.svc_daemon) {
				ErrPrint("Service daemon is already prepared\n");
				return DBOX_STATUS_ERROR_INVALID_PARAMETER;
			}

			if (s_info.launch_timer) {
				service_common_del_timer(tcb_svc_ctx(tcb), s_info.launch_timer);
				s_info.launch_timer = NULL;
			}

			s_info.ttl_timer = service_common_add_timer(tcb_svc_ctx(tcb), TTL_TIMEOUT, ttl_timer_cb, NULL);
			if (!s_info.ttl_timer) {
				ErrPrint("Failed to add TTL timer\n");
				if (s_info.svc_daemon_pid > 0) {
					ret = aul_terminate_pid_async(s_info.svc_daemon_pid);
					ErrPrint("Terminate: %d\n", ret);
					s_info.svc_daemon_pid = -1;
				}
				s_info.svc_daemon_is_launched = 0;
				return DBOX_STATUS_ERROR_FAULT;
			}
			DbgPrint("TTL Timer is added: %p\n", s_info.ttl_timer);

			s_info.svc_daemon = tcb;
			flush_pended_request();
		}
		break;
	case PACKET_ACK:
		tcb = get_reply_tcb(packet_seq(packet));
		if (!tcb) {
			ErrPrint("Unable to find reply tcb\n");
			break;
		}

		if (tcb_is_valid(s_info.svc_ctx, tcb) < 0) {
			ErrPrint("TCB is not valid\n");
			break;
		}

		ret = service_common_unicast_packet(tcb, packet);
		if (ret < 0) {
			ErrPrint("Unable to forward the reply packet\n");
		}
		break;
	default:
		ErrPrint("Packet type is not valid[%s]\n", cmd);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	return DBOX_STATUS_ERROR_NONE;

reply_out:
	ErrPrint("Error: %d\n", ret);
	reply = packet_create_reply(packet, "i", ret);
	if (service_common_unicast_packet(tcb, reply) < 0) {
		ErrPrint("Unable to send reply packet\n");
	}
	packet_destroy(reply);
	return ret;
}

int utility_service_init(void)
{
	if (s_info.svc_ctx) {
		ErrPrint("Already initialized\n");
		return DBOX_STATUS_ERROR_ALREADY;
	}

	s_info.svc_ctx = service_common_create(UTILITY_SOCKET, service_thread_main, NULL);
	if (!s_info.svc_ctx) {
		ErrPrint("Unable to activate service thread\n");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (smack_fsetlabel(service_common_fd(s_info.svc_ctx), UTILITY_SMACK_LABEL, SMACK_LABEL_IPOUT) != 0) {
		if (errno != EOPNOTSUPP) {
			ErrPrint("Unable to set SMACK label(%d)\n", errno);
			service_common_destroy(s_info.svc_ctx);
			s_info.svc_ctx = NULL;
			return DBOX_STATUS_ERROR_FAULT;
		}
	}

	if (smack_fsetlabel(service_common_fd(s_info.svc_ctx), UTILITY_SMACK_LABEL, SMACK_LABEL_IPIN) != 0) {
		if (errno != EOPNOTSUPP) {
			ErrPrint("Unable to set SMACK label(%d)\n", errno);
			service_common_destroy(s_info.svc_ctx);
			s_info.svc_ctx = NULL;
			return DBOX_STATUS_ERROR_FAULT;
		}
	}

	DbgPrint("Successfully initiated\n");
	return DBOX_STATUS_ERROR_NONE;
}

int utility_service_fini(void)
{
	if (!s_info.svc_ctx) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	service_common_destroy(s_info.svc_ctx);
	s_info.svc_ctx = NULL;
	DbgPrint("Successfully Finalized\n");
	return DBOX_STATUS_ERROR_NONE;
}

/* End of a file */
