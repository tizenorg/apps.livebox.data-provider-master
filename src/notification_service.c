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
#if defined(HAVE_LIVEBOX)
#include <dynamicbox_errno.h>
#else
#include "lite-errno.h"
#endif
#include <packet.h>

#include <sys/smack.h>
#include <security-server.h>

#include <vconf.h>
#include <notification.h>
#include <notification_internal.h>
#include <notification_ipc.h>
#include <notification_noti.h>
#include <notification_setting_service.h>

#include "service_common.h"
#include "debug.h"
#include "util.h"
#include "conf.h"

#ifndef NOTIFICATION_DEL_PACKET_UNIT
#define NOTIFICATION_DEL_PACKET_UNIT 10
#endif

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
	const char *rule;
	const char *access;
	void (*handler_access_error)(struct tcb *tcb, struct packet *packet);
};

static inline char *_string_get(char *string)
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
 * FUNCTIONS to create packets
 */
static inline int _priv_id_get_from_list(int num_data, int *list, int index) {
	if (index < num_data) {
		return *(list + index);
	} else {
		return -1;
	}
}

static inline struct packet *_packet_create_with_list(int op_num, int *list, int start_index) {
	return packet_create(
			"del_noti_multiple",
			"iiiiiiiiiii",
			((op_num - start_index) > NOTIFICATION_DEL_PACKET_UNIT) ? NOTIFICATION_DEL_PACKET_UNIT : op_num - start_index,
			_priv_id_get_from_list(op_num, list, start_index),
			_priv_id_get_from_list(op_num, list, start_index + 1),
			_priv_id_get_from_list(op_num, list, start_index + 2),
			_priv_id_get_from_list(op_num, list, start_index + 3),
			_priv_id_get_from_list(op_num, list, start_index + 4),
			_priv_id_get_from_list(op_num, list, start_index + 5),
			_priv_id_get_from_list(op_num, list, start_index + 6),
			_priv_id_get_from_list(op_num, list, start_index + 7),
			_priv_id_get_from_list(op_num, list, start_index + 8),
			_priv_id_get_from_list(op_num, list, start_index + 9)
			);
}

/*!
 * SERVICE HANDLER
 */
static void _handler_insert_noti(struct tcb *tcb, struct packet *packet, notification_h noti, void *data)
{
	int ret = 0, ret_p = 0;
	int priv_id = 0;
	struct packet *packet_reply = NULL;
	struct packet *packet_service = NULL;

	ret = notification_noti_insert(noti);
	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]\n", priv_id);
	packet_reply = packet_create_reply(packet, "ii", ret, priv_id);
	if (packet_reply) {
		if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
			ErrPrint("failed to send reply packet: %d\n", ret_p);
		}
		packet_destroy(packet_reply);
	} else {
		ErrPrint("failed to create a reply packet\n");
	}

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to insert a notification: %d\n", ret);
		return ;
	}

	packet_service = notification_ipc_make_packet_from_noti(noti, "add_noti", 2);
	if (packet_service != NULL) {
		if ((ret_p = service_common_multicast_packet(tcb, packet_service, TCB_CLIENT_TYPE_SERVICE)) < 0) {
			ErrPrint("failed to send a multicast packet: %d\n", ret_p);
		}
		packet_destroy(packet_service);
	} else {
		ErrPrint("failed to create a multicats packet\n");
	}
}

/*static void _handler_insert(struct tcb *tcb, struct packet *packet, void *data) // not used
  {
  notification_h noti = NULL;

  noti = notification_create(NOTIFICATION_TYPE_NOTI);
  if (noti != NULL) {
  if (notification_ipc_make_noti_from_packet(noti, packet) == NOTIFICATION_ERROR_NONE) {
  _handler_insert_noti(tcb, packet, noti, data);
  } else {
  ErrPrint("Failed to create the packet");
  }
  notification_free(noti);
  }
  }*/

static void _handler_update_noti(struct tcb *tcb, struct packet *packet, notification_h noti, void *data)
{
	int ret = 0, ret_p = 0;
	int priv_id = 0;
	struct packet *packet_reply = NULL;
	struct packet *packet_service = NULL;

	ret = notification_noti_update(noti);

	notification_get_id(noti, NULL, &priv_id);
	DbgPrint("priv_id: [%d]\n", priv_id);
	packet_reply = packet_create_reply(packet, "ii", ret, priv_id);
	if (packet_reply) {
		if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
			ErrPrint("failed to send reply packet:%d\n", ret_p);
		}
		packet_destroy(packet_reply);
	} else {
		ErrPrint("failed to create a reply packet\n");
	}

	if (ret != NOTIFICATION_ERROR_NONE) {
		ErrPrint("failed to update a notification:%d\n", ret);
		return ;
	}

	packet_service = notification_ipc_make_packet_from_noti(noti, "update_noti", 2);
	if (packet_service != NULL) {
		if ((ret_p = service_common_multicast_packet(tcb, packet_service, TCB_CLIENT_TYPE_SERVICE)) < 0) {
			ErrPrint("failed to send a multicast packet: %d\n", ret_p);
		}
		packet_destroy(packet_service);
	}
}

static void _handler_update(struct tcb *tcb, struct packet *packet, void *data)
{
	notification_h noti = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		if (notification_ipc_make_noti_from_packet(noti, packet) == NOTIFICATION_ERROR_NONE) {
			_handler_update_noti(tcb, packet, noti, data);
		} else {
			ErrPrint("Failed to create the packet");
		}
		notification_free(noti);
	}
}

static void _handler_check_noti_by_tag(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0;
	notification_h noti = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		if (notification_ipc_make_noti_from_packet(noti, packet) == NOTIFICATION_ERROR_NONE) {
			ret = notification_noti_check_tag(noti);
			if (ret == NOTIFICATION_ERROR_NOT_EXIST_ID) {
				_handler_insert_noti(tcb, packet, noti, data);
			} else if (ret == NOTIFICATION_ERROR_ALREADY_EXIST_ID) {
				_handler_update_noti(tcb, packet, noti, data);
			}
		}
		notification_free(noti);
	}
}

static void _handler_load_noti_by_tag(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0, ret_p = 0;
	char* tag;
	char* pkgname;
	struct packet *packet_reply = NULL;
	notification_h noti = NULL;

	noti = notification_create(NOTIFICATION_TYPE_NOTI);
	if (noti != NULL) {
		if (packet_get(packet, "ss", &pkgname, &tag) == 2) {
			ret = notification_noti_get_by_tag(noti, pkgname, tag);
			packet_reply = notification_ipc_make_reply_packet_from_noti(noti, packet);
			if (packet_reply) {
				if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
					ErrPrint("failed to send reply packet: %d\n", ret_p);
				}
				packet_destroy(packet_reply);
			} else {
				ErrPrint("failed to create a reply packet\n");
			}

			if (ret != NOTIFICATION_ERROR_NONE) {
				ErrPrint("failed to load_noti_by_tag : %d\n", ret);
				notification_free(noti);
				return ;
			}
		} else {
			ErrPrint("Failed to create the packet");
		}
		notification_free(noti);
	}
}


static void _handler_refresh(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0;
	struct packet *packet_reply = NULL;

	packet_reply = packet_create_reply(packet, "i", ret);
	if (packet_reply) {
		if ((ret = service_common_unicast_packet(tcb, packet_reply)) < 0) {
			ErrPrint("failed to send reply packet:%d\n", ret);
		}
		packet_destroy(packet_reply);
	} else {
		ErrPrint("failed to create a reply packet\n");
	}

	if ((ret = service_common_multicast_packet(tcb, packet, TCB_CLIENT_TYPE_SERVICE)) < 0) {
		ErrPrint("failed to send a multicast packet:%d\n", ret);
	}
}

static void _handler_delete_single(struct tcb *tcb, struct packet *packet, void *data)
{
	int num_changes = 0;
	int ret = 0, ret_p = 0;
	int priv_id = 0;
	struct packet *packet_reply = NULL;
	struct packet *packet_service = NULL;
	char *pkgname = NULL;

	if (packet_get(packet, "si", &pkgname, &priv_id) == 2) {
		pkgname = _string_get(pkgname);

		ret = notification_noti_delete_by_priv_id_get_changes(pkgname, priv_id, &num_changes);

		DbgPrint("priv_id: [%d] num_delete:%d\n", priv_id, num_changes);
		packet_reply = packet_create_reply(packet, "ii", ret, priv_id);
		if (packet_reply) {
			if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
				ErrPrint("failed to send reply packet:%d\n", ret_p);
			}
			packet_destroy(packet_reply);
		} else {
			ErrPrint("failed to create a reply packet\n");
		}

		if (ret != NOTIFICATION_ERROR_NONE || num_changes <= 0) {
			ErrPrint("failed to delete a notification:%d %d\n", ret, num_changes);
			return ;
		}

		packet_service = packet_create("del_noti_single", "ii", 1, priv_id);
		if (packet_service != NULL) {
			if ((ret_p = service_common_multicast_packet(tcb, packet_service, TCB_CLIENT_TYPE_SERVICE)) < 0) {
				ErrPrint("failed to send a multicast packet: %d\n", ret_p);
			}
			packet_destroy(packet_service);
		}
	} else {
		ErrPrint("Failed to get data from the packet");
	}
}

static void _handler_delete_multiple(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0, ret_p = 0;
	struct packet *packet_reply = NULL;
	struct packet *packet_service = NULL;
	char *pkgname = NULL;
	notification_type_e type = 0;
	int num_deleted = 0;
	int *list_deleted = NULL;

	if (packet_get(packet, "si", &pkgname, &type) == 2) {
		pkgname = _string_get(pkgname);
		DbgPrint("pkgname: [%s] type: [%d]\n", pkgname, type);

		ret = notification_noti_delete_all(type, pkgname, &num_deleted, &list_deleted);
		DbgPrint("ret: [%d] num_deleted: [%d]\n", ret, num_deleted);

		packet_reply = packet_create_reply(packet, "ii", ret, num_deleted);
		if (packet_reply) {
			if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
				ErrPrint("failed to send reply packet:%d\n", ret_p);
			}
			packet_destroy(packet_reply);
		} else {
			ErrPrint("failed to create a reply packet\n");
		}

		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("failed to delete notifications:%d\n", ret);
			if (list_deleted != NULL) {
				DbgFree(list_deleted);
			}
			return ;
		}

		if (num_deleted > 0) {
			if (num_deleted <= NOTIFICATION_DEL_PACKET_UNIT) {
				packet_service = _packet_create_with_list(num_deleted, list_deleted, 0);

				if (packet_service) {
					if ((ret_p = service_common_multicast_packet(tcb, packet_service, TCB_CLIENT_TYPE_SERVICE)) < 0) {
						ErrPrint("failed to send a multicast packet: %d\n", ret_p);
					}
					packet_destroy(packet_service);
				} else {
					ErrPrint("failed to create a multicast packet\n");
				}
			} else {
				int set = 0;
				int set_total = num_deleted / NOTIFICATION_DEL_PACKET_UNIT;

				for (set = 0; set <= set_total; set++) {
					packet_service = _packet_create_with_list(num_deleted,
							list_deleted, set * NOTIFICATION_DEL_PACKET_UNIT);

					if (packet_service) {
						if ((ret_p = service_common_multicast_packet(tcb, packet_service, TCB_CLIENT_TYPE_SERVICE)) < 0) {
							ErrPrint("failed to send a multicast packet:%d\n", ret_p);
						}
						packet_destroy(packet_service);
					} else {
						ErrPrint("failed to create a multicast packet\n");
					}
				}
			}
		}

		if (list_deleted != NULL) {
			DbgFree(list_deleted);
			list_deleted = NULL;
		}
	} else {
		ErrPrint("Failed to get data from the packet");
	}
}

static void _handler_noti_property_set(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0, ret_p = 0;
	struct packet *packet_reply = NULL;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;

	if (packet_get(packet, "sss", &pkgname, &property, &value) == 3) {
		pkgname = _string_get(pkgname);
		property = _string_get(property);
		value = _string_get(value);

		ret = notification_setting_db_set(pkgname, property, value);

		packet_reply = packet_create_reply(packet, "ii", ret, ret);
		if (packet_reply) {
			if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
				ErrPrint("failed to send reply packet:%d\n", ret_p);
			}
			packet_destroy(packet_reply);
		} else {
			ErrPrint("failed to create a reply packet\n");
		}

		if (ret != NOTIFICATION_ERROR_NONE) {
			ErrPrint("failed to set noti property:%d\n", ret);
		}
	} else {
		ErrPrint("Failed to get data from the packet");
	}
}

static void _handler_noti_property_get(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0, ret_p = 0;
	struct packet *packet_reply = NULL;
	char *pkgname = NULL;
	char *property = NULL;
	char *value = NULL;

	if (packet_get(packet, "sss", &pkgname, &property) == 2) {
		pkgname = _string_get(pkgname);
		property = _string_get(property);

		ret = notification_setting_db_get(pkgname, property, &value);

		packet_reply = packet_create_reply(packet, "is", ret, value);
		if (packet_reply) {
			if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
				ErrPrint("failed to send reply packet:%d\n", ret_p);
			}
			packet_destroy(packet_reply);
		} else {
			ErrPrint("failed to create a reply packet\n");
		}

		if (value != NULL) {
			DbgFree(value);
		}
	}
}

static void _handler_service_register(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0;
	struct packet *packet_reply;

	ret = tcb_client_type_set(tcb, TCB_CLIENT_TYPE_SERVICE);

	packet_reply = packet_create_reply(packet, "i", ret);
	if (packet_reply) {
		if ((ret = service_common_unicast_packet(tcb, packet_reply)) < 0) {
			ErrPrint("failed to send reply packet:%d\n", ret);
		}
		packet_destroy(packet_reply);
	} else {
		ErrPrint("failed to create a reply packet\n");
	}
}

static void _handler_post_toast_message(struct tcb *tcb, struct packet *packet, void *data)
{
	int ret = 0;
	struct packet *packet_reply = NULL;

	packet_reply = packet_create_reply(packet, "i", ret);
	if (packet_reply) {
		if ((ret = service_common_unicast_packet(tcb, packet_reply)) < 0) {
			ErrPrint("failed to send reply packet:%d\n", ret);
		}
		packet_destroy(packet_reply);
	} else {
		ErrPrint("failed to create a reply packet\n");
	}

	if ((ret = service_common_multicast_packet(tcb, packet, TCB_CLIENT_TYPE_SERVICE)) < 0) {
		ErrPrint("failed to send a multicast packet:%d\n", ret);
	}

}

/*!
 * SERVICE PERMISSION CHECK
 */
static void _permission_check_common(struct tcb *tcb, struct packet *packet)
{
	int ret_p = 0;
	struct packet *packet_reply = NULL;

	packet_reply = packet_create_reply(packet, "ii", NOTIFICATION_ERROR_PERMISSION_DENIED, 0);
	if (packet_reply) {
		if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
			ErrPrint("Failed to send a reply packet:%d", ret_p);
		}
		packet_destroy(packet_reply);
	} else {
		ErrPrint("Failed to create a reply packet");
	}
}

static void _permission_check_refresh(struct tcb *tcb, struct packet *packet)
{
	int ret_p = 0;
	struct packet *packet_reply = NULL;

	packet_reply = packet_create_reply(packet, "i", NOTIFICATION_ERROR_PERMISSION_DENIED);
	if (packet_reply) {
		if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
			ErrPrint("Failed to send a reply packet:%d", ret_p);
		}
		packet_destroy(packet_reply);
	} else {
		ErrPrint("Failed to create a reply packet");
	}
}

static void _permission_check_property_get(struct tcb *tcb, struct packet *packet)
{
	int ret_p = 0;
	struct packet *packet_reply = NULL;

	packet_reply = packet_create_reply(packet, "is", NOTIFICATION_ERROR_PERMISSION_DENIED, NULL);
	if (packet_reply) {
		if ((ret_p = service_common_unicast_packet(tcb, packet_reply)) < 0) {
			ErrPrint("Failed to send a reply packet:%d", ret_p);
		}
		packet_destroy(packet_reply);
	} else {
		ErrPrint("Failed to create a reply packet");
	}
}

static int _persmission_check(int fd, struct noti_service *service)
{
	int ret;

	if (service->rule != NULL && service->access != NULL) {
		ret = security_server_check_privilege_by_sockfd(fd, service->rule, service->access);
		if (ret == SECURITY_SERVER_API_ERROR_ACCESS_DENIED) {
			ErrPrint("SMACK:Access denied\n");
			return 0;
		}
	}

	return 1;
}

/*!
 * NOTIFICATION SERVICE INITIALIZATION
 */
static void _notification_data_init(void)
{
	int property = 0;
	int priv_id = 0;
	char *noti_pkgname = NULL;
	notification_h noti = NULL;
	notification_list_h noti_list = NULL;
	notification_list_h noti_list_head = NULL;
	notification_type_e noti_type = NOTIFICATION_TYPE_NONE;

	notification_get_list(NOTIFICATION_TYPE_NONE, -1, &noti_list);
	noti_list_head = noti_list;

	while (noti_list != NULL) {
		noti = notification_list_get_data(noti_list);
		if (noti) {
			notification_get_id(noti, NULL, &priv_id);
			notification_get_pkgname(noti, &noti_pkgname);
			notification_get_property(noti, &property);
			notification_get_type(noti, &noti_type);

			if (noti_type == NOTIFICATION_TYPE_ONGOING
					|| property & NOTIFICATION_PROP_VOLATILE_DISPLAY) {
				notification_noti_delete_by_priv_id(noti_pkgname, priv_id);
			}
		}
		noti_list = notification_list_get_next(noti_list);
	}

	if (noti_list_head != NULL) {
		notification_free_list(noti_list_head);
	}
}

static void _notification_init(void) {
	int ret = -1;
	int restart_count = 0;

	ret = vconf_get_int(VCONFKEY_MASTER_RESTART_COUNT, &restart_count);
	if (ret == 0 && restart_count <= 1) {
		_notification_data_init();
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
			.cmd = "add_noti",
			.handler = _handler_check_noti_by_tag,
			.rule = "data-provider-master::notification.client",
			.access = "w",
			.handler_access_error = _permission_check_common,
		},
		{
			.cmd = "update_noti",
			.handler = _handler_update,
			.rule = "data-provider-master::notification.client",
			.access = "w",
			.handler_access_error = _permission_check_common,
		},
		{
			.cmd = "load_noti_by_tag",
			.handler = _handler_load_noti_by_tag,
			.rule = "data-provider-master::notification.client",
			.access = "r",
			.handler_access_error = _permission_check_common,
		},
		{
			.cmd = "refresh_noti",
			.handler = _handler_refresh,
			.rule = "data-provider-master::notification.client",
			.access = "w",
			.handler_access_error = _permission_check_refresh,
		},
		{
			.cmd = "del_noti_single",
			.handler = _handler_delete_single,
			.rule = "data-provider-master::notification.client",
			.access = "w",
			.handler_access_error = _permission_check_common,
		},
		{
			.cmd = "del_noti_multiple",
			.handler = _handler_delete_multiple,
			.rule = "data-provider-master::notification.client",
			.access = "w",
			.handler_access_error = _permission_check_common,
		},
		{
			.cmd = "set_noti_property",
			.handler = _handler_noti_property_set,
			.rule = "data-provider-master::notification.client",
			.access = "w",
			.handler_access_error = _permission_check_common,
		},
		{
			.cmd = "get_noti_property",
			.handler = _handler_noti_property_get,
			.rule = "data-provider-master::notification.client",
			.access = "r",
			.handler_access_error = _permission_check_property_get,
		},
		{
			.cmd = "service_register",
			.handler = _handler_service_register,
			.rule = NULL,
			.access = NULL,
			.handler_access_error = NULL,
		},
		{
			.cmd = "post_toast",
			.handler = _handler_post_toast_message,
			.rule = NULL,
			.access = NULL,
			.handler_access_error = NULL,
		},
		{
			.cmd = NULL,
			.handler = NULL,
			.rule = NULL,
			.access = NULL,
			.handler_access_error = NULL,
		},
	};

	if (!packet) {
		DbgPrint("TCB: %p is terminated\n", tcb);
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

		for (i = 0; service_req_table[i].cmd; i++) {
			if (strcmp(service_req_table[i].cmd, command)) {
				continue;
			}

			if (_persmission_check(tcb_fd(tcb), &(service_req_table[i])) == 1) {
				service_req_table[i].handler(tcb, packet, data);
			} else {
				if (service_req_table[i].handler_access_error != NULL) {
					service_req_table[i].handler_access_error(tcb, packet);
				}
			}
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
HAPI int notification_service_init(void)
{
	if (s_info.svc_ctx) {
		ErrPrint("Already initialized\n");
		return DBOX_STATUS_ERROR_ALREADY;
	}

	_notification_init();

	s_info.svc_ctx = service_common_create(NOTIFICATION_SOCKET, service_thread_main, NULL);
	if (!s_info.svc_ctx) {
		ErrPrint("Unable to activate service thread\n");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (smack_fsetlabel(service_common_fd(s_info.svc_ctx), NOTIFICATION_SMACK_LABEL, SMACK_LABEL_IPOUT) != 0) {
		if (errno != EOPNOTSUPP) {
			ErrPrint("Unable to set SMACK label(%d)\n", errno);
			service_common_destroy(s_info.svc_ctx);
			s_info.svc_ctx = NULL;
			return DBOX_STATUS_ERROR_FAULT;
		}
	}

	if (smack_fsetlabel(service_common_fd(s_info.svc_ctx), NOTIFICATION_SMACK_LABEL, SMACK_LABEL_IPIN) != 0) {
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

HAPI int notification_service_fini(void)
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
