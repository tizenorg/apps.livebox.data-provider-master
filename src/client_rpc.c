/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org
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
#include <com-core_packet.h>
#include <packet.h>
#include <livebox-errno.h>

#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "debug.h"
#include "conf.h"
#include "util.h"

#define RPC_TAG "rpc"

/*!
 * \note
 * Static component information structure.
 */
static struct info {
	Eina_List *command_list; /*!< Packet Q: Before sending the request, all request commands will stay here */
	Ecore_Timer *command_consumer; /*!< This timer will consuming the command Q. sending them to the specified client */
} s_info = {
	.command_list = NULL,
	.command_consumer = NULL,
};

struct client_rpc {
	int handle; /*!< Handler for communication with client */
};

struct command {
	struct packet *packet;
	struct client_node *client; /*!< Target client. who should receive this command */
};

/*!
 * \brief
 * Creating or Destroying command object
 */
static inline struct command *create_command(struct client_node *client, struct packet *packet)
{
	struct command *command;

	command = calloc(1, sizeof(*command));
	if (!command) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	command->packet = packet_ref(packet);
	command->client = client_ref(client);

	return command;
}

static inline void destroy_command(struct command *command)
{
	client_unref(command->client);
	packet_unref(command->packet);
	DbgFree(command);
}

static inline int count_command(void)
{
	return eina_list_count(s_info.command_list);
}

static inline struct command *pop_command(void)
{
	struct command *command;

	command = eina_list_nth(s_info.command_list, 0);
	if (!command)
		return NULL;

	s_info.command_list = eina_list_remove(s_info.command_list, command);
	return command;
}

static Eina_Bool command_consumer_cb(void *data)
{
	struct command *command;
	struct client_rpc *rpc;
	int ret;

	command = pop_command();
	if (!command) {
		s_info.command_consumer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	if (!command->client) {
		DbgPrint("Has no client\n");
		goto out;
	}

	if (!client_is_activated(command->client)) {
		ErrPrint("Client[%p] is not activated, destroy this command\n", command->client);
		goto out;
	}

	if (client_is_faulted(command->client)) {
		ErrPrint("Client[%p] is faulted, discard command\n", command->client);
		goto out;
	}

	rpc = client_data(command->client, RPC_TAG);
	if (!rpc) {
		ErrPrint("Invalid command\n");
		goto out;
	}

	if (rpc->handle < 0) {
		DbgPrint("RPC is not initialized\n");
		goto out;
	}

	ret = com_core_packet_send_only(rpc->handle, command->packet);
	if (ret < 0)
		ErrPrint("Failed to send packet %d\n", ret);

out:
	destroy_command(command);
	return ECORE_CALLBACK_RENEW;
}

static inline void push_command(struct command *command)
{
	s_info.command_list = eina_list_append(s_info.command_list, command);

	if (s_info.command_consumer)
		return;

	s_info.command_consumer = ecore_timer_add(PACKET_TIME, command_consumer_cb, NULL);
	if (!s_info.command_consumer) {
		ErrPrint("Failed to add command consumer\n");
		s_info.command_list = eina_list_remove(s_info.command_list, command);
		destroy_command(command);
	}
}

HAPI int client_rpc_async_request(struct client_node *client, struct packet *packet)
{
	struct command *command;
	struct client_rpc *rpc;

	if (!client || !packet)
		return LB_STATUS_ERROR_INVALID;

	if (client_is_faulted(client)) {
		ErrPrint("Client[%p] is faulted\n", client);
		packet_unref(packet);
		return LB_STATUS_ERROR_FAULT;
	}

	rpc = client_data(client, RPC_TAG);
	if (!rpc)
		ErrPrint("Client[%p] is not ready for communication (%s)\n", client, packet_command(packet));

	command = create_command(client, packet);
	if (!command) {
		packet_unref(packet);
		return LB_STATUS_ERROR_MEMORY;
	}

	push_command(command);
	packet_unref(packet);
	return LB_STATUS_SUCCESS;
}

static int deactivated_cb(struct client_node *client, void *data)
{
	struct client_rpc *rpc;
	struct command *command;
	Eina_List *l;
	Eina_List *n;

	rpc = client_data(client, RPC_TAG);
	if (!rpc) {
		ErrPrint("client is not valid\n");
		return LB_STATUS_SUCCESS;
	}

	DbgPrint("Reset handle for %d\n", client_pid(client));
	rpc->handle = -1;

	DbgPrint("Begin: Destroying command\n");
	EINA_LIST_FOREACH_SAFE(s_info.command_list, l, n, command) {
		if (command->client == client) {
			s_info.command_list = eina_list_remove(s_info.command_list, command);
			destroy_command(command);
		}
	}
	DbgPrint("End: Destroying command\n");

	return LB_STATUS_SUCCESS;
}

HAPI int client_rpc_init(struct client_node *client, int handle)
{
	struct client_rpc *rpc;
	int ret;

	rpc = calloc(1, sizeof(*rpc));
	if (!rpc) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	ret = client_set_data(client, RPC_TAG, rpc);
	if (ret < 0) {
		ErrPrint("Failed to set \"rpc\" for client\n");
		DbgFree(rpc);
		return ret;
	}

	DbgPrint("CLIENT: New handle assigned for %d, %d (old: %d)\n", client_pid(client), handle, rpc->handle);
	rpc->handle = handle;

	client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, deactivated_cb, NULL);
	return LB_STATUS_SUCCESS;
}

HAPI int client_rpc_fini(struct client_node *client)
{
	struct client_rpc *rpc;

	rpc = client_del_data(client, RPC_TAG);
	if (!rpc)
		return LB_STATUS_ERROR_INVALID;

	client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, deactivated_cb, NULL);
	DbgFree(rpc);
	return LB_STATUS_SUCCESS;
}

HAPI int client_rpc_handle(struct client_node *client)
{
	struct client_rpc *rpc;

	rpc = client_data(client, RPC_TAG);
	if (!rpc) {
		DbgPrint("Client has no RPC\n");
		return LB_STATUS_ERROR_INVALID;
	}

	return rpc->handle;
}

/* End of a file */
