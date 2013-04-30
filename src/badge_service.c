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

#include <badge.h>
#include <badge_db.h>

#include "service_common.h"
#include "debug.h"
#include "util.h"
#include "conf.h"

#define BADGE_ADDR "/tmp/.badge.service"

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

struct noti_service {
	const char *cmd;
	void (*handler)(struct tcb *tcb, struct packet *packet, void *data);
};

/*!
 * FUNCTIONS to handle badge
 */
static inline char *get_string(char *string)
{
	if (string == NULL) {
		return NULL;
	}
	if (string[0] == '\0') {
		return NULL;
	}

	return string;
}

/*!
 * SERVICE HANDLER
 */
static void _handler_insert_badge(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0;
	struct packet *packet_reply = NULL;
	struct packet *packet_service = NULL;
	char *pkgname = NULL;
	char *writable_pkg = NULL;
	char *caller = NULL;

	if (packet_get(packet, "sss", &pkgname, &writable_pkg, &caller) == 3) {
		pkgname = get_string(pkgname);
		writable_pkg = get_string(writable_pkg);
		caller = get_string(caller);

		if (pkgname != NULL && writable_pkg != NULL && caller != NULL) {
			ret = badge_db_insert(pkgname, writable_pkg, caller);

		} else {
			ret = BADGE_ERROR_INVALID_DATA;
		}

		packet_reply = packet_create_reply(packet, "i", ret);
		if (packet_reply) {
			service_common_unicast_packet(tcb, packet_reply);
			packet_destroy(packet_reply);
		}

		if (ret == BADGE_ERROR_NONE) {
			packet_service = packet_create("insert_badge", "is", ret, pkgname);
			if (packet_service != NULL) {
				service_common_multicast_packet(tcb, packet_service, TCB_CLIENT_TYPE_SERVICE);
				packet_destroy(packet_service);
			}
		}
	} else {
		ErrPrint("Failed to get data from the packet");
	}
}

static void _handler_delete_badge(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0;
	struct packet *packet_reply = NULL;
	struct packet *packet_service = NULL;
	char *pkgname = NULL;
	char *caller = NULL;

	if (packet_get(packet, "ss", &pkgname, &caller) == 2) {
		pkgname = get_string(pkgname);
		caller = get_string(caller);

		if (pkgname != NULL && caller != NULL) {
			ret = badge_db_delete(pkgname, caller);

		} else {
			ret = BADGE_ERROR_INVALID_DATA;
		}

		packet_reply = packet_create_reply(packet, "i", ret);
		if (packet_reply) {
			service_common_unicast_packet(tcb, packet_reply);
			packet_destroy(packet_reply);
		}

		if (ret == BADGE_ERROR_NONE) {
			packet_service = packet_create("delete_badge", "is", ret, pkgname);
			if (packet_service != NULL) {
				service_common_multicast_packet(tcb, packet_service, TCB_CLIENT_TYPE_SERVICE);
				packet_destroy(packet_service);
			}
		}
	} else {
		ErrPrint("Failed to get data from the packet");
	}
}

static void _handler_set_badge_count(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0;
	struct packet *packet_reply = NULL;
	struct packet *packet_service = NULL;
	char *pkgname = NULL;
	char *caller = NULL;
	int count = 0;

	if (packet_get(packet, "ssi", &pkgname, &caller, &count) == 3) {
		pkgname = get_string(pkgname);
		caller = get_string(caller);

		if (pkgname != NULL && caller != NULL) {
			ret = badge_db_set_count(pkgname, caller, count);

		} else {
			ret = BADGE_ERROR_INVALID_DATA;
		}

		packet_reply = packet_create_reply(packet, "i", ret);
		if (packet_reply) {
			service_common_unicast_packet(tcb, packet_reply);
			packet_destroy(packet_reply);
		}

		if (ret == BADGE_ERROR_NONE) {
			packet_service = packet_create("set_badge_count", "isi", ret, pkgname, count);
			if (packet_service != NULL) {
				service_common_multicast_packet(tcb, packet_service, TCB_CLIENT_TYPE_SERVICE);
				packet_destroy(packet_service);
			}
		}
	} else {
		ErrPrint("Failed to get data from the packet");
	}
}

static void _handler_set_display_option(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0;
	struct packet *packet_reply = NULL;
	struct packet *packet_service = NULL;
	char *pkgname = NULL;
	char *caller = NULL;
	int is_display = 0;

	if (packet_get(packet, "ssi", &pkgname, &caller, &is_display) == 3) {
		pkgname = get_string(pkgname);
		caller = get_string(caller);

		if (pkgname != NULL && caller != NULL) {
			ret = badge_db_set_display_option(pkgname, caller, is_display);

		} else {
			ret = BADGE_ERROR_INVALID_DATA;
		}

		packet_reply = packet_create_reply(packet, "i", ret);
		if (packet_reply) {
			service_common_unicast_packet(tcb, packet_reply);
			packet_destroy(packet_reply);
		}

		if (ret == BADGE_ERROR_NONE) {
			packet_service = packet_create("set_disp_option", "isi", ret, pkgname, is_display);
			if (packet_service != NULL) {
				service_common_multicast_packet(tcb, packet_service, TCB_CLIENT_TYPE_SERVICE);
				packet_destroy(packet_service);
			}
		}
	} else {
		ErrPrint("Failed to get data from the packet");
	}
}

static void _handler_service_register(struct tcb *tcb, struct packet *packet, void *data)
{
	struct packet *packet_reply;
	int ret;

	ret = tcb_client_type_set(tcb, TCB_CLIENT_TYPE_SERVICE);

	packet_reply = packet_create_reply(packet, "i", ret);
	if (packet_reply) {
		service_common_unicast_packet(tcb, packet_reply);
		packet_destroy(packet_reply);
	}
}

/*!
 * SERVICE THREAD
 */
static int service_thread_main(struct tcb *tcb, struct packet *packet, void *data)
{
	int i = 0;
	const char *command;
	static struct noti_service service_req_table[] = {
		{
			.cmd = "insert_badge",
			.handler = _handler_insert_badge,
		},
		{
			.cmd = "delete_badge",
			.handler = _handler_delete_badge,
		},
		{
			.cmd = "set_badge_count",
			.handler = _handler_set_badge_count,
		},
		{
			.cmd = "set_disp_option",
			.handler = _handler_set_display_option,
		},
		{
			.cmd = "service_register",
			.handler = _handler_service_register,
		},
		{
			.cmd = NULL,
			.handler = NULL,
		},
	};

	DbgPrint("TCB: %p, Packet: %p\n", tcb, packet);

	command = packet_command(packet);
	if (!command) {
		ErrPrint("Invalid command\n");
		return -EINVAL;
	}
	DbgPrint("Command: %s, Packet type[%d]\n", command, packet_type(packet));

	switch (packet_type(packet)) {
	case PACKET_REQ:
		/* Need to send reply packet */
		DbgPrint("REQ: Command: [%s]\n", command);

		for (i = 0; service_req_table[i].cmd; i++) {
			if (strcmp(service_req_table[i].cmd, command))
				continue;

			service_req_table[i].handler(tcb, packet, data);
			break;
		}

		break;
	case PACKET_REQ_NOACK:
		break;
	case PACKET_ACK:
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
HAPI int badge_service_init(void)
{
	if (s_info.svc_ctx) {
		ErrPrint("Already initialized\n");
		return LB_STATUS_ERROR_ALREADY;
	}

	s_info.svc_ctx = service_common_create(BADGE_ADDR, service_thread_main, NULL);
	if (!s_info.svc_ctx) {
		ErrPrint("Unable to activate service thread\n");
		return LB_STATUS_ERROR_FAULT;
	}

	DbgPrint("Successfully initiated\n");
	return LB_STATUS_SUCCESS;
}

HAPI int badge_service_fini(void)
{
	if (!s_info.svc_ctx)
		return LB_STATUS_ERROR_INVALID;

	service_common_destroy(s_info.svc_ctx);
	DbgPrint("Successfully Finalized\n");
	return LB_STATUS_SUCCESS;
}

/* End of a file */
