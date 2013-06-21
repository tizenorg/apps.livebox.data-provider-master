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
#include <livebox-errno.h>
#include <packet.h>

#include <Eina.h>
#include <sys/smack.h>

#include <security-server.h>

#include "service_common.h"
#include "debug.h"
#include "util.h"
#include "conf.h"

static struct info {
	Eina_List *context_list;
	struct service_context *svc_ctx;
} s_info = {
	.context_list = NULL, /*!< \WARN: This is only used for SERVICE THREAD */
	.svc_ctx = NULL, /*!< \WARN: This is only used for MAIN THREAD */
};

struct context {
	struct tcb *tcb;
	double seq;
};

/*!
 * SERVICE THREAD
 */
static inline int put_reply_context(struct tcb *tcb, double seq)
{
	struct context *ctx;

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	ctx->tcb = tcb;
	ctx->seq = seq; /* Could we this sequence value is uniq? */

	s_info.context_list = eina_list_append(s_info.context_list, ctx);
	return 0;
}

/*!
 * SERVICE THREAD
 */
static inline struct tcb *get_reply_context(double seq)
{
	Eina_List *l;
	Eina_List *n;
	struct context *ctx;
	struct tcb *tcb;

	tcb = NULL;
	EINA_LIST_FOREACH_SAFE(s_info.context_list, l, n, ctx) {
		if (ctx->seq != seq)
			continue;

		s_info.context_list = eina_list_remove(s_info.context_list, ctx);
		tcb = ctx->tcb;
		free(ctx);
		break;
	}

	return tcb;
}

/*!
 * SERVICE THREAD
 */
static int service_thread_main(struct tcb *tcb, struct packet *packet, void *data)
{
	const char *command;
	int ret;

	if (!packet) {
		DbgPrint("TCB: %p is terminated (NIL packet)\n", tcb);
		return 0;
	}

	command = packet_command(packet);
	if (!command) {
		ErrPrint("Invalid command\n");
		return -EINVAL;
	}

	switch (packet_type(packet)) {
	case PACKET_REQ:

		/* Need to send reply packet */
		DbgPrint("%p REQ: Command: [%s]\n", tcb, command);
		if (!strcmp(command, "add_livebox")) {
			ret = security_server_check_privilege_by_sockfd(tcb_fd(tcb), "data-provider-master::shortcut.livebox", "w");
			if (ret == SECURITY_SERVER_API_ERROR_ACCESS_DENIED) {
				ErrPrint("SMACK:Access denied\n");
			}
		} else if (!strcmp(command, "add_shortcut")) {
			ret = security_server_check_privilege_by_sockfd(tcb_fd(tcb), "data-provider-master::shortcut.shortcut", "w");
			if (ret == SECURITY_SERVER_API_ERROR_ACCESS_DENIED) {
				ErrPrint("SMACK:Access denied\n");
			}
		}

		if (service_common_multicast_packet(tcb, packet, TCB_CLIENT_TYPE_SERVICE) < 0)
			ErrPrint("Unable to send service request packet\n");
		else
			(void)put_reply_context(tcb, packet_seq(packet));
		break;
	case PACKET_REQ_NOACK:
		/* Doesn't need to send reply packet */
		DbgPrint("%p REQ_NOACK: Command: [%s]\n", tcb, command);
		if (!strcmp(command, "service_register")) {
			tcb_client_type_set(tcb, TCB_CLIENT_TYPE_SERVICE);
			break;
		}

		if (service_common_multicast_packet(tcb, packet, TCB_CLIENT_TYPE_SERVICE) < 0)
			ErrPrint("Unable to send service reuqest packet\n");
		break;
	case PACKET_ACK:
		/* Okay, client(or app) send a reply packet to us. */
		DbgPrint("%p ACK: Command: [%s]\n", tcb, command);
		tcb = get_reply_context(packet_seq(packet));
		if (!tcb) {
			ErrPrint("There is no proper context\n");
			break;
		}

		if (service_common_unicast_packet(tcb, packet) < 0)
			ErrPrint("Unable to send reply packet\n");
		break;
	default:
		ErrPrint("Packet type is not valid[%s]\n", command);
		return -EINVAL;
	}

	/*!
	 * return value has no meanning,
	 * it will be printed by dlogutil.
	 */
	return 0;
}


/*!
 * MAIN THREAD
 * Do not try to do anyother operation in these functions
 */

HAPI int shortcut_service_init(void)
{
	if (s_info.svc_ctx) {
		ErrPrint("Already initialized\n");
		return LB_STATUS_ERROR_ALREADY;
	}

	s_info.svc_ctx = service_common_create(SHORTCUT_SOCKET, service_thread_main, NULL);
	if (!s_info.svc_ctx) {
		ErrPrint("Unable to activate service thread\n");
		return LB_STATUS_ERROR_FAULT;
	}

	if (smack_fsetlabel(service_common_fd(s_info.svc_ctx), SHORTCUT_SMACK_LABEL, SMACK_LABEL_IPOUT) != 0) {
		if (errno != EOPNOTSUPP) {
			ErrPrint("Unable to set SMACK label(%d)\n", errno);
			service_common_destroy(s_info.svc_ctx);
			s_info.svc_ctx = NULL;
			return LB_STATUS_ERROR_FAULT;
		}
	}

	if (smack_fsetlabel(service_common_fd(s_info.svc_ctx), SHORTCUT_SMACK_LABEL, SMACK_LABEL_IPIN) != 0) {
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

HAPI int shortcut_service_fini(void)
{
	if (!s_info.svc_ctx)
		return LB_STATUS_ERROR_INVALID;

	service_common_destroy(s_info.svc_ctx);
	s_info.svc_ctx = NULL;
	DbgPrint("Successfully Finalized\n");
	return LB_STATUS_SUCCESS;
}

/* End of a file */
