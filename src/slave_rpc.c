/*
 * Copyright 2012  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.tizenopensource.org/license
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <string.h> /* strerror */
#include <errno.h> /* errno */
#include <unistd.h> /* pid_t */
#include <stdlib.h> /* free */
#include <assert.h>

#include <Eina.h>
#include <Ecore.h>

#include <dlog.h>

#include <packet.h>
#include <com-core_packet.h>

#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "package.h"
#include "fault_manager.h"
#include "util.h"
#include "conf.h"

struct slave_rpc {
	Ecore_Timer *pong_timer;
	int handle;

	unsigned long ping_count;
	unsigned long next_ping_count;
	Eina_List *pending_list;
};

struct command {
	/* create_command, destroy_command will care these varaibles */
	char *pkgname;
	struct packet *packet;
	struct slave_node *slave;
	int ttl; /* If it fails to handle this, destroy this */

	/* Don't need to care these data */
	void (*ret_cb)(struct slave_node *slave, const struct packet *packet, void *cbdata);
	void *cbdata;
};

static struct info {
	Eina_List *command_list;
	Ecore_Timer *command_consuming_timer;
} s_info = {
	.command_list = NULL,
	.command_consuming_timer = NULL,
};

#define DEFAULT_CMD_TTL 3

static inline void prepend_command(struct command *command);

static inline struct command *create_command(struct slave_node *slave, const char *pkgname, struct packet *packet)
{
	struct command *command;

	command = calloc(1, sizeof(*command));
	if (!command) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	if (pkgname) {
		command->pkgname = strdup(pkgname);
		if (!command->pkgname) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(command);
			return NULL;
		}
	}

	command->slave = slave_ref(slave); /*!< To prevent from destroying of the slave while communicating with the slave */
	command->packet = packet_ref(packet);
	command->ttl = DEFAULT_CMD_TTL;

	return command;
}

static inline void destroy_command(struct command *command)
{
	slave_unref(command->slave);
	packet_unref(command->packet);
	DbgFree(command->pkgname);
	DbgFree(command);
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

static int slave_async_cb(pid_t pid, int handle, const struct packet *packet, void *data)
{
	struct command *command = data;

	if (!command) {
		ErrPrint("Packet is NIL\n");
		return 0;
	}

	/*!
	 * \note
	 * command->packet is not valid from here.
	 */
	if (!slave_is_activated(command->slave)) {
		ErrPrint("Slave is not activated (accidently dead)\n");
		if (command->ret_cb)
			command->ret_cb(command->slave, packet, command->cbdata);

		goto out;
	}

	if (!packet) {
		DbgPrint("packet == NULL\n");
		if (command->ret_cb)
			command->ret_cb(command->slave, packet, command->cbdata);

		command->slave = slave_deactivated_by_fault(command->slave);
		goto out;
	}

	if (command->ret_cb)
		command->ret_cb(command->slave, packet, command->cbdata);

out:
	destroy_command(command);
	return 0;
}

static Eina_Bool command_consumer_cb(void *data)
{
	struct command *command;
	struct slave_rpc *rpc;

	command = pop_command();
	if (!command) {
		s_info.command_consuming_timer = NULL;
		return ECORE_CALLBACK_CANCEL;
	}

	if (!slave_is_activated(command->slave)) {
		ErrPrint("Slave is not activated: %s(%d)\n",
				slave_name(command->slave), slave_pid(command->slave));
		goto errout;
	}

	if (command->pkgname) {
		struct pkg_info *info;

		info = package_find(command->pkgname);
		if (info && package_is_fault(info)) {
			ErrPrint("info: %p (%s) is fault package\n", info, command->pkgname);
			goto errout;
		}
	}

	rpc = slave_data(command->slave, "rpc");
	if (!rpc || rpc->handle < 0) {
		ErrPrint("Slave has no rpc info\n");
		goto errout;
	}

	if (packet_type(command->packet) == PACKET_REQ_NOACK) {
		if (com_core_packet_send_only(rpc->handle, command->packet) == 0) {
			/* Keep a slave alive, while processing events */
			slave_give_more_ttl(command->slave);
			destroy_command(command);
			return ECORE_CALLBACK_RENEW;
		}
	} else if (packet_type(command->packet) == PACKET_REQ) {
		if (com_core_packet_async_send(rpc->handle, command->packet, 0.0f, slave_async_cb, command) == 0) {
			/* Keep a slave alive, while processing events */
			slave_give_more_ttl(command->slave);
			return ECORE_CALLBACK_RENEW;
		}
	}

	/*!
	 * \WARN
	 * What happens at here?
	 * We are failed to send a packet!!!
	 * Let's try to send this again
	 */
	/*!
	 * \todo
	 * Do we need to handle this error?
	 * Close current connection and make new one?
	 * how about pended command lists?
	 */
	DbgPrint("Packet type: %d\n", packet_type(command->packet));
	DbgPrint("Packet: %p\n", command->packet);
	DbgPrint("Handle: %d\n", rpc->handle);
	DbgPrint("PID: %d\n", slave_pid(command->slave));
	DbgPrint("Name: %s\n", slave_name(command->slave));
	DbgPrint("Package: %s\n", command->pkgname);
	command->ttl--;
	if (command->ttl == 0) {
		DbgPrint("Discard packet (%d)\n", command->ttl);
		destroy_command(command);
	} else {
		DbgPrint("Send again (%d)\n", command->ttl);
		prepend_command(command);
	}
	return ECORE_CALLBACK_RENEW;

errout:
	if (command->ret_cb)
		command->ret_cb(command->slave, NULL, command->cbdata);

	destroy_command(command);
	return ECORE_CALLBACK_RENEW;
}

static inline void prepend_command(struct command *command)
{
	s_info.command_list = eina_list_prepend(s_info.command_list, command);

	if (s_info.command_consuming_timer)
		return;

	s_info.command_consuming_timer = ecore_timer_add(PACKET_TIME, command_consumer_cb, NULL);
	if (!s_info.command_consuming_timer) {
		ErrPrint("Failed to add command consumer\n");
		s_info.command_list = eina_list_remove(s_info.command_list, command);
		destroy_command(command);
	}
}

static inline void push_command(struct command *command)
{
	s_info.command_list = eina_list_append(s_info.command_list, command);

	if (s_info.command_consuming_timer)
		return;

	s_info.command_consuming_timer = ecore_timer_add(PACKET_TIME, command_consumer_cb, NULL);
	if (!s_info.command_consuming_timer) {
		ErrPrint("Failed to add command consumer\n");
		s_info.command_list = eina_list_remove(s_info.command_list, command);
		destroy_command(command);
	}
}

static int slave_deactivate_cb(struct slave_node *slave, void *data)
{
	struct slave_rpc *rpc;
	struct command *command;
	Eina_List *l;
	Eina_List *n;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		/*!
		 * \note
		 * Return negative value will remove this callback from the event list of the slave
		 */
		return -EINVAL;
	}

	if (rpc->pong_timer) {
		ecore_timer_del(rpc->pong_timer);
		rpc->pong_timer = NULL;
	} else {
		ErrPrint("slave has no pong timer\n");
	}

	if (rpc->handle < 0) {
		EINA_LIST_FREE(rpc->pending_list, command) {
			assert(command->slave == slave);
			if (command->ret_cb)
				command->ret_cb(command->slave, NULL, command->cbdata);
			destroy_command(command);
		}
	} else {
		EINA_LIST_FOREACH_SAFE(s_info.command_list, l, n, command) {
			if (command->slave == slave) {
				s_info.command_list = eina_list_remove(s_info.command_list, command);
				if (command->ret_cb)
					command->ret_cb(command->slave, NULL, command->cbdata);
				destroy_command(command);
			}
		}
	}

	/*!
	 * \note
	 * Reset handle
	 */
	DbgPrint("Reset handle for %d (%d)\n", slave_pid(slave), rpc->handle);
	rpc->handle = -1;

	/*!
	 * \todo
	 * Make statistics table
	 */
	rpc->ping_count = 0;
	rpc->next_ping_count = 1;
	return 0;
}

static Eina_Bool ping_timeout_cb(void *data)
{
	struct slave_rpc *rpc;
	struct slave_node *slave = data;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave RPC is not valid (%s)\n", slave_name(slave));
		return ECORE_CALLBACK_CANCEL;
	}

	/*!
	 * \note
	 * Clear the pong_timer
	 */
	rpc->pong_timer = NULL;

	if (!slave_is_activated(slave)) {
		ErrPrint("Slave is not activated (%s)\n", slave_name(slave));
		return ECORE_CALLBACK_CANCEL;
	}

	/*!
	 * Dead callback will handling this
	 */
	DbgPrint("Slave PING TIMEOUT: %s(%d)\n", slave_name(slave), slave_pid(slave));
	slave = slave_deactivated_by_fault(slave);
	DbgPrint("Slave(%p)\n", slave);
	return ECORE_CALLBACK_CANCEL;
}

HAPI int slave_rpc_async_request(struct slave_node *slave, const char *pkgname, struct packet *packet, void (*ret_cb)(struct slave_node *slave, const struct packet *packet, void *data), void *data, int urgent)
{
	struct command *command;
	struct slave_rpc *rpc;

	command = create_command(slave, pkgname, packet);
	if (!command) {
		ErrPrint("Failed to create command\n");

		if (ret_cb)
			ret_cb(slave, NULL, data);

		packet_unref(packet);
		return -ENOMEM;
	}

	command->ret_cb = ret_cb;
	command->cbdata = data;
	packet_unref(packet);

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave has no RPC\n");
		if (ret_cb)
			ret_cb(slave, NULL, data);
		destroy_command(command);
		return -EFAULT;
	}

	if (rpc->handle < 0) {
		DbgPrint("RPC handle is not ready to use it\n");
		if (slave_is_secured(slave) && !slave_is_activated(slave)) {
			int ret;
			DbgPrint("Activate slave forcely\n");
			ret = slave_activate(slave);
			if (ret < 0 && ret != -EALREADY) {

				if (ret_cb)
					ret_cb(slave, NULL, data);

				destroy_command(command);
				return ret;
			}
		}

		if (urgent)
			rpc->pending_list = eina_list_prepend(rpc->pending_list, command);
		else
			rpc->pending_list = eina_list_append(rpc->pending_list, command);

		return 0;
	}

	if (urgent)
		prepend_command(command);
	else
		push_command(command);

	return 0;
}

HAPI int slave_rpc_request_only(struct slave_node *slave, const char *pkgname, struct packet *packet, int urgent)
{
	struct command *command;
	struct slave_rpc *rpc;

	command = create_command(slave, pkgname, packet);
	if (!command) {
		ErrPrint("Failed to create a command\n");
		packet_unref(packet);
		return -ENOMEM;
	}

	command->ret_cb = NULL;
	command->cbdata = NULL;
	packet_unref(packet);

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave has no RPC\n");
		destroy_command(command);
		return -EFAULT;
	}

	if (rpc->handle < 0) {
		DbgPrint("RPC handle is not ready to use it\n");

		if (slave_is_secured(slave) && !slave_is_activated(slave)) {
			int ret;

			DbgPrint("Activate slave forcely\n");
			ret = slave_activate(slave);
			if (ret < 0 && ret != -EALREADY) {
				destroy_command(command);
				return ret;
			}
		}

		if (urgent)
			rpc->pending_list = eina_list_prepend(rpc->pending_list, command);
		else
			rpc->pending_list = eina_list_append(rpc->pending_list, command);

		return 0;
	}

	if (urgent)
		prepend_command(command);
	else
		push_command(command);

	return 0;
}

HAPI int slave_rpc_update_handle(struct slave_node *slave, int handle)
{
	struct slave_rpc *rpc;
	struct command *command;

	rpc = slave_data(slave, "rpc");
	if (!rpc)
		return -EINVAL;

	DbgPrint("SLAVE: New handle assigned for %d, %d\n", slave_pid(slave), handle);
	rpc->handle = handle;
	if (rpc->pong_timer)
		ecore_timer_del(rpc->pong_timer);

	rpc->pong_timer = ecore_timer_add(DEFAULT_PING_TIME, ping_timeout_cb, slave);
	if (!rpc->pong_timer)
		ErrPrint("Failed to add ping timer\n");

	/*!
	 * \note
	 * slave_activated will call the activated callback.
	 * activated callback will try to recover the normal instances state.
	 * so the reset_fault should be called after slave_activated function.
	 */
	slave_activated(slave);

	EINA_LIST_FREE(rpc->pending_list, command) {
		push_command(command);
	}

	return 0;
}

HAPI int slave_rpc_init(struct slave_node *slave)
{
	struct slave_rpc *rpc;

	rpc = calloc(1, sizeof(*rpc));
	if (!rpc) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	if (slave_set_data(slave, "rpc", rpc) < 0) {
		DbgFree(rpc);
		return -ENOMEM;
	}

	slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_deactivate_cb, NULL);

	rpc->ping_count = 0;
	rpc->next_ping_count = 1;
	rpc->handle = -1;

	return 0;
}

HAPI int slave_rpc_fini(struct slave_node *slave)
{
	struct slave_rpc *rpc;

	rpc = slave_del_data(slave, "rpc");
	if (!rpc)
		return -EINVAL;

	slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_deactivate_cb, NULL);

	if (rpc->pong_timer)
		ecore_timer_del(rpc->pong_timer);

	DbgFree(rpc);
	return 0;
}

HAPI int slave_rpc_ping(struct slave_node *slave)
{
	struct slave_rpc *rpc;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave RPC is not valid\n");
		return -EINVAL;
	}

	if (!slave_is_activated(slave)) {
		ErrPrint("Slave is not activated\n");
		return -EFAULT;
	}

	rpc->ping_count++;
	if (rpc->ping_count != rpc->next_ping_count) {
		ErrPrint("Ping count is not correct\n");
		rpc->next_ping_count = rpc->ping_count;
	}
	rpc->next_ping_count++;

	ecore_timer_reset(rpc->pong_timer);
	return 0;
}

HAPI int slave_rpc_ping_freeze(struct slave_node *slave)
{
	struct slave_rpc *rpc;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave RPC is not valid\n");
		return -EINVAL;
	}

	if (!slave_is_activated(slave)) {
		ErrPrint("Slave is not activated\n");
		return -EFAULT;
	}

	ecore_timer_freeze(rpc->pong_timer);
	return 0;
}

HAPI int slave_rpc_ping_thaw(struct slave_node *slave)
{
	struct slave_rpc *rpc;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		ErrPrint("Slave RPC is not valid\n");
		return -EINVAL;
	}

	if (!slave_is_activated(slave)) {
		ErrPrint("Slave is not activated\n");
		return -EFAULT;
	}

	ecore_timer_thaw(rpc->pong_timer);
	return 0;
}

HAPI void slave_rpc_request_update(const char *pkgname, const char *id, const char *cluster, const char *category)
{
	struct slave_node *slave;
	struct pkg_info *info;
	struct packet *packet;

	info = package_find(pkgname);
	if (!info) {
		ErrPrint("Failed to find a package\n");
		return;
	}

	slave = package_slave(info);
	if (!slave) {
		ErrPrint("Failed to find a slave for %s\n", pkgname);
		return;
	}

	packet = packet_create_noack("update_content", "ssss", pkgname, id, cluster, category);
	if (!packet) {
		ErrPrint("Failed to create a new param\n");
		return;
	}

	(void)slave_rpc_request_only(slave, pkgname, packet, 0);
}

HAPI int slave_rpc_handle(struct slave_node *slave)
{
	struct slave_rpc *rpc;

	rpc = slave_data(slave, "rpc");
	if (!rpc) {
		DbgPrint("Slave RPC is not initiated\n");
		return -EINVAL;
	}

	return rpc->handle;
}

/* End of a file */
