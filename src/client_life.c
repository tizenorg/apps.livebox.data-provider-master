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
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/smack.h>

#include <Eina.h>
#include <Ecore.h>

#include <dlog.h>
#include <packet.h>
#include <dynamicbox_errno.h>
#include <dynamicbox_service.h>

#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "debug.h"
#include "util.h"
#include "slave_life.h"
#include "xmonitor.h"
#include "conf.h"

int errno;

static struct {
	Eina_List *client_list;
	int nr_of_paused_clients;

	enum global_event_process {
		GLOBAL_EVENT_PROCESS_IDLE = 0x00,
		GLOBAL_EVENT_PROCESS_CREATE = 0x01,
		GLOBAL_EVENT_PROCESS_DESTROY = 0x02
	} in_event_process;

	Eina_List *create_event_list;
	Eina_List *destroy_event_list;

} s_info = {
	.client_list = NULL,
	.nr_of_paused_clients = 0,
	.in_event_process = GLOBAL_EVENT_PROCESS_IDLE,
	.create_event_list = NULL,
	.destroy_event_list = NULL,
};

struct subscribe_item {
	char *cluster;
	char *category;
};

struct global_event_item {
	void *cbdata;
	int (*cb)(struct client_node *client, void *data);
	int deleted;
};

struct event_item {
	void *data;
	int (*cb)(struct client_node *, void *);
	int deleted;
};

struct data_item {
	char *tag;
	void *data;
};

struct client_node {
	pid_t pid;
	int refcnt;

	int paused;

	enum client_event_process {
		CLIENT_EVENT_PROCESS_IDLE = 0x00,
		CLIENT_EVENT_PROCESS_DEACTIVATE = 0x01,
		CLIENT_EVENT_PROCESS_ACTIVATE = 0x02
	} in_event_process;
	Eina_List *event_deactivate_list;
	Eina_List *event_activate_list;

	Eina_List *data_list;
	Eina_List *subscribe_list;

	int faulted;
	char *direct_addr;
};

static inline void invoke_global_destroyed_cb(struct client_node *client)
{
	Eina_List *l;
	Eina_List *n;
	struct global_event_item *item;

	s_info.in_event_process |= GLOBAL_EVENT_PROCESS_DESTROY;
	EINA_LIST_FOREACH_SAFE(s_info.destroy_event_list, l, n, item) {
		/*!
		 * The first,
		 * item->deleted will be checked, so if it is deleted, remove item from the list
		 * The second, if the first routine takes false path,
		 * item->cb will be called, if it returns negative value, remove item from the list
		 * The third, if the second routine takes false path,
		 * Check the item->deleted again, so if it is turnned on, remove item from the list
		 */
		if (item->deleted || item->cb(client, item->cbdata) < 0 || item->deleted) {
			s_info.destroy_event_list = eina_list_remove(s_info.destroy_event_list, item);
			DbgFree(item);
		}
	}
	s_info.in_event_process &= ~GLOBAL_EVENT_PROCESS_DESTROY;
}

static inline void invoke_global_created_cb(struct client_node *client)
{
	Eina_List *l;
	Eina_List *n;
	struct global_event_item *item;

	s_info.in_event_process |= GLOBAL_EVENT_PROCESS_CREATE;
	EINA_LIST_FOREACH_SAFE(s_info.create_event_list, l, n, item) {
		/*!
		 * The first,
		 * item->deleted will be checked, so if it is deleted, remove item from the list
		 * The second, if the first routine takes false path,
		 * item->cb will be called, if it returns negative value, remove item from the list
		 * The third, if the second routine takes false path,
		 * Check the item->deleted again, so if it is turnned on, remove item from the list
		 */

		if (item->deleted || item->cb(client, item->cbdata) < 0 || item->deleted) {
			s_info.create_event_list = eina_list_remove(s_info.create_event_list, item);
			DbgFree(item);
		}
	}
	s_info.in_event_process &= ~GLOBAL_EVENT_PROCESS_CREATE;
}

static inline void invoke_deactivated_cb(struct client_node *client)
{
	struct event_item *item;
	Eina_List *l;
	Eina_List *n;

	client->in_event_process |= CLIENT_EVENT_PROCESS_DEACTIVATE;
	EINA_LIST_FOREACH_SAFE(client->event_deactivate_list, l, n, item) {
		/*!
		 * The first,
		 * item->deleted will be checked, so if it is deleted, remove item from the list
		 * The second, if the first routine takes false path,
		 * item->cb will be called, if it returns negative value, remove item from the list
		 * The third, if the second routine takes false path,
		 * Check the item->deleted again, so if it is turnned on, remove item from the list
		 */

		if (item->deleted || item->cb(client, item->data) < 0 || item->deleted) {
			client->event_deactivate_list = eina_list_remove(client->event_deactivate_list, item);
			DbgFree(item);
		}
	}
	client->in_event_process &= ~CLIENT_EVENT_PROCESS_DEACTIVATE;
}

static inline void invoke_activated_cb(struct client_node *client)
{
	struct event_item *item;
	Eina_List *l;
	Eina_List *n;

	client->in_event_process |= CLIENT_EVENT_PROCESS_ACTIVATE;
	EINA_LIST_FOREACH_SAFE(client->event_activate_list, l, n, item) {
		/*!
		 * The first,
		 * item->deleted will be checked, so if it is deleted, remove item from the list
		 * The second, if the first routine takes false path,
		 * item->cb will be called, if it returns negative value, remove item from the list
		 * The third, if the second routine takes false path,
		 * Check the item->deleted again, so if it is turnned on, remove item from the list
		 */

		if (item->deleted || item->cb(client, item->data) < 0 || item->deleted) {
			client->event_activate_list = eina_list_remove(client->event_activate_list, item);
			DbgFree(item);
		}
	}
	client->in_event_process &= ~CLIENT_EVENT_PROCESS_ACTIVATE;
}

static inline void destroy_client_data(struct client_node *client)
{
	struct event_item *event;
	struct data_item *data;
	struct subscribe_item *item;
	Ecore_Timer *timer;

	timer = client_del_data(client, "create,timer");
	if (timer) {
		ecore_timer_del(timer);
	}

	DbgPrint("Destroy client: %p\n", client);

	invoke_global_destroyed_cb(client);
	client_rpc_fini(client); /*!< Finalize the RPC after invoke destroy callbacks */

	EINA_LIST_FREE(client->data_list, data) {
		DbgPrint("Tag is not cleared (%s)\n", data->tag);
		DbgFree(data->tag);
		DbgFree(data);
	}

	EINA_LIST_FREE(client->event_deactivate_list, event) {
		DbgFree(event);
	}

	EINA_LIST_FREE(client->subscribe_list, item) {
		DbgFree(item->cluster);
		DbgFree(item->category);
		DbgFree(item);
	}

	if (client->paused) {
		s_info.nr_of_paused_clients--;
	}

	if (client->direct_addr) {
		(void)unlink(client->direct_addr);
		DbgFree(client->direct_addr);
	}

	s_info.client_list = eina_list_remove(s_info.client_list, client);
	DbgFree(client);

	/*!
	 * \note
	 * If there is any changes of clients,
	 * We should check the pause/resume states again.
	 */
	xmonitor_handle_state_changes();
}

static inline struct client_node *create_client_data(pid_t pid, const char *direct_addr)
{
	struct client_node *client;

	client = calloc(1, sizeof(*client));
	if (!client) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	client->pid = pid;
	client->refcnt = 1;

	if (direct_addr && direct_addr[0]) {
		client->direct_addr = strdup(direct_addr);
		if (!client->direct_addr) {
			ErrPrint("Failed to allocate direct_addr (%s)\n", direct_addr);
		}
	}

	s_info.client_list = eina_list_append(s_info.client_list, client);

	/*!
	 * \note
	 * Right after create a client ADT,
	 * We assume that the client is paused.
	 */
	client_paused(client);
	xmonitor_handle_state_changes();
	return client;
}

static Eina_Bool created_cb(void *data)
{
	(void)client_del_data(data, "create,timer");

	invoke_global_created_cb(data);
	invoke_activated_cb(data);
	/*!
	 * \note
	 * Client PAUSE/RESUME event must has to be sent after created event.
	 */
	xmonitor_update_state(client_pid(data));

	(void)client_unref(data);
	return ECORE_CALLBACK_CANCEL;
}

/*!
 * \note
 * Noramlly, client ADT is created when it send the "acquire" packet.
 * It means we have the handle for communicating with the client already,
 * So we just create its ADT in this function.
 * And invoke the global created event & activated event callbacks
 */
HAPI struct client_node *client_create(pid_t pid, int handle, const char *direct_addr)
{
	struct client_node *client;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (client) {
		ErrPrint("Client %d(%d) is already exists\n", pid, handle);
		return client;
	}

	client = create_client_data(pid, direct_addr);
	if (!client) {
		ErrPrint("Failed to create a new client (%d)\n", pid);
		return NULL;
	}

	ret = client_rpc_init(client, handle);
	if (ret < 0) {
		client = client_unref(client);
		ErrPrint("Failed to initialize the RPC for %d, Destroy client data %p(has to be 0x0)\n", pid, client);
	} else {
		Ecore_Timer *create_timer;
		/*!
		 * \note
		 * To save the time to send reply packet to the client.
		 */
		create_timer = ecore_timer_add(DELAY_TIME, created_cb, client_ref(client));
		if (create_timer == NULL) {
			ErrPrint("Failed to add a timer for client created event\n");
			client = client_unref(client); /* Decrease refcnt for argument */
			client = client_unref(client); /* Destroy client object */
			return NULL;
		}

		ret = client_set_data(client, "create,timer", create_timer);
		DbgPrint("Set data: %d\n", ret);
	}

	return client;
}

HAPI struct client_node *client_ref(struct client_node *client)
{
	if (!client) {
		return NULL;
	}

	client->refcnt++;
	return client;
}

HAPI struct client_node *client_unref(struct client_node *client)
{
	if (!client) {
		return NULL;
	}

	if (client->refcnt == 0) {
		ErrPrint("Client refcnt is not managed correctly\n");
		return NULL;
	}

	client->refcnt--;
	if (client->refcnt == 0) {
		destroy_client_data(client);
		client = NULL;
	}

	return client;
}

HAPI const int const client_refcnt(const struct client_node *client)
{
	return client->refcnt;
}

HAPI const pid_t const client_pid(const struct client_node *client)
{
	return client ? client->pid : (pid_t)-1;
}

HAPI struct client_node *client_find_by_pid(pid_t pid)
{
	Eina_List *l;
	struct client_node *client;

	EINA_LIST_FOREACH(s_info.client_list, l, client) {
		if (client->pid == pid) {
			return client;
		}
	}

	return NULL;
}

HAPI struct client_node *client_find_by_rpc_handle(int handle)
{
	Eina_List *l;
	struct client_node *client;

	if (handle <= 0) {
		ErrPrint("Invalid handle %d\n", handle);
		return NULL;
	}

	EINA_LIST_FOREACH(s_info.client_list, l, client) {
		if (client_rpc_handle(client) == handle) {
			return client;
		}
	}

	return NULL;
}

HAPI const int const client_count_paused(void)
{
	return s_info.nr_of_paused_clients;
}

HAPI int client_is_all_paused(void)
{
	DbgPrint("%d, %d\n", eina_list_count(s_info.client_list), s_info.nr_of_paused_clients);
	return eina_list_count(s_info.client_list) == s_info.nr_of_paused_clients;
}

HAPI int client_count(void)
{
	return eina_list_count(s_info.client_list);
}

HAPI struct client_node *client_deactivated_by_fault(struct client_node *client)
{
	if (!client || client->faulted) {
		return client;
	}

	ErrPrint("Client[%p] is faulted(%d), pid(%d)\n", client, client->refcnt, client->pid);
	client->faulted = 1;
	client->pid = (pid_t)-1;

	invoke_deactivated_cb(client);
	client = client_destroy(client);
	/*!
	 * \todo
	 * Who invokes this function has to care the reference counter of a client
	 * do I need to invoke the deactivated callback from here?
	 * client->pid = (pid_t)-1;
	 * slave_unref(client)
	 */
	return client;
}

HAPI const int const client_is_faulted(const struct client_node *client)
{
	/*!
	 * \note
	 * If the "client" is NIL, I assume that it is fault so returns TRUE(1)
	 */
	return client ? client->faulted : 1;
}

HAPI void client_reset_fault(struct client_node *client)
{
	if (client) {
		client->faulted = 0;
	}
}

HAPI int client_event_callback_add(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data)
{
	struct event_item *item;

	if (!cb) {
		ErrPrint("Invalid callback (cb == NULL)\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	item->cb = cb;
	item->data = data;
	item->deleted = 0;

	/*!
	 * \note
	 * Use the eina_list_prepend API.
	 * To keep the sequence of a callback invocation.
	 *
	 * Here is an example sequence.
	 *
	 * client_event_callback_add(CALLBACK_01);
	 * client_event_callback_add(CALLBACK_02);
	 * client_event_callback_add(CALLBACK_03);
	 *
	 * Then the invoke_event_callback function will call the CALLBACKS as below sequence
	 *
	 * invoke_CALLBACK_03
	 * invoke_CALLBACK_02
	 * invoke_CALLBACK_01
	 */

	switch (event) {
	case CLIENT_EVENT_DEACTIVATE:
		client->event_deactivate_list = eina_list_prepend(client->event_deactivate_list, item);
		break;
	case CLIENT_EVENT_ACTIVATE:
		client->event_activate_list = eina_list_prepend(client->event_activate_list, item);
		break;
	default:
		DbgFree(item);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int client_event_callback_del(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data)
{
	struct event_item *item;
	Eina_List *l;
	Eina_List *n;

	if (!cb) {
		ErrPrint("Invalid callback (cb == NULL)\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	switch (event) {
	case CLIENT_EVENT_DEACTIVATE:
		EINA_LIST_FOREACH_SAFE(client->event_deactivate_list, l, n, item) {
			if (item->cb == cb && item->data == data) {
				if (client->in_event_process & CLIENT_EVENT_PROCESS_DEACTIVATE) {
					item->deleted = 1;
				} else {
					client->event_deactivate_list = eina_list_remove(client->event_deactivate_list, item);
					DbgFree(item);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;

	case CLIENT_EVENT_ACTIVATE:
		EINA_LIST_FOREACH_SAFE(client->event_activate_list, l, n, item) {
			if (item->cb == cb && item->data == data) {
				if (client->in_event_process & CLIENT_EVENT_PROCESS_ACTIVATE) {
					item->deleted = 1;
				} else {
					client->event_activate_list = eina_list_remove(client->event_activate_list, item);
					DbgFree(item);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;

	default:
		ErrPrint("Invalid event\n");
		break;
	}

	return DBOX_STATUS_ERROR_NOT_EXIST;
}

HAPI int client_set_data(struct client_node *client, const char *tag, void *data)
{
	struct data_item *item;

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	item->tag = strdup(tag);
	if (!item->tag) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item);
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	item->data = data;

	client->data_list = eina_list_append(client->data_list, item);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI void *client_data(struct client_node *client, const char *tag)
{
	Eina_List *l;
	struct data_item *item;

	EINA_LIST_FOREACH(client->data_list, l, item) {
		if (!strcmp(item->tag, tag)) {
			return item->data;
		}
	}

	return NULL;
}

HAPI void *client_del_data(struct client_node *client, const char *tag)
{
	Eina_List *l;
	Eina_List *n;
	struct data_item *item;

	EINA_LIST_FOREACH_SAFE(client->data_list, l, n, item) {
		if (!strcmp(item->tag, tag)) {
			void *data;
			client->data_list = eina_list_remove(client->data_list, item);
			data = item->data;
			DbgFree(item->tag);
			DbgFree(item);
			return data;
		}
	}

	return NULL;
}

HAPI void client_paused(struct client_node *client)
{
	if (client->paused) {
		return;
	}

	client->paused = 1;
	s_info.nr_of_paused_clients++;
}

HAPI void client_resumed(struct client_node *client)
{
	if (client->paused == 0) {
		return;
	}

	client->paused = 0;
	s_info.nr_of_paused_clients--;
}

HAPI int client_init(void)
{
	return DBOX_STATUS_ERROR_NONE;
}

HAPI void client_fini(void)
{
	struct global_event_item *handler;
	struct client_node *client;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(s_info.client_list, l, n, client) {
		(void)client_destroy(client);
	}

	EINA_LIST_FREE(s_info.create_event_list, handler) {
		DbgFree(handler);
	}

	EINA_LIST_FREE(s_info.destroy_event_list, handler) {
		DbgFree(handler);
	}
}

HAPI int client_global_event_handler_add(enum client_global_event event_type, int (*cb)(struct client_node *client, void *data), void *data)
{
	struct global_event_item *handler;

	handler = malloc(sizeof(*handler));
	if (!handler) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	handler->cbdata = data;
	handler->cb = cb;
	handler->deleted = 0;

	switch (event_type) {
	case CLIENT_GLOBAL_EVENT_CREATE:
		s_info.create_event_list = eina_list_prepend(s_info.create_event_list, handler);
		break;
	case CLIENT_GLOBAL_EVENT_DESTROY:
		s_info.destroy_event_list = eina_list_prepend(s_info.destroy_event_list, handler);
		break;
	default:
		DbgFree(handler);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int client_global_event_handler_del(enum client_global_event event_type, int (*cb)(struct client_node *, void *), void *data)
{
	Eina_List *l;
	Eina_List *n;
	struct global_event_item *item;

	switch (event_type) {
	case CLIENT_GLOBAL_EVENT_CREATE:
		EINA_LIST_FOREACH_SAFE(s_info.create_event_list, l, n, item) {
			if (item->cb == cb && item->cbdata == data) {
				if (s_info.in_event_process & GLOBAL_EVENT_PROCESS_CREATE) {
					item->deleted = 1;
				} else {
					s_info.create_event_list = eina_list_remove(s_info.create_event_list, item);
					DbgFree(item);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;
	case CLIENT_GLOBAL_EVENT_DESTROY:
		EINA_LIST_FOREACH_SAFE(s_info.destroy_event_list, l, n, item) {
			if (item->cb == cb && item->cbdata == data) {
				if (s_info.in_event_process & GLOBAL_EVENT_PROCESS_DESTROY) {
					item->deleted = 1;
				} else {
					s_info.destroy_event_list = eina_list_remove(s_info.destroy_event_list, item);
					DbgFree(item);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;
	default:
		break;
	}

	return DBOX_STATUS_ERROR_NOT_EXIST;
}

HAPI int client_subscribe(struct client_node *client, const char *cluster, const char *category)
{
	struct subscribe_item *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	item->cluster = strdup(cluster);
	if (!item->cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item);
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	item->category = strdup(category);
	if (!item->category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item->cluster);
		DbgFree(item);
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	client->subscribe_list = eina_list_append(client->subscribe_list, item);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int client_unsubscribe(struct client_node *client, const char *cluster, const char *category)
{
	struct subscribe_item *item;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(client->subscribe_list, l, n, item) {
		if (!strcasecmp(cluster, item->cluster) && !strcasecmp(category, item->category)) {
			client->subscribe_list = eina_list_remove(client->subscribe_list, item);
			DbgFree(item->cluster);
			DbgFree(item->category);
			DbgFree(item);
			return DBOX_STATUS_ERROR_NONE;
		}
	}

	return DBOX_STATUS_ERROR_NOT_EXIST;
}

HAPI int client_is_subscribed(struct client_node *client, const char *cluster, const char *category)
{
	struct subscribe_item *item;
	Eina_List *l;

	EINA_LIST_FOREACH(client->subscribe_list, l, item) {
		if (!strcmp(item->cluster, "*")) {
			return 1;
		}

		if (strcasecmp(item->cluster, cluster)) {
			continue;
		}

		if (!strcmp(item->category, "*")) {
			return 1;
		}

		if (!strcasecmp(item->category, category)) {
			return 1;
		}
	}

	return 0;
}

HAPI int client_browse_list(const char *cluster, const char *category, int (*cb)(struct client_node *client, void *data), void *data)
{
	Eina_List *l;
	struct client_node *client;
	int cnt;

	if (!cb || !cluster || !category) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	cnt = 0;
	EINA_LIST_FOREACH(s_info.client_list, l, client) {
		if (!client_is_subscribed(client, cluster, category)) {
			continue;
		}

		if (cb(client, data) < 0) {
			return DBOX_STATUS_ERROR_CANCEL;
		}

		cnt++;
	}

	return cnt;
}

HAPI int client_nr_of_subscriber(const char *cluster, const char *category)
{
	Eina_List *l;
	struct client_node *client;
	int cnt;

	cnt = 0;
	EINA_LIST_FOREACH(s_info.client_list, l, client) {
		cnt += !!client_is_subscribed(client, cluster, category);
	}

	return cnt;
}

HAPI int client_broadcast(struct inst_info *inst, struct packet *packet)
{
	Eina_List *l;
	Eina_List *list;
	struct client_node *client;

	list = inst ? instance_client_list(inst) : s_info.client_list;
	EINA_LIST_FOREACH(list, l, client) {
		if (client_pid(client) == -1) {
			ErrPrint("Client[%p] has PID[%d]\n", client, client_pid(client));
			continue;
		}

		(void)client_rpc_async_request(client, packet_ref(packet));
	}

	packet_unref(packet);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI const char *client_direct_addr(const struct client_node *client)
{
	return client ? client->direct_addr : NULL;
}

/* End of a file */
