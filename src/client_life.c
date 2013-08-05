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

#include <Eina.h>
#include <Ecore.h>

#include <dlog.h>
#include <packet.h>
#include <livebox-errno.h>

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

	Eina_List *create_event_list;
	Eina_List *destroy_event_list;
} s_info = {
	.client_list = NULL,
	.nr_of_paused_clients = 0,
	.create_event_list = NULL,
	.destroy_event_list = NULL,
};

struct subscribe_item {
	char *cluster;
	char *category;
};

struct global_event_handler {
	void *cbdata;
	int (*cb)(struct client_node *client, void *data);
};

struct data_item {
	char *tag;
	void *data;
};

struct event_item {
	void *data;
	int (*cb)(struct client_node *, void *);
};

struct client_node {
	pid_t pid;
	int refcnt;

	int paused;

	Eina_List *event_deactivate_list;
	Eina_List *event_activate_list;
	Eina_List *data_list;
	Eina_List *subscribe_list;

	int faulted;
};

static inline void invoke_global_destroyed_cb(struct client_node *client)
{
	Eina_List *l;
	Eina_List *n;
	struct global_event_handler *item;

	EINA_LIST_FOREACH_SAFE(s_info.destroy_event_list, l, n, item) {
		if (!item->cb) {
			ErrPrint("Callback function is not valid\n");
			continue;
		}

		if (item->cb(client, item->cbdata) < 0) {
			if (eina_list_data_find(s_info.destroy_event_list, item)) {
				s_info.destroy_event_list = eina_list_remove(s_info.destroy_event_list, item);
				DbgFree(item);
			}
		}
	}
}

static inline void invoke_global_created_cb(struct client_node *client)
{
	Eina_List *l;
	Eina_List *n;
	struct global_event_handler *item;

	EINA_LIST_FOREACH_SAFE(s_info.create_event_list, l, n, item) {
		if (!item->cb) {
			ErrPrint("Callback function is not valid\n");
			continue;
		}

		if (item->cb(client, item->cbdata) < 0) {
			if (eina_list_data_find(s_info.create_event_list, item)) {
				s_info.create_event_list = eina_list_remove(s_info.create_event_list, item);
				DbgFree(item);
			}
		}
	}
}

static inline void invoke_deactivated_cb(struct client_node *client)
{
	struct event_item *item;
	Eina_List *l;
	Eina_List *n;
	int ret;

	client_ref(client); /*!< Prevent from client deletion in the callbacks */
	EINA_LIST_FOREACH_SAFE(client->event_deactivate_list, l, n, item) {
		ret = item->cb(client, item->data);
		if (ret < 0) {
			if (eina_list_data_find(client->event_deactivate_list, item)) {
				client->event_deactivate_list = eina_list_remove(client->event_deactivate_list, item);
				DbgFree(item);
			}
		}
	}
	client_unref(client);
}

static inline void invoke_activated_cb(struct client_node *client)
{
	struct event_item *item;
	Eina_List *l;
	Eina_List *n;
	int ret;

	client_ref(client); /*!< Prevent from client deletion in the callbacks */
	EINA_LIST_FOREACH_SAFE(client->event_activate_list, l, n, item) {
		ret = item->cb(client, item->data);
		if (ret < 0) {
			if (eina_list_data_find(client->event_activate_list, item)) {
				client->event_activate_list = eina_list_remove(client->event_activate_list, item);
				DbgFree(item);
			}
		}
	}
	client_unref(client);
}

static inline void destroy_client_data(struct client_node *client)
{
	struct event_item *event;
	struct data_item *data;
	struct subscribe_item *item;

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

	if (client->paused)
		s_info.nr_of_paused_clients--;

	s_info.client_list = eina_list_remove(s_info.client_list, client);
	DbgFree(client);

	/*!
	 * \note
	 * If there is any changes of clients,
	 * We should check the pause/resume states again.
	 */
	xmonitor_handle_state_changes();
}

static inline struct client_node *create_client_data(pid_t pid)
{
	struct client_node *client;

	client = calloc(1, sizeof(*client));
	if (!client) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	client->pid = pid;
	client->refcnt = 1;

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
	invoke_global_created_cb(data);
	invoke_activated_cb(data);
	/*!
	 * \note
	 * Client PAUSE/RESUME event must has to be sent after created event.
	 */
	xmonitor_update_state(client_pid(data));
	return ECORE_CALLBACK_CANCEL;
}

/*!
 * \note
 * Noramlly, client ADT is created when it send the "acquire" packet.
 * It means we have the handle for communicating with the client already,
 * So we just create its ADT in this function.
 * And invoke the global created event & activated event callbacks
 */
HAPI struct client_node *client_create(pid_t pid, int handle)
{
	struct client_node *client;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (client) {
		ErrPrint("Client %d(%d) is already exists\n", pid, handle);
		return client;
	}

	client = create_client_data(pid);
	if (!client) {
		ErrPrint("Failed to create a new client (%d)\n", pid);
		return NULL;
	}

	ret = client_rpc_init(client, handle);
	if (ret < 0) {
		client = client_unref(client);
		ErrPrint("Failed to initialize the RPC for %d, Destroy client data %p(has to be 0x0)\n", pid, client);
	} else {
		/*!
		 * \note
		 * To save the time to send reply packet to the client.
		 */
		if (ecore_timer_add(DELAY_TIME, created_cb, client) == NULL) {
			ErrPrint("Failed to add a timer for client created event\n");
			client = client_unref(client);
			return NULL;
		}
	}

	return client;
}

HAPI struct client_node *client_ref(struct client_node *client)
{
	if (!client)
		return NULL;

	client->refcnt++;
	return client;
}

HAPI struct client_node *client_unref(struct client_node *client)
{
	if (!client)
		return NULL;

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
		if (client->pid == pid)
			return client;
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
		if (client_rpc_handle(client) == handle)
			return client;
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
	if (!client || client->faulted)
		return client;

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
	if (client)
		client->faulted = 0;
}

HAPI int client_event_callback_add(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data)
{
	struct event_item *item;

	if (!cb) {
		ErrPrint("Invalid callback (cb == NULL)\n");
		return LB_STATUS_ERROR_INVALID;
	}

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	item->cb = cb;
	item->data = data;

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
		return LB_STATUS_ERROR_INVALID;
	}

	return LB_STATUS_SUCCESS;
}

HAPI int client_event_callback_del(struct client_node *client, enum client_event event, int (*cb)(struct client_node *, void *), void *data)
{
	struct event_item *item;
	Eina_List *l;
	Eina_List *n;

	if (!cb) {
		ErrPrint("Invalid callback (cb == NULL)\n");
		return LB_STATUS_ERROR_INVALID;
	}

	switch (event) {
	case CLIENT_EVENT_DEACTIVATE:
		EINA_LIST_FOREACH_SAFE(client->event_deactivate_list, l, n, item) {
			if (item->cb == cb && item->data == data) {
				client->event_deactivate_list = eina_list_remove(client->event_deactivate_list, item);
				DbgFree(item);
				return LB_STATUS_SUCCESS;
			}
		}
		break;

	case CLIENT_EVENT_ACTIVATE:
		EINA_LIST_FOREACH_SAFE(client->event_activate_list, l, n, item) {
			if (item->cb == cb && item->data == data) {
				client->event_activate_list = eina_list_remove(client->event_activate_list, item);
				DbgFree(item);
				return LB_STATUS_SUCCESS;
			}
		}
		break;

	default:
		ErrPrint("Invalid event\n");
		break;
	}

	return LB_STATUS_ERROR_NOT_EXIST;
}

HAPI int client_set_data(struct client_node *client, const char *tag, void *data)
{
	struct data_item *item;

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	item->tag = strdup(tag);
	if (!item->tag) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item);
		return LB_STATUS_ERROR_MEMORY;
	}

	item->data = data;

	client->data_list = eina_list_append(client->data_list, item);
	return LB_STATUS_SUCCESS;
}

HAPI void *client_data(struct client_node *client, const char *tag)
{
	Eina_List *l;
	struct data_item *item;

	EINA_LIST_FOREACH(client->data_list, l, item) {
		if (!strcmp(item->tag, tag))
			return item->data;
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
	if (client->paused)
		return;

	client->paused = 1;
	s_info.nr_of_paused_clients++;
}

HAPI void client_resumed(struct client_node *client)
{
	if (client->paused == 0)
		return;

	client->paused = 0;
	s_info.nr_of_paused_clients--;
}

HAPI int client_init(void)
{
	return LB_STATUS_SUCCESS;
}

HAPI void client_fini(void)
{
	struct global_event_handler *handler;
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
	struct global_event_handler *handler;

	handler = malloc(sizeof(*handler));
	if (!handler) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	handler->cbdata = data;
	handler->cb = cb;

	switch (event_type) {
	case CLIENT_GLOBAL_EVENT_CREATE:
		s_info.create_event_list = eina_list_prepend(s_info.create_event_list, handler);
		break;
	case CLIENT_GLOBAL_EVENT_DESTROY:
		s_info.destroy_event_list = eina_list_prepend(s_info.destroy_event_list, handler);
		break;
	default:
		DbgFree(handler);
		return LB_STATUS_ERROR_INVALID;
	}

	return LB_STATUS_SUCCESS;
}

HAPI int client_global_event_handler_del(enum client_global_event event_type, int (*cb)(struct client_node *, void *), void *data)
{
	Eina_List *l;
	Eina_List *n;
	struct global_event_handler *handler;

	switch (event_type) {
	case CLIENT_GLOBAL_EVENT_CREATE:
		EINA_LIST_FOREACH_SAFE(s_info.create_event_list, l, n, handler) {
			if (handler->cb == cb && handler->cbdata == data) {
				s_info.create_event_list = eina_list_remove(s_info.create_event_list, handler);
				DbgFree(handler);
				return LB_STATUS_SUCCESS;
			}
		}
		break;
	case CLIENT_GLOBAL_EVENT_DESTROY:
		EINA_LIST_FOREACH_SAFE(s_info.destroy_event_list, l, n, handler) {
			if (handler->cb == cb && handler->cbdata == data) {
				s_info.destroy_event_list = eina_list_remove(s_info.destroy_event_list, handler);
				DbgFree(handler);
				return LB_STATUS_SUCCESS;
			}
		}
		break;
	default:
		break;
	}

	return LB_STATUS_ERROR_NOT_EXIST;
}

HAPI int client_subscribe(struct client_node *client, const char *cluster, const char *category)
{
	struct subscribe_item *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	item->cluster = strdup(cluster);
	if (!item->cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item);
		return LB_STATUS_ERROR_MEMORY;
	}

	item->category = strdup(category);
	if (!item->category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item->cluster);
		DbgFree(item);
		return LB_STATUS_ERROR_MEMORY;
	}

	client->subscribe_list = eina_list_append(client->subscribe_list, item);
	return LB_STATUS_SUCCESS;
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
			return LB_STATUS_SUCCESS;
		}
	}

	return LB_STATUS_ERROR_NOT_EXIST;
}

HAPI int client_is_subscribed(struct client_node *client, const char *cluster, const char *category)
{
	struct subscribe_item *item;
	Eina_List *l;

	EINA_LIST_FOREACH(client->subscribe_list, l, item) {
		if (!strcmp(item->cluster, "*"))
			return 1;

		if (strcasecmp(item->cluster, cluster))
			continue;

		if (!strcmp(item->category, "*"))
			return 1;

		if (!strcasecmp(item->category, category))
			return 1;
	}

	return 0;
}

HAPI int client_browse_list(const char *cluster, const char *category, int (*cb)(struct client_node *client, void *data), void *data)
{
	Eina_List *l;
	struct client_node *client;
	int cnt;

	if (!cb || !cluster || !category)
		return LB_STATUS_ERROR_INVALID;

	cnt = 0;
	EINA_LIST_FOREACH(s_info.client_list, l, client) {
		if (!client_is_subscribed(client, cluster, category))
			continue;

		if (cb(client, data) < 0)
			return LB_STATUS_ERROR_CANCEL;

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
	return LB_STATUS_SUCCESS;
}

/* End of a file */
