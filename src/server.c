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
#include <unistd.h>
#include <errno.h>

#include <dlog.h>
#include <Evas.h>
#include <Ecore_Evas.h> /* fb.h */
#include <aul.h>
#include <Ecore.h>
#include <ail.h>

#include <packet.h>
#include <com-core_packet.h>
#include <livebox-errno.h>
#include <livebox-service.h>

#include "critical_log.h"
#include "conf.h"
#include "debug.h"
#include "server.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "package.h"
#include "script_handler.h"
#include "buffer_handler.h"
#include "util.h"
#include "fault_manager.h"
#include "fb.h" /* fb_type */
#include "group.h"
#include "xmonitor.h"
#include "abi.h"
#include "liveinfo.h"
#include "io.h"
#include "event.h"

static struct info {
	int info_fd;
	int client_fd;
	int service_fd;
	int slave_fd;
	int remote_client_fd;
} s_info = {
	.info_fd = -1,
	.client_fd = -1,
	.service_fd = -1,
	.slave_fd = -1,
	.remote_client_fd = -1,
};

/* Share this with provider */
enum target_type {
	TYPE_LB,
	TYPE_PD,
	TYPE_ERROR
};

struct access_cbdata {
	int status;
	struct inst_info *inst;
};

struct deleted_item {
	struct client_node *client;
	struct inst_info *inst;
};

static inline int forward_lb_event_packet(const struct pkg_info *pkg, struct inst_info *inst, const struct packet *packet)
{
	struct buffer_info *buffer;
	struct slave_node *slave;
	int ret;

	buffer = instance_lb_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = LB_STATUS_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	packet_ref((struct packet *)packet);
	ret = slave_rpc_request_only(slave, package_name(pkg), (struct packet *)packet, 0);

out:
	return ret;
}

static inline int forward_pd_event_packet(const struct pkg_info *pkg, struct inst_info *inst, const struct packet *packet)
{
	struct buffer_info *buffer;
	struct slave_node *slave;
	int ret;

	buffer = instance_pd_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = LB_STATUS_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	packet_ref((struct packet *)packet);
	ret = slave_rpc_request_only(slave, package_name(pkg), (struct packet *)packet, 0);

out:
	return ret;
}

static inline int forward_pd_access_packet(const struct pkg_info *pkg, struct inst_info *inst, const char *command, double timestamp, int x, int y)
{
	int ret;
	struct buffer_info *buffer;
	struct slave_node *slave;
	struct packet *p;

	buffer = instance_pd_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = LB_STATUS_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	p = packet_create_noack(command, "ssdii", package_name(pkg), instance_id(inst), timestamp, x, y);
	ret = slave_rpc_request_only(slave, package_name(pkg), p, 0);

out:
	return ret;
}

static int forward_lb_access_packet(const struct pkg_info *pkg, struct inst_info *inst, const char *command, double timestamp, int x, int y)
{
	int ret;
	struct buffer_info *buffer;
	struct slave_node *slave;
	struct packet *p;

	buffer = instance_lb_buffer(inst);
	if (!buffer) {
		ErrPrint("Instance[%s] has no buffer\n", instance_id(inst));
		ret = LB_STATUS_ERROR_FAULT;
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ErrPrint("Package[%s] has no slave\n", package_name(pkg));
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	p = packet_create_noack(command, "ssdii", package_name(pkg), instance_id(inst), timestamp, x, y);
	ret = slave_rpc_request_only(slave, package_name(pkg), p, 0);

out:
	return ret;
}

static Eina_Bool lazy_access_status_cb(void *data)
{
	struct access_cbdata *cbdata = data;

	if (instance_unref(cbdata->inst)) {
		instance_send_access_status(cbdata->inst, cbdata->status);
	} else {
		DbgPrint("Skip sending access status (%d)\n", cbdata->status);
	}
	/*!
	 * If instance_unref returns NULL,
	 * The instance is destroyed. it means, we don't need to send event to the viewer
	 */
	DbgFree(cbdata);
	return ECORE_CALLBACK_CANCEL;
}

static int slave_fault_open_script_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)script_handler_unload(instance_pd_script(data), 1);
	(void)instance_slave_close_pd(data, instance_pd_owner(data));
	(void)instance_client_pd_created(data, LB_STATUS_ERROR_FAULT);

	timer = instance_del_data(data, "lazy,pd,open");
	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int slave_fault_open_buffer_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)instance_slave_close_pd(data, instance_pd_owner(data));
	(void)instance_client_pd_created(data, LB_STATUS_ERROR_FAULT);

	timer = instance_del_data(data, "pd,open,monitor");
	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int slave_fault_close_script_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)instance_client_pd_destroyed(data, LB_STATUS_ERROR_FAULT);

	timer = instance_del_data(data, "lazy,pd,close");
	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int slave_fault_close_buffer_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)instance_client_pd_destroyed(data, LB_STATUS_ERROR_FAULT);

	timer = instance_del_data(data, "lazy,pd,close");
	if (!timer) {
		timer = instance_del_data(data, "pd,close,monitor");
	}

	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int slave_fault_resize_buffer_cb(struct slave_node *slave, void *data)
{
	Ecore_Timer *timer;

	(void)instance_slave_close_pd(data, instance_pd_owner(data));
	(void)instance_client_pd_destroyed(data, LB_STATUS_ERROR_FAULT);

	timer = instance_del_data(data, "pd,resize,monitor");
	if (timer) {
		ecore_timer_del(timer);
	}

	(void)instance_unref(data);

	return -1; /* remove this handler */
}

static int event_lb_route_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	struct slave_node *slave;
	struct packet *packet;
	const char *cmdstr;

	pkg = instance_package(inst);
	if (!pkg) {
		return LB_STATUS_ERROR_INVALID;
	}

	slave = package_slave(pkg);
	if (!slave) {
		return LB_STATUS_ERROR_INVALID;
	}

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		cmdstr = "lb_mouse_down";
		break;
	case EVENT_STATE_ACTIVATED:
		cmdstr = "lb_mouse_move";
		break;
	case EVENT_STATE_DEACTIVATE:
		cmdstr = "lb_mouse_up";
		break;
	default:
		return LB_STATUS_ERROR_INVALID;
	}

	packet = packet_create_noack(cmdstr, "ssdii", package_name(pkg), instance_id(inst), util_timestamp(), event_info->x, event_info->y);
	if (!packet) {
		return LB_STATUS_ERROR_FAULT;
	}

	return slave_rpc_request_only(slave, package_name(pkg), packet, 0);
}

static int event_lb_consume_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct script_info *script;
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	double timestamp;
	Evas *e;

	pkg = instance_package(inst);
	if (!pkg) {
		return 0;
	}

	script = instance_lb_script(inst);
	if (!script) {
		return LB_STATUS_ERROR_FAULT;
	}

	e = script_handler_evas(script);
	if (!e) {
		return LB_STATUS_ERROR_FAULT;
	}

	timestamp = util_timestamp();

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		script_handler_update_pointer(script, event_info->x, event_info->y, 1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_DOWN, timestamp);
		break;
	case EVENT_STATE_ACTIVATED:
		script_handler_update_pointer(script, event_info->x, event_info->y, -1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_MOVE, timestamp);
		break;
	case EVENT_STATE_DEACTIVATE:
		script_handler_update_pointer(script, event_info->x, event_info->y, 0);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_UP, timestamp);
		break;
	default:
		break;
	}

	return 0;
}

static int event_pd_route_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	struct slave_node *slave;
	struct packet *packet;
	const char *cmdstr;

	pkg = instance_package(inst);
	if (!pkg) {
		return LB_STATUS_ERROR_INVALID;
	}

	slave = package_slave(pkg);
	if (!slave) {
		return LB_STATUS_ERROR_INVALID;
	}

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		cmdstr = "pd_mouse_down";
		break;
	case EVENT_STATE_ACTIVATED:
		cmdstr = "pd_mouse_move";
		break;
	case EVENT_STATE_DEACTIVATE:
		cmdstr = "pd_mouse_up";
		break;
	default:
		return LB_STATUS_ERROR_INVALID;
	}

	packet = packet_create_noack(cmdstr, "ssdii", package_name(pkg), instance_id(inst), util_timestamp(), event_info->x, event_info->y);
	if (!packet) {
		return LB_STATUS_ERROR_FAULT;
	}

	return slave_rpc_request_only(slave, package_name(pkg), packet, 0);
}

static int event_pd_consume_cb(enum event_state state, struct event_data *event_info, void *data)
{
	struct script_info *script;
	struct inst_info *inst = data;
	const struct pkg_info *pkg;
	double timestamp;
	Evas *e;

	pkg = instance_package(inst);
	if (!pkg) {
		return 0;
	}

	script = instance_pd_script(inst);
	if (!script) {
		return LB_STATUS_ERROR_FAULT;
	}

	e = script_handler_evas(script);
	if (!e) {
		return LB_STATUS_ERROR_FAULT;
	}

	timestamp = util_timestamp();

	switch (state) {
	case EVENT_STATE_ACTIVATE:
		script_handler_update_pointer(script, event_info->x, event_info->y, 1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_DOWN, timestamp);
		break;
	case EVENT_STATE_ACTIVATED:
		script_handler_update_pointer(script, event_info->x, event_info->y, -1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_MOVE, timestamp);
		break;
	case EVENT_STATE_DEACTIVATE:
		script_handler_update_pointer(script, event_info->x, event_info->y, 0);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_UP, timestamp);
		break;
	default:
		break;
	}
	return 0;
}

static struct packet *client_acquire(pid_t pid, int handle, const struct packet *packet) /*!< timestamp, ret */
{
	struct client_node *client;
	struct packet *result;
	double timestamp;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (client) {
		ErrPrint("Client is already exists %d\n", pid);
		ret = LB_STATUS_ERROR_EXIST;
		goto out;
	}

	if (packet_get(packet, "d", &timestamp) != 1) {
		ErrPrint("Invalid arguemnt\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = 0;
	/*!
	 * \note
	 * client_create will invoke the client created callback
	 */
	client = client_create(pid, handle);
	if (!client) {
		ErrPrint("Failed to create a new client for %d\n", pid);
		ret = LB_STATUS_ERROR_FAULT;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *cilent_release(pid_t pid, int handle, const struct packet *packet) /*!< pid, ret */
{
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	client_destroy(client);
	ret = 0;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static inline int validate_request(const char *pkgname, const char *id, struct inst_info **out_inst, const struct pkg_info **out_pkg)
{
	struct inst_info *inst;
	const struct pkg_info *pkg;

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance is not exists\n");
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("System error - instance has no package?\n");
		return LB_STATUS_ERROR_INVALID;
	}

	if (package_is_fault(pkg)) {
		ErrPrint("Faulted package: %s\n", pkgname);
		return LB_STATUS_ERROR_FAULT;
	}

	if (out_inst) {
		*out_inst = inst;
	}

	if (out_pkg) {
		*out_pkg = pkg;
	}

	return LB_STATUS_SUCCESS;
}

/*!< pid, pkgname, filename, event, timestamp, x, y, ret */
static struct packet *client_clicked(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	const char *event;
	double timestamp;
	double x;
	double y;
	int ret;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sssddd", &pkgname, &id, &event, &timestamp, &x, &y);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret == LB_STATUS_SUCCESS) {
		(void)instance_clicked(inst, event, timestamp, x, y);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_update_mode(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	int active_update;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &active_update);
	if (ret != 3) {
		ErrPrint("Invalid argument\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret == LB_STATUS_SUCCESS) {
		/*!
		 * \note
		 * Send change update mode request to a slave
		 */
		ret = instance_set_update_mode(inst, active_update);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

/* pid, pkgname, filename, emission, source, s, sy, ex, ey, ret */
static struct packet *client_text_signal(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	const char *emission;
	const char *source;
	double sx;
	double sy;
	double ex;
	double ey;
	struct inst_info *inst;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssssdddd", &pkgname, &id, &emission, &source, &sx, &sy, &ex, &ey);
	if (ret != 8) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret == LB_STATUS_SUCCESS) {
		ret = instance_text_signal_emit(inst, emission, source, sx, sy, ex, ey);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static Eina_Bool lazy_delete_cb(void *data)
{
	struct deleted_item *item = data;

	/*!
	 * Before invoke this callback, the instance is able to already remove this client
	 * So check it again
	 */
	if (instance_has_client(item->inst, item->client)) {
		(void)instance_unicast_deleted_event(item->inst, item->client);
		(void)instance_del_client(item->inst, item->client);
	}

	(void)client_unref(item->client);
	(void)instance_unref(item->inst);
	DbgFree(item);
	return ECORE_CALLBACK_CANCEL;
}

static struct packet *client_delete(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	struct inst_info *inst;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_client(inst) != client) {
		if (instance_has_client(inst, client)) {
			struct deleted_item *item;

			item = malloc(sizeof(*item));
			if (!item) {
				ErrPrint("Heap: %s\n", strerror(errno));
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				/*!
				 * \NOTE:
				 * Send DELETED EVENT to the client.
				 * after return from this function.
				 *
				 * Client will prepare the deleted event after get this function's return value.
				 * So We have to make a delay to send a deleted event.
				 */

				item->client = client_ref(client);
				item->inst = instance_ref(inst);

				if (!ecore_timer_add(DELAY_TIME, lazy_delete_cb, item)) {
					ErrPrint("Failed to add a delayzed delete callback\n");
					(void)client_unref(client);
					(void)instance_unref(inst);
					DbgFree(item);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		} else {
			ErrPrint("Client has no permission\n");
			ret = LB_STATUS_ERROR_PERMISSION;
		}
	} else {
		ret = instance_destroy(inst, INSTANCE_DESTROY_DEFAULT);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_resize(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, w, h, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	struct inst_info *inst;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssii", &pkgname, &id, &w, &h);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	DbgPrint("RESIZE: Client request resize to %dx%d (pid: %d, pkgname: %s)\n", w, h, pid, pkgname);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_client(inst) != client) {
		ret = LB_STATUS_ERROR_PERMISSION;
	} else {
		ret = instance_resize(inst, w, h);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_new(pid_t pid, int handle, const struct packet *packet) /* pid, timestamp, pkgname, content, cluster, category, period, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *content;
	const char *cluster;
	const char *category;
	double period;
	double timestamp;
	int ret;
	struct pkg_info *info;
	int width;
	int height;
	char *lb_pkgname;
	char *mainappid;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "dssssdii", &timestamp, &pkgname, &content, &cluster, &category, &period, &width, &height);
	if (ret != 8) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	DbgPrint("pid[%d] period[%lf] pkgname[%s] content[%s] cluster[%s] category[%s] period[%lf]\n",
						pid, timestamp, pkgname, content, cluster, category, period);

	lb_pkgname = package_lb_pkgname(pkgname);
	if (!lb_pkgname) {
		ErrPrint("This %s has no livebox package\n", pkgname);
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	mainappid = livebox_service_mainappid(lb_pkgname);
	if (!package_is_enabled(mainappid)) {
		DbgFree(mainappid);
		DbgFree(lb_pkgname);
		ret = LB_STATUS_ERROR_DISABLED;
		goto out;
	}
	DbgFree(mainappid);

	info = package_find(lb_pkgname);
	if (!info) {
		info = package_create(lb_pkgname);
	}

	if (!info) {
		ret = LB_STATUS_ERROR_FAULT;
	} else if (package_is_fault(info)) {
		ret = LB_STATUS_ERROR_FAULT;
	} else if (util_free_space(IMAGE_PATH) < MINIMUM_SPACE) {
		ErrPrint("Not enough space\n");
		ret = LB_STATUS_ERROR_NO_SPACE;
	} else {
		struct inst_info *inst;

		if (period > 0.0f && period < MINIMUM_PERIOD) {
			period = MINIMUM_PERIOD;
		}

		inst = instance_create(client, timestamp, lb_pkgname, content, cluster, category, period, width, height);
		/*!
		 * \note
		 * Using the "inst" without validate its value is at my disposal. ;)
		 */
		ret = inst ? 0 : LB_STATUS_ERROR_FAULT;
	}

	DbgFree(lb_pkgname);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_change_visibility(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	enum livebox_visible_state state;
	int ret;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, (int *)&state);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_client(inst) != client) {
		ret = LB_STATUS_ERROR_PERMISSION;
	} else {
		ret = instance_set_visible_state(inst, state);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_set_period(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, period, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	double period;
	int ret;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssd", &pkgname, &id, &period);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] period[%lf]\n", pid, pkgname, period);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_client(inst) != client) {
		ret = LB_STATUS_ERROR_PERMISSION;
	} else {
		ret = instance_set_period(inst, period);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_change_group(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, cluster, category, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	const char *cluster;
	const char *category;
	struct inst_info *inst;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssss", &pkgname, &id, &cluster, &category);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] cluster[%s] category[%s]\n", pid, pkgname, cluster, category);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_client(inst) != client) {
		ret = LB_STATUS_ERROR_PERMISSION;
	} else {
		ret = instance_change_group(inst, cluster, category);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_pd_mouse_enter(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_event_packet(pkg, inst, packet);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_IN, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_mouse_leave(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_event_packet(pkg, inst, packet);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_OUT, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_mouse_down(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, id, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_event_packet(pkg, inst, packet);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_DOWN, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_mouse_up(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_event_packet(pkg, inst, packet);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_UP, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_mouse_move(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_event_packet(pkg, inst, packet);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_MOVE, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_mouse_move(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_event_packet(pkg, inst, packet);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_MOVE, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static int inst_del_cb(struct inst_info *inst, void *data)
{
	(void)event_deactivate(data, inst);
	return -1; /* Delete this callback */
}

static struct packet *client_lb_mouse_set(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = event_activate(x, y, event_lb_route_cb, inst);
		if (ret == 0) {
			instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, event_lb_route_cb);
		}
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		ret = event_activate(x, y, event_lb_consume_cb, inst);
		if (ret == 0) {
			instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, event_lb_consume_cb);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}
out:
	return NULL;
}

static struct packet *client_lb_mouse_unset(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = event_deactivate(event_lb_route_cb, inst);
		if (ret == 0) {
			instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb);
		}
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		ret = event_deactivate(event_lb_consume_cb, inst);
		if (ret == 0) {
			instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}
out:
	return NULL;
}

static struct packet *client_pd_mouse_set(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = event_activate(x, y, event_pd_route_cb, inst);
		if (ret == 0) {
			instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, event_pd_route_cb);
		}
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		ret = event_activate(x, y, event_pd_consume_cb, inst);
		if (ret == 0) {
			instance_event_callback_add(inst, INSTANCE_EVENT_DESTROY, inst_del_cb, event_pd_consume_cb);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	return NULL;
}

static struct packet *client_pd_mouse_unset(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = event_deactivate(event_pd_route_cb, inst);
		if (ret == 0) {
			instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb);
		}
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		ret = event_deactivate(event_pd_consume_cb, inst);
		if (ret == 0) {
			instance_event_callback_del(inst, INSTANCE_EVENT_DESTROY, inst_del_cb);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}
out:
	return NULL;
}

static struct packet *client_lb_mouse_enter(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_event_packet(pkg, inst, packet);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_IN, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_mouse_leave(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_event_packet(pkg, inst, packet);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_OUT, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_mouse_down(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_event_packet(pkg, inst, packet);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_DOWN, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_mouse_up(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_event_packet(pkg, inst, packet);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		script_handler_feed_event(script, LB_SCRIPT_MOUSE_UP, timestamp);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_access_action_up(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_ACTION, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_access_action_down(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_ACTION, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_access_scroll_down(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_SCROLL, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_access_scroll_move(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_SCROLL, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_access_scroll_up(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_SCROLL, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_access_unhighlight(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int ret;
	double timestamp;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_UNHIGHLIGHT, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}
out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_access_hl(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_HIGHLIGHT, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_access_hl_prev(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_HIGHLIGHT_PREV, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_access_hl_next(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ErrPrint("Script is not created yet\n");
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ErrPrint("Evas is not exists\n");
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_HIGHLIGHT_NEXT, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ErrPrint("Heap: %s\n", strerror(errno));
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					ErrPrint("Failed to add timer\n");
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		} else {
			DbgPrint("Returns: %d\n", ret);
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_access_activate(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_ACTIVATE, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_key_down(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_event_packet(pkg, inst, packet);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		/*!
		 * \TODO: Push up the KEY_DOWN event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pause_request(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	double timestamp;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is paused - manually reported\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "d", &timestamp);
	if (ret != 1) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	if (USE_XMONITOR) {
		DbgPrint("XMONITOR enabled. ignore client paused request\n");
	} else {
		xmonitor_pause(client);
	}

out:
	return NULL;
}

static struct packet *client_resume_request(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	double timestamp;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "d", &timestamp);
	if (ret != 1) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	if (USE_XMONITOR) {
		DbgPrint("XMONITOR enabled. ignore client resumed request\n");
	} else {
		xmonitor_resume(client);
	}

out:
	return NULL;
}

static struct packet *client_pd_key_up(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Invalid parameter\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		ret = forward_pd_event_packet(pkg, inst, packet);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		/*!
		 * \TODO: Push up the KEY_UP event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_access_hl(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_HIGHLIGHT, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_access_hl_prev(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_HIGHLIGHT_PREV, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_access_hl_next(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_HIGHLIGHT_NEXT, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_access_action_up(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int x;
	int y;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ErrPrint("Script has no evas\n");
			ret = LB_STATUS_ERROR_INVALID;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_ACTION, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_access_action_down(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int x;
	int y;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ErrPrint("Script has no evas\n");
			ret = LB_STATUS_ERROR_INVALID;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_ACTION, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_access_unhighlight(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_UNHIGHLIGHT, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_access_scroll_down(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int x;
	int y;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ErrPrint("Instance has no evas\n");
			ret = LB_STATUS_ERROR_INVALID;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_SCROLL, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_access_scroll_move(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int x;
	int y;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ErrPrint("Instance has no evas\n");
			ret = LB_STATUS_ERROR_INVALID;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_SCROLL, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_access_scroll_up(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int x;
	int y;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exist\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
		/*!
		 * Enen if it fails to send packet,
		 * The packet will be unref'd
		 * So we don't need to check the ret value.
		 */
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ErrPrint("Instance has no evas\n");
			ret = LB_STATUS_ERROR_INVALID;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_SCROLL, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_access_activate(pid_t pid, int handle, const struct packet *packet)
{
	struct packet *result;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 5) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_access_packet(pkg, inst, packet_command(packet), timestamp, x, y);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ErrPrint("Instance has no script\n");
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ErrPrint("Script has no Evas\n");
			ret = LB_STATUS_ERROR_INVALID;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		ret = script_handler_feed_event(script, LB_SCRIPT_ACCESS_ACTIVATE, timestamp);
		if (ret >= 0) {
			struct access_cbdata *cbdata;

			cbdata = malloc(sizeof(*cbdata));
			if (!cbdata) {
				ret = LB_STATUS_ERROR_MEMORY;
			} else {
				cbdata->inst = instance_ref(inst);
				cbdata->status = ret;

				if (!ecore_timer_add(DELAY_TIME, lazy_access_status_cb, cbdata)) {
					(void)instance_unref(cbdata->inst);
					DbgFree(cbdata);
					ret = LB_STATUS_ERROR_FAULT;
				} else {
					ret = LB_STATUS_SUCCESS;
				}
			}
		}
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_key_down(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_event_packet(pkg, inst, packet);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);

		/*!
		 * \TODO: Feed up this KEY_DOWN event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_key_up(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	double timestamp;
	int x;
	int y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdii", &pkgname, &id, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = forward_lb_event_packet(pkg, inst, packet);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);

		/*!
		 * \TODO: Feed up this KEY_UP event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static int release_pixmap_cb(struct client_node *client, void *canvas)
{
	DbgPrint("Forcely unref the \"buffer\"\n");
	buffer_handler_pixmap_unref(canvas);
	return -1; /* Delete this callback */
}

static struct packet *client_lb_acquire_pixmap(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct packet *result;
	const char *pkgname;
	const char *id;
	struct client_node *client;
	struct inst_info *inst;
	int ret;
	int pixmap = 0;
	void *buf_ptr;
	struct buffer_info *buffer;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	buffer = instance_lb_buffer(inst);
	if (!buffer) {
		struct script_info *script_info;
		struct fb_info *fb_info;

		script_info = instance_lb_script(inst);
		if (!script_info) {
			ErrPrint("Unable to get LB buffer: %s\n", id);
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		fb_info = script_handler_fb(script_info);
		if (!fb_info) {
			ErrPrint("Unable to get fb_info: %s\n", id);
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		buffer = fb_buffer_info(fb_info);
		if (!buffer) {
			ErrPrint("Unable to get buffer_info: %s\n", id);
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}
	}

	buf_ptr = buffer_handler_pixmap_ref(buffer);
	if (!buf_ptr) {
		ErrPrint("Failed to ref pixmap\n");
		ret = LB_STATUS_ERROR_FAULT;
		goto out;
	}

	ret = client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr);
	if (ret < 0) {
		ErrPrint("Failed to add a new client deactivate callback\n");
		buffer_handler_pixmap_unref(buf_ptr);
	} else {
		pixmap = buffer_handler_pixmap(buffer);
		ret = LB_STATUS_SUCCESS;
	}

out:
	result = packet_create_reply(packet, "ii", pixmap, ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_lb_release_pixmap(pid_t pid, int handle, const struct packet *packet)
{
	const char *pkgname;
	const char *id;
	struct client_node *client;
	struct inst_info *inst;
	int pixmap;
	void *buf_ptr;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pixmap);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	buf_ptr = buffer_handler_pixmap_find(pixmap);
	if (!buf_ptr) {
		ErrPrint("Failed to find a buf_ptr of 0x%X\n", pixmap);
		goto out;
	}

	if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr) == 0) {
		buffer_handler_pixmap_unref(buf_ptr);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_acquire_pixmap(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct packet *result;
	const char *pkgname;
	const char *id;
	struct client_node *client;
	struct inst_info *inst;
	int ret;
	int pixmap = 0;
	void *buf_ptr;
	struct buffer_info *buffer;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ret = LB_STATUS_ERROR_INVALID;
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ret = LB_STATUS_ERROR_INVALID;
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_get_data(inst, "pd,resize,monitor")) {
		ret = LB_STATUS_ERROR_BUSY;
		goto out;
	}

	buffer = instance_pd_buffer(inst);
	if (!buffer) {
		struct script_info *script_info;
		struct fb_info *fb_info;

		script_info = instance_pd_script(inst);
		if (!script_info) {
			ErrPrint("Unable to get LB buffer: %s\n", id);
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		fb_info = script_handler_fb(script_info);
		if (!fb_info) {
			ErrPrint("Unable to get fb_info: %s\n", id);
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}

		buffer = fb_buffer_info(fb_info);
		if (!buffer) {
			ErrPrint("Unable to get buffer_info: %s\n", id);
			ret = LB_STATUS_ERROR_FAULT;
			goto out;
		}
	}

	buf_ptr = buffer_handler_pixmap_ref(buffer);
	if (!buf_ptr) {
		ErrPrint("Failed to ref pixmap\n");
		ret = LB_STATUS_ERROR_FAULT;
		goto out;
	}

	ret = client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr);
	if (ret < 0) {
		buffer_handler_pixmap_unref(buf_ptr);
	}

	pixmap = buffer_handler_pixmap(buffer);
out:
	result = packet_create_reply(packet, "ii", pixmap, ret);
	if (!result) {
		ErrPrint("Failed to create a reply packet\n");
	}

	return result;
}

static struct packet *client_pd_release_pixmap(pid_t pid, int handle, const struct packet *packet)
{
	const char *pkgname;
	const char *id;
	struct client_node *client;
	int pixmap;
	void *buf_ptr;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pixmap);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pkgname, id, NULL, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	buf_ptr = buffer_handler_pixmap_find(pixmap);
	if (!buf_ptr) {
		ErrPrint("Failed to find a buf_ptr of 0x%X\n", pixmap);
		goto out;
	}

	if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr) == 0) {
		buffer_handler_pixmap_unref(buf_ptr);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pinup_changed(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, pinup, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int pinup;
	int ret;
	struct inst_info *inst;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		pinup = 0;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pinup);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		pinup = 0;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret == LB_STATUS_SUCCESS) {
		ret = instance_set_pinup(inst, pinup);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static Eina_Bool lazy_pd_created_cb(void *inst)
{
	struct pkg_info *pkg;

	if (!instance_del_data(inst, "lazy,pd,open")) {
		ErrPrint("lazy,pd,open is already deleted.\n");
		return ECORE_CALLBACK_CANCEL;
	}

	pkg = instance_package(inst);
	if (pkg) {
		struct slave_node *slave;

		slave = package_slave(pkg);
		if (slave) {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_script_cb, inst);
		}
	}

	/*!
	 * After unref instance first,
	 * if the instance is not destroyed, try to notify the created PD event to the client.
	 */
	if (instance_unref(inst)) {
		int ret;
		ret = instance_client_pd_created(inst, LB_STATUS_SUCCESS);
		DbgPrint("Send PD Create event (%d) to client\n", ret);
	}

	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool lazy_pd_destroyed_cb(void *inst)
{
	struct pkg_info *pkg;
	struct slave_node *slave;

	if (!instance_del_data(inst, "lazy,pd,close")) {
		ErrPrint("lazy,pd,close is already deleted.\n");
		return ECORE_CALLBACK_CANCEL;
	}

	pkg = instance_package(inst);
	if (pkg) {
		slave = package_slave(pkg);
		if (slave) {
			if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
				slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_script_cb, inst);
			} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
				slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst);
			}
		}
	}

	if (instance_unref(inst)) {
		int ret;
		ret = instance_client_pd_destroyed(inst, LB_STATUS_SUCCESS);
		if (ret < 0) {
			ErrPrint("Failed sending PD Destroy event (%d)\n", ret);
		}
	}

	return ECORE_CALLBACK_CANCEL;
}

static struct packet *client_pd_move(pid_t pid, int handle, const struct packet *packet) /* pkgname, id, x, y */
{
	struct client_node *client;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	const char *pkgname;
	const char *id;
	double x = 0.0f;
	double y = 0.0f;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdd", &pkgname, &id, &x, &y);
	if (ret != 4) {
		ErrPrint("Parameter is not correct\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		instance_slave_set_pd_pos(inst, x, y);
		ret = instance_signal_emit(inst, "pd,move", instance_id(inst), 0.0, 0.0, 0.0, 0.0, x, y, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		int ix;
		int iy;

		instance_slave_set_pd_pos(inst, x, y);
		ix = x * instance_pd_width(inst);
		iy = y * instance_pd_height(inst);
		script_handler_update_pointer(instance_pd_script(inst), ix, iy, 0);
		ret = instance_signal_emit(inst, "pd,move", instance_id(inst), 0.0, 0.0, 0.0, 0.0, x, y, 0);
	} else {
		ErrPrint("Invalid PD type\n");
		ret = LB_STATUS_ERROR_INVALID;
	}
out:
	DbgPrint("Update PD position: %d\n", ret);
	return NULL;
}

static Eina_Bool pd_open_monitor_cb(void *inst)
{
	int ret;
	struct pkg_info *pkg;

	pkg = instance_package(inst);
	if (pkg) {
		struct slave_node *slave;

		slave = package_slave(pkg);
		if (slave) {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_buffer_cb, inst);
		}
	}

	ret = instance_slave_close_pd(inst, instance_pd_owner(inst));
	ret = instance_client_pd_created(inst, LB_STATUS_ERROR_TIMEOUT);
	(void)instance_del_data(inst, "pd,open,monitor");
	(void)instance_unref(inst);
	ErrPrint("PD Open request is timed-out (%lf), ret: %d\n", PD_REQUEST_TIMEOUT, ret);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool pd_close_monitor_cb(void *inst)
{
	int ret;
	struct pkg_info *pkg;

	pkg = instance_package(inst);
	if (pkg) {
		struct slave_node *slave;

		slave = package_slave(pkg);
		if (slave) {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst);
		}
	}

	ret = instance_client_pd_destroyed(inst, LB_STATUS_ERROR_TIMEOUT);
	(void)instance_del_data(inst, "pd,close,monitor");
	(void)instance_unref(inst);
	ErrPrint("PD Close request is not processed in %lf seconds (%d)\n", PD_REQUEST_TIMEOUT, ret);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool pd_resize_monitor_cb(void *inst)
{
	int ret;
	struct pkg_info *pkg;

	pkg = instance_package(inst);
	if (pkg) {
		struct slave_node *slave;
		slave = package_slave(pkg);
		if (slave) {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_resize_buffer_cb, inst);
		}
	}

	ret = instance_slave_close_pd(inst, instance_pd_owner(inst));
	ret = instance_client_pd_destroyed(inst, LB_STATUS_ERROR_TIMEOUT);
	(void)instance_del_data(inst, "pd,resize,monitor");
	(void)instance_unref(inst);
	ErrPrint("PD Resize request is not processed in %lf seconds (%d)\n", PD_REQUEST_TIMEOUT, ret);
	return ECORE_CALLBACK_CANCEL;
}

static struct packet *client_create_pd(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	Ecore_Timer *pd_monitor;
	double x;
	double y;

	DbgPrint("PERF_DBOX\n");

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ssdd", &pkgname, &id, &x, &y);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_pd_owner(inst)) {
		ErrPrint("PD is already owned\n");
		ret = LB_STATUS_ERROR_ALREADY;
	} else if (package_pd_type(instance_package(inst)) == PD_TYPE_BUFFER) {
		pd_monitor = instance_get_data(inst, "lazy,pd,close");
		if (pd_monitor) {
			ecore_timer_del(pd_monitor);
			/* This timer attribute will be deleted */
			lazy_pd_destroyed_cb(inst);
		}
		
		if (instance_get_data(inst, "pd,open,monitor")) {
			DbgPrint("PD Open request is already processed\n");
			ret = LB_STATUS_ERROR_ALREADY;
			goto out;
		}

		if (instance_get_data(inst, "pd,close,monitor")) {
			DbgPrint("PD Close request is already in process\n");
			ret = LB_STATUS_ERROR_BUSY;
			goto out;
		}

		if (instance_get_data(inst, "pd,resize,monitor")) {
			DbgPrint("PD resize request is already in process\n");
			ret = LB_STATUS_ERROR_BUSY;
			goto out;
		}

		instance_slave_set_pd_pos(inst, x, y);
		/*!
		 * \note
		 * Send request to the slave.
		 * The SLAVE must has to repsonse this via "release_buffer" method.
		 */
		ret = instance_slave_open_pd(inst, client);
		if (ret == LB_STATUS_SUCCESS) {
			ret = instance_signal_emit(inst, "pd,show", instance_id(inst), 0.0, 0.0, 0.0, 0.0, x, y, 0);
			if (ret != LB_STATUS_SUCCESS) {
				int tmp_ret;

				tmp_ret = instance_slave_close_pd(inst, client);
				ErrPrint("Unable to send script event for openning PD [%s], %d\n", pkgname, tmp_ret);
			} else {
				pd_monitor = ecore_timer_add(PD_REQUEST_TIMEOUT, pd_open_monitor_cb, instance_ref(inst));
				if (!pd_monitor) {
					(void)instance_unref(inst);
					ErrPrint("Failed to create a timer for PD Open monitor\n");
				} else {
					struct slave_node *slave;

					(void)instance_set_data(inst, "pd,open,monitor", pd_monitor);

					slave = package_slave(pkg);
					if (!slave) {
						ErrPrint("Failed to get slave(%s)\n", pkgname);
						goto out;
					}

					if (slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_buffer_cb, inst) != LB_STATUS_SUCCESS) {
						ErrPrint("Failed to add fault handler: %s\n");
					}
				}
			}
		} else {
			ErrPrint("Unable to send request for openning PD [%s]\n", pkgname);
		}

		/*!
		 * \note
		 * PD craeted event will be send by the acquire_buffer function.
		 * Because the slave will make request the acquire_buffer to
		 * render the PD
		 *
		 * instance_client_pd_created(inst);
		 */
	} else if (package_pd_type(instance_package(inst)) == PD_TYPE_SCRIPT) {
		int ix;
		int iy;

		pd_monitor = instance_get_data(inst, "lazy,pd,close");
		if (pd_monitor) {
			ecore_timer_del(pd_monitor);
			/* lazy,pd,close will be deleted */
			lazy_pd_destroyed_cb(inst);
		}

		/*!
		 * \note
		 * ret value should be cared but in this case,
		 * we ignore this for this moment, so we have to handle this error later.
		 *
		 * if ret is less than 0, the slave has some problem.
		 * but the script mode doesn't need slave for rendering default view of PD
		 * so we can hanle it later.
		 */
		instance_slave_set_pd_pos(inst, x, y);
		ix = x * instance_pd_width(inst);
		iy = y * instance_pd_height(inst);

		script_handler_update_pointer(instance_pd_script(inst), ix, iy, 0);

		ret = instance_slave_open_pd(inst, client);
		if (ret == LB_STATUS_SUCCESS) {
			ret = script_handler_load(instance_pd_script(inst), 1);

			/*!
			 * \note
			 * Send the PD created event to the clients,
			 */
			if (ret == LB_STATUS_SUCCESS) {

				/*!
				 * \note
				 * But the created event has to be send afte return
				 * from this function or the viewer couldn't care
				 * the event correctly.
				 */
				inst = instance_ref(inst); /* To guarantee the inst */

				/*!
				 * \note
				 * At here, we don't need to rememeber the timer object.
				 * Even if the timer callback is called, after the instance is destroyed.
				 * lazy_pd_created_cb will decrease the instance refcnt first.
				 * At that time, if the instance is released, the timer callback will do nothing.
				 *
				 * 13-05-28
				 * I change my mind. There is no requirements to keep the timer handler.
				 * But I just add it to the tagged-data of the instance.
				 * Just reserve for future-use.
				 */
				pd_monitor = ecore_timer_add(DELAY_TIME, lazy_pd_created_cb, inst);
				if (!pd_monitor) {
					ret = script_handler_unload(instance_pd_script(inst), 1);
					ErrPrint("Unload script: %d\n", ret);

					ret = instance_slave_close_pd(inst, client);
					ErrPrint("Close PD %d\n", ret);

					inst = instance_unref(inst);
					if (!inst) {
						DbgPrint("Instance destroyed\n");
					}

					ErrPrint("Instance: %s\n", pkgname);

					ret = LB_STATUS_ERROR_FAULT;
				} else {
					struct slave_node *slave;

					(void)instance_set_data(inst, "lazy,pd,open", pd_monitor);

					slave = package_slave(pkg);
					if (!slave) {
						ErrPrint("Failed to get slave: %s\n", pkgname);
						goto out;
					}

					if (slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_script_cb, inst) != LB_STATUS_SUCCESS) {
						ErrPrint("Failed to add fault callback: %s\n", pkgname);
					}
				}
			} else {
				int tmp_ret;
				tmp_ret = instance_slave_close_pd(inst, client);
				ErrPrint("Unable to load script: %d, (close: %d)\n", ret, tmp_ret);
			}
		} else {
			ErrPrint("Unable open PD(%s): %d\n", pkgname, ret);
		}
	} else {
		ErrPrint("Invalid PD TYPE\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_destroy_pd(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	Ecore_Timer *pd_monitor;
	struct slave_node *slave;

	DbgPrint("PERF_DBOX\n");

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	slave = package_slave(pkg);
	if (!slave) {
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	if (instance_pd_owner(inst) != client) {
		if (instance_pd_owner(inst) == NULL) {
			ErrPrint("PD looks already closed\n");
			ret = LB_STATUS_ERROR_ALREADY;
		} else {
			ErrPrint("PD owner mimatched\n");
			ret = LB_STATUS_ERROR_PERMISSION;
		}
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		int resize_aborted = 0;

		pd_monitor = instance_del_data(inst, "pd,open,monitor");
		if (pd_monitor) {
			
			ErrPrint("PD Open request is found. cancel it [%s]\n", pkgname);

			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_buffer_cb, inst);

			/*!
			 * \note
			 * We should return negative value
			 * Or we have to send "destroyed" event to the client.
			 * If we didn't send destroyed event after return SUCCESS from here,
			 * The client will permanently waiting destroyed event.
			 * Because they understand that the destroy request is successfully processed.
			 */
			ret = LB_STATUS_ERROR_CANCEL;
			ret = instance_client_pd_created(inst, ret);
			if (ret < 0) {
				ErrPrint("PD client create event: %d\n", ret);
			}

			ret = instance_client_pd_destroyed(inst, LB_STATUS_SUCCESS);
			if (ret < 0) {
				ErrPrint("PD client destroy event: %d\n", ret);
			}

			ret = instance_signal_emit(inst, "pd,hide", instance_id(inst), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0);
			if (ret < 0) {
				ErrPrint("PD close signal emit failed: %d\n", ret);
			}

			ret = instance_slave_close_pd(inst, client);
			if (ret < 0) {
				ErrPrint("PD close request failed: %d\n", ret);
			}

			ecore_timer_del(pd_monitor);
			(void)instance_unref(inst);
			goto out;
		}

		if (instance_get_data(inst, "lazy,pd,close") || instance_get_data(inst, "pd,close,monitor")) {
			ret = LB_STATUS_ERROR_ALREADY;
			goto out;
		}

		pd_monitor = instance_del_data(inst, "pd,resize,monitor");
		if (pd_monitor) {
			ErrPrint("PD Resize request is found. clear it [%s]\n", pkgname);
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_resize_buffer_cb, inst);

			ecore_timer_del(pd_monitor);

			inst = instance_unref(inst);
			if (!inst) {
				goto out;
			}

			resize_aborted = 1;
		}

		ret = instance_signal_emit(inst, "pd,hide", instance_id(inst), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0);
		if (ret < 0) {
			ErrPrint("PD close signal emit failed: %d\n", ret);
		}

		ret = instance_slave_close_pd(inst, client);
		if (ret < 0) {
			ErrPrint("PD close request failed: %d\n", ret);
		} else if (resize_aborted) {
			pd_monitor = ecore_timer_add(DELAY_TIME, lazy_pd_destroyed_cb, instance_ref(inst));
			if (!pd_monitor) {
				ErrPrint("Failed to create a timer: %s\n", pkgname);
				(void)instance_unref(inst);
			} else {
				(void)instance_set_data(inst, "lazy,pd,close", pd_monitor);
				slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst);
			}
		} else {
			pd_monitor = ecore_timer_add(PD_REQUEST_TIMEOUT, pd_close_monitor_cb, instance_ref(inst));
			if (!pd_monitor) {
				(void)instance_unref(inst);
				ErrPrint("Failed to add pd close monitor\n");
			} else {
				(void)instance_set_data(inst, "pd,close,monitor", pd_monitor);
				slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst);
			}
		}
		/*!
		 * \note
		 * release_buffer will be called by the slave after this.
		 * Then it will send the "pd_destroyed" event to the client
		 *
		 * instance_client_pd_destroyed(inst);
		 */
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		pd_monitor = instance_get_data(inst, "lazy,pd,open");
		if (pd_monitor) {
			ecore_timer_del(pd_monitor);
			lazy_pd_created_cb(inst);
		}

		ret = script_handler_unload(instance_pd_script(inst), 1);
		if (ret < 0) {
			ErrPrint("Unable to unload the script: %s, %d\n", pkgname, ret);
		}

		/*!
		 * \note
		 * Send request to the slave.
		 * The SLAVE must has to repsonse this via "release_buffer" method.
		 */
		ret = instance_slave_close_pd(inst, client);
		if (ret < 0) {
			ErrPrint("Unable to close the PD: %s, %d\n", pkgname, ret);
		}

		/*!
		 * \note
		 * Send the destroyed PD event to the client
		 */
		if (ret == LB_STATUS_SUCCESS) {
			/*!
			 * \note
			 * 13-05-28
			 * I change my mind. There is no requirements to keep the timer handler.
			 * But I just add it to the tagged-data of the instance.
			 * Just reserve for future-use.
			 */
			pd_monitor = ecore_timer_add(DELAY_TIME, lazy_pd_destroyed_cb, instance_ref(inst));
			if (!pd_monitor) {
				ErrPrint("Failed to create a timer: %s\n", pkgname);
				(void)instance_unref(inst);
			} else {
				(void)instance_set_data(inst, "lazy,pd,close", pd_monitor);
				slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_script_cb, inst);
			}
		}
	} else {
		ErrPrint("Invalid PD TYPE\n");
		ret = LB_STATUS_ERROR_INVALID;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_activate_package(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	int ret;
	struct pkg_info *info;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		pkgname = "";
		goto out;
	}

	ret = packet_get(packet, "s", &pkgname);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		ret = LB_STATUS_ERROR_INVALID;
		pkgname = "";
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s]\n", pid, pkgname);

	/*!
	 * \NOTE:
	 * Validate the livebox package name.
	 */
	if (!package_is_lb_pkgname(pkgname)) {
		ErrPrint("%s is not a valid livebox package\n", pkgname);
		pkgname = "";
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	info = package_find(pkgname);
	if (!info) {
		ret = LB_STATUS_ERROR_NOT_EXIST;
	} else {
		ret = package_clear_fault(info);
	}

out:
	result = packet_create_reply(packet, "is", ret, pkgname);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_subscribed(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid argument\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	DbgPrint("[%d] cluster[%s] category[%s]\n", pid, cluster, category);
	if (!strlen(cluster) || !strcasecmp(cluster, DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster name\n");
		goto out;
	}

	/*!
	 * \todo
	 * SUBSCRIBE cluster & sub-cluster for a client.
	 */
	ret = client_subscribe(client, cluster, category);
	if (ret == 0) {
		package_alter_instances_to_client(client, ALTER_CREATE);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_delete_cluster(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "s", &cluster);
	if (ret != 1) {
		ErrPrint("Invalid parameters\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	DbgPrint("pid[%d] cluster[%s]\n", pid, cluster);

	if (!strlen(cluster) || !strcasecmp(cluster, DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster: %s\n", cluster);
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	/*!
	 * \todo
	 */
	ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static inline int update_pkg_cb(struct category *category, const char *pkgname)
{
	const char *c_name;
	const char *s_name;

	c_name = group_cluster_name_by_category(category);
	s_name = group_category_name(category);

	if (!c_name || !s_name || !pkgname) {
		ErrPrint("Name is not valid\n");
		return EXIT_FAILURE;
	}

	DbgPrint("Send refresh request: %s (%s/%s)\n", pkgname, c_name, s_name);
	slave_rpc_request_update(pkgname, "", c_name, s_name);

	/* Just try to create a new package */
	if (util_free_space(IMAGE_PATH) > MINIMUM_SPACE) {
		double timestamp;
		struct inst_info *inst;

		timestamp = util_timestamp();
		/*!
		 * \NOTE
		 * Don't need to check the subscribed clients.
		 * Because this callback is called by the requests of clients.
		 * It means. some clients wants to handle this instances ;)
		 */
		inst = instance_create(NULL, timestamp, pkgname, "", c_name, s_name, DEFAULT_PERIOD, 0, 0);
		if (!inst) {
			ErrPrint("Failed to create a new instance\n");
		}
	} else {
		ErrPrint("Not enough space\n");
	}
	return EXIT_SUCCESS;
}

static struct packet *client_update(pid_t pid, int handle, const struct packet *packet)
{
	struct inst_info *inst;
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Cilent %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_client(inst) != client) {
		/* PERMISSIONS */
		ErrPrint("Insufficient permissions [%s] - %d\n", pkgname, pid);
	} else {
		slave_rpc_request_update(pkgname, id, instance_cluster(inst), instance_category(inst));
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_refresh_group(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster_id;
	const char *category_id;
	struct client_node *client;
	int ret;
	struct cluster *cluster;
	struct category *category;
	struct context_info *info;
	Eina_List *info_list;
	Eina_List *l;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Cilent %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster_id, &category_id);
	if (ret != 2) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	DbgPrint("[%d] cluster[%s] category[%s]\n", pid, cluster_id, category_id);

	if (!strlen(cluster_id) || !strcasecmp(cluster_id, DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster name: %s\n", cluster_id);
		goto out;
	}

	cluster = group_find_cluster(cluster_id);
	if (!cluster) {
		ErrPrint("Cluster [%s] is not registered\n", cluster_id);
		goto out;
	}

	category = group_find_category(cluster, category_id);
	if (!category) {
		ErrPrint("Category [%s] is not registered\n", category_id);
		goto out;
	}

	info_list = group_context_info_list(category);
	EINA_LIST_FOREACH(info_list, l, info) {
		update_pkg_cb(category, group_pkgname_from_context_info(info));
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_delete_category(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid paramenters\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	DbgPrint("pid[%d] cluster[%s] category[%s]\n", pid, cluster, category);
	if (!strlen(cluster) || !strcasecmp(cluster, DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster: %s\n", cluster);
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	/*!
	 * \todo
	 */
	ret = LB_STATUS_ERROR_NOT_IMPLEMENTED;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *client_unsubscribed(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	int ret;

	client = client_find_by_rpc_handle(handle);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid argument\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	DbgPrint("[%d] cluster[%s] category[%s]\n", pid, cluster, category);

	if (!strlen(cluster) || !strcasecmp(cluster, DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster name: %s\n", cluster);
		goto out;
	}

	/*!
	 * \todo
	 * UNSUBSCRIBE cluster & sub-cluster for a client.
	 */
	ret = client_unsubscribe(client, cluster, category);
	if (ret == 0) {
		package_alter_instances_to_client(client, ALTER_DESTROY);
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *slave_hello(pid_t pid, int handle, const struct packet *packet) /* slave_name, ret */
{
	struct slave_node *slave;
	const char *slavename;
	int ret;

	ret = packet_get(packet, "s", &slavename);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	DbgPrint("New slave[%s](%d) is arrived\n", slavename, pid);

	slave = slave_find_by_name(slavename);

	if (!slave) { /* Try again to find a slave using pid */
		slave = slave_find_by_pid(pid);
	}

	if (!slave) {
		if (DEBUG_MODE) {
			char pkgname[pathconf("/", _PC_PATH_MAX)];
			const char *abi;

			if (aul_app_get_pkgname_bypid(pid, pkgname, sizeof(pkgname)) != AUL_R_OK) {
				ErrPrint("pid[%d] is not authroized provider package, try to find it using its name[%s]\n", pid, slavename);
				slave = slave_find_by_name(slavename);
				pkgname[0] = '\0'; /* Reset the pkgname */
			} else {
				slave = slave_find_by_pkgname(pkgname);
			}

			if (!slave) {
				abi = abi_find_by_pkgname(pkgname);
				if (!abi) {
					abi = DEFAULT_ABI;
					DbgPrint("Slave pkgname is invalid, ABI is replaced with '%s'(default)\n", abi);
				}

				slave = slave_create(slavename, 1, abi, pkgname, 0);
				if (!slave) {
					ErrPrint("Failed to create a new slave for %s\n", slavename);
					goto out;
				}

				DbgPrint("New slave is created (net: 0)\n");
			} else {
				DbgPrint("Registered slave is replaced with this new one\n");
				abi = slave_abi(slave);
				if (!abi) {
					ErrPrint("ABI is not valid: %s\n", slavename);
					abi = DEFAULT_ABI;
				}
			}

			slave_set_pid(slave, pid);
			DbgPrint("Provider is forcely activated, pkgname(%s), abi(%s), slavename(%s)\n", pkgname, abi, slavename);
		} else {
			ErrPrint("Slave[%d, %s] is not exists\n", pid, slavename);
			goto out;
		}
	} else {
		if (slave_pid(slave) != pid) {
			if (slave_pid(slave) > 0) {
				CRITICAL_LOG("Slave(%s) is already assigned to %d\n", slave_name(slave), slave_pid(slave));
				if (pid > 0) {
					ret = aul_terminate_pid(pid);
					CRITICAL_LOG("Terminate %d (ret: %d)\n", pid, ret);
				}
				goto out;
			}
			CRITICAL_LOG("PID of slave(%s) is updated (%d -> %d)\n", slave_name(slave), slave_pid(slave), pid);
			slave_set_pid(slave, pid);
		}
	}

	/*!
	 * \note
	 * After updating handle,
	 * slave activated callback will be called.
	 */
	slave_rpc_update_handle(slave, handle);

out:
	return NULL;
}

static struct packet *slave_ping(pid_t pid, int handle, const struct packet *packet) /* slave_name, ret */
{
	struct slave_node *slave;
	const char *slavename;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "s", &slavename);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
	} else {
		slave_rpc_ping(slave);
	}

out:
	return NULL;
}

static struct packet *slave_faulted(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sss", &pkgname, &id, &func);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = fault_info_set(slave, pkgname, id, func);
	DbgPrint("Slave Faulted: %s (%d)\n", slave_name(slave), ret);

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		DbgPrint("There is a no such instance(%s)\n", id);
	} else if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Instance(%s) is already destroyed\n", id);
	} else {
		ret = instance_destroy(inst, INSTANCE_DESTROY_FAULT);
	}

out:
	return NULL;
}

static struct packet *slave_lb_update_begin(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	const char *pkgname;
	const char *id;
	double priority;
	const char *content;
	const char *title;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssdss", &pkgname, &id, &priority, &content, &title);
	if (ret != 5) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = instance_lb_update_begin(inst, priority, content, title);
		if (ret == LB_STATUS_SUCCESS) {
			slave_freeze_ttl(slave);
		}
	} else {
		ErrPrint("Invalid request[%s]\n", id);
	}

out:
	return NULL;
}

static struct packet *slave_lb_update_end(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	const char *pkgname;
	const char *id;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		ret = instance_lb_update_end(inst);
		if (ret == LB_STATUS_SUCCESS) {
			slave_thaw_ttl(slave);
		}
	} else {
		ErrPrint("Invalid request[%s]\n", id);
	}

out:
	return NULL;
}

static struct packet *slave_pd_update_begin(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	const struct pkg_info *pkg;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		(void)instance_pd_update_begin(inst);
	} else {
		ErrPrint("Invalid request[%s]\n", id);
	}

out:
	return NULL;
}

static struct packet *slave_access_status(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int status;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &status);
	if (ret != 3) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret == LB_STATUS_SUCCESS) {
		if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		} else {
			(void)instance_forward_packet(inst, packet_ref((struct packet *)packet));
		}
	}

out:
	return NULL;
}

static struct packet *slave_pd_update_end(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	const struct pkg_info *pkg;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Invalid parameters\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		(void)instance_pd_update_end(inst);
	} else {
		ErrPrint("Invalid request[%s]\n", id);
	}

out:
	return NULL;
}

static struct packet *slave_call(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, function, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sss", &pkgname, &id, &func);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = fault_func_call(slave, pkgname, id, func);
	slave_give_more_ttl(slave);

out:
	return NULL;
}

static struct packet *slave_ret(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, function, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sss", &pkgname, &id, &func);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = fault_func_ret(slave, pkgname, id, func);
	slave_give_more_ttl(slave);

out:
	return NULL;
}

static struct packet *slave_updated(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, width, height, priority, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	const char *content_info;
	const char *title;
	int w;
	int h;
	double priority;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssiidss", &pkgname, &id,
						&w, &h, &priority,
						&content_info, &title);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret == LB_STATUS_SUCCESS) {
		char *filename;

		if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
			goto out;
		}

		instance_set_lb_info(inst, priority, content_info, title);

		switch (package_lb_type(instance_package(inst))) {
		case LB_TYPE_SCRIPT:
			script_handler_resize(instance_lb_script(inst), w, h);
			filename = util_get_file_kept_in_safe(id);
			if (filename) {
				(void)script_handler_parse_desc(pkgname, id, filename, 0);
				DbgFree(filename);
			} else {
				(void)script_handler_parse_desc(pkgname, id, util_uri_to_path(id), 0);
			}
			break;
		case LB_TYPE_BUFFER:
		default:
			/*!
			 * \check
			 * text format (inst)
			 */
			instance_set_lb_size(inst, w, h);
			instance_lb_updated_by_instance(inst);
			break;
		}

		slave_give_more_ttl(slave);
	}

out:
	return NULL;
}

static struct packet *slave_hold_scroll(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const char *pkgname;
	const char *id;
	int seize;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &seize);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret == LB_STATUS_SUCCESS) {
		if (instance_state(inst) == INST_DESTROYED) {
			ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		} else {
			(void)instance_hold_scroll(inst, seize);
		}
	}

out:
	return NULL;
}

static struct packet *slave_desc_updated(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, decsfile, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	const char *descfile;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "sss", &pkgname, &id, &descfile);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	switch (package_pd_type(instance_package(inst))) {
	case PD_TYPE_SCRIPT:
		if (script_handler_is_loaded(instance_pd_script(inst))) {
			(void)script_handler_parse_desc(pkgname, id, descfile, 1);
		}
		break;
	case PD_TYPE_TEXT:
		instance_set_pd_size(inst, 0, 0);
	case PD_TYPE_BUFFER:
		instance_pd_updated(pkgname, id, descfile);
		break;
	default:
		DbgPrint("Ignore updated DESC(%s)\n", pkgname);
		break;
	}

out:
	return NULL;
}

static struct packet *slave_deleted(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, id, ret */
{
	struct slave_node *slave;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, NULL);
	if (ret == LB_STATUS_SUCCESS) {
		ret = instance_destroyed(inst);
	}

out:
	return NULL;
}

/*!
 * \note for the BUFFER Type slave
 */
static struct packet *slave_acquire_buffer(pid_t pid, int handle, const struct packet *packet) /* type, id, w, h, size */
{
	enum target_type target;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	int pixel_size;
	struct packet *result;
	struct slave_node *slave;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		id = "";
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	ret = packet_get(packet, "issiii", &target, &pkgname, &id, &w, &h, &pixel_size);
	if (ret != 6) {
		ErrPrint("Invalid argument\n");
		id = "";
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	id = "";

	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	ret = LB_STATUS_ERROR_INVALID;

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (target == TYPE_LB && package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_lb_buffer(inst);
		if (!info) {
			if (!instance_create_lb_buffer(inst)) {
				ErrPrint("Failed to create a LB buffer\n");
				ret = LB_STATUS_ERROR_FAULT;
				goto out;
			}

			info = instance_lb_buffer(inst);
			if (!info) {
				ErrPrint("LB buffer is not valid\n");
				/*!
				 * \NOTE
				 * ret value should not be changed.
				 */
				goto out;
			}
		}

		ret = buffer_handler_resize(info, w, h);
		ret = buffer_handler_load(info);
		if (ret == 0) {
			instance_set_lb_size(inst, w, h);
			instance_set_lb_info(inst, PRIORITY_NO_CHANGE, CONTENT_NO_CHANGE, TITLE_NO_CHANGE);
			id = buffer_handler_id(info);
		} else {
			ErrPrint("Failed to load a buffer(%d)\n", ret);
		}
	} else if (target == TYPE_PD && package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *info;
		Ecore_Timer *pd_monitor;
		int is_resize;

		is_resize = 0;
		pd_monitor = instance_del_data(inst, "pd,open,monitor");
		if (!pd_monitor) {
			pd_monitor = instance_del_data(inst, "pd,resize,monitor");
			is_resize = !!pd_monitor;
			if (!is_resize) {
				/* Invalid request. Reject this */
				ErrPrint("Invalid request\n");
				goto out;
			}

			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_resize_buffer_cb, inst);
		} else {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_open_buffer_cb, inst);
		}

		ecore_timer_del(pd_monitor);
		inst = instance_unref(inst);
		if (!inst) {
			ErrPrint("Instance refcnt is ZERO: %s\n", pkgname);
			goto out;
		}

		info = instance_pd_buffer(inst);
		if (!info) {
			if (!instance_create_pd_buffer(inst)) {
				ErrPrint("Failed to create a PD buffer\n");
				ret = LB_STATUS_ERROR_FAULT;
				instance_client_pd_created(inst, ret);
				goto out;
			}

			info = instance_pd_buffer(inst);
			if (!info) {
				ErrPrint("PD buffer is not valid\n");
				/*!
				 * \NOTE
				 * ret value should not be changed.
				 */
				instance_client_pd_created(inst, ret);
				goto out;
			}
		}

		ret = buffer_handler_resize(info, w, h);
		ret = buffer_handler_load(info);
		if (ret == 0) {
			instance_set_pd_size(inst, w, h);
			id = buffer_handler_id(info);
		} else {
			ErrPrint("Failed to load a buffer (%d)\n", ret);
		}

		/*!
		 * Send the PD created event to the client
		 */
		if (!is_resize) {
			instance_client_pd_created(inst, ret);
		}
	}

out:
	result = packet_create_reply(packet, "is", ret, id);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *slave_resize_buffer(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct packet *result;
	enum target_type type;
	const char *pkgname;
	const char *id;
	int w;
	int h;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		ret = LB_STATUS_ERROR_NOT_EXIST;
		id = "";
		goto out;
	}

	ret = packet_get(packet, "issii", &type, &pkgname, &id, &w, &h);
	if (ret != 5) {
		ErrPrint("Invalid argument\n");
		ret = LB_STATUS_ERROR_INVALID;
		id = "";
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	id = "";
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	ret = LB_STATUS_ERROR_INVALID;
	/*!
	 * \note
	 * Reset "id", It will be re-used from here
	 */

	if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
		goto out;
	}

	if (type == TYPE_LB && package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_lb_buffer(inst);
		if (!info) {
			goto out;
		}

		ret = buffer_handler_resize(info, w, h);
		/*!
		 * \note
		 * id is resued for newly assigned ID
		 */
		if (ret == LB_STATUS_SUCCESS) {
			id = buffer_handler_id(info);
			instance_set_lb_size(inst, w, h);
			instance_set_lb_info(inst, PRIORITY_NO_CHANGE, CONTENT_NO_CHANGE, TITLE_NO_CHANGE);
		}
	} else if (type == TYPE_PD && package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_pd_buffer(inst);
		if (!info) {
			goto out;
		}

		ret = buffer_handler_resize(info, w, h);
		/*!
		 * \note
		 * id is resued for newly assigned ID
		 */
		if (ret == LB_STATUS_SUCCESS) {
			id = buffer_handler_id(info);
			instance_set_pd_size(inst, w, h);
		}
	}

out:
	result = packet_create_reply(packet, "is", ret, id);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *slave_release_buffer(pid_t pid, int handle, const struct packet *packet)
{
	enum target_type type;
	const char *pkgname;
	const char *id;
	struct packet *result;
	struct slave_node *slave;
	struct inst_info *inst;
	const struct pkg_info *pkg;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		ret = LB_STATUS_ERROR_NOT_EXIST;
		goto out;
	}

	if (packet_get(packet, "iss", &type, &pkgname, &id) != 3) {
		ErrPrint("Inavlid argument\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = validate_request(pkgname, id, &inst, &pkg);
	if (ret != LB_STATUS_SUCCESS) {
		goto out;
	}

	ret = LB_STATUS_ERROR_INVALID;

	if (type == TYPE_LB && package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *info;

		info = instance_lb_buffer(inst);
		ret = buffer_handler_unload(info);
	} else if (type == TYPE_PD && package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *info;
		Ecore_Timer *pd_monitor;

		pd_monitor = instance_del_data(inst, "pd,close,monitor");
		if (!pd_monitor) {
			ErrPrint("Slave requests to release a buffer\n");
			/*!
			 * \note
			 * In this case just keep going to release buffer,
			 * Even if a user(client) doesn't wants to destroy the PD.
			 *
			 * If the slave tries to destroy PD buffer, it should be
			 * released and reported to the client about its status.
			 *
			 * Even if the pd is destroyed by timeout handler,
			 * instance_client_pd_destroyed function will be ignored
			 * by pd.need_to_send_close_event flag.
			 * which will be checked by instance_client_pd_destroyed function.
			 */

			/*!
			 * \note
			 * provider can try to resize the buffer size.
			 * in that case, it will release the buffer first.
			 * Then even though the client doesn't request to close the PD,
			 * the provider can release it.
			 * If we send the close event to the client,
			 * The client will not able to allocate PD again.
			 * In this case, add the pd,monitor again. from here.
			 * to wait the re-allocate buffer.
			 * If the client doesn't request buffer reallocation,
			 * Treat it as a fault. and close the PD.
			 */
			info = instance_pd_buffer(inst);
			ret = buffer_handler_unload(info);

			if (ret == LB_STATUS_SUCCESS) {
				pd_monitor = ecore_timer_add(PD_REQUEST_TIMEOUT, pd_resize_monitor_cb, instance_ref(inst));
				if (!pd_monitor) {
					(void)instance_unref(inst);
					ErrPrint("Failed to create a timer for PD Open monitor\n");
				} else {
					(void)instance_set_data(inst, "pd,resize,monitor", pd_monitor);
					if (slave_event_callback_add(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_resize_buffer_cb, inst) != LB_STATUS_SUCCESS) {
						ErrPrint("Failed to add event handler: %s\n", pkgname);
					}
				}
			}
		} else {
			slave_event_callback_del(slave, SLAVE_EVENT_DEACTIVATE, slave_fault_close_buffer_cb, inst);

			ecore_timer_del(pd_monitor);
			inst = instance_unref(inst);
			if (!inst) {
				ErrPrint("Instance is released: %s\n", pkgname);
				ret = LB_STATUS_ERROR_FAULT;
				goto out;
			}

			info = instance_pd_buffer(inst);
			ret = buffer_handler_unload(info);

			/*!
			 * \note
			 * Send the PD destroyed event to the client
			 */
			instance_client_pd_destroyed(inst, ret);
		}
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *service_change_period(pid_t pid, int handle, const struct packet *packet)
{
	struct inst_info *inst;
	struct packet *result;
	const char *pkgname;
	const char *id;
	double period;
	int ret;

	ret = packet_get(packet, "ssd", &pkgname, &id, &period);
	if (ret != 3) {
		ErrPrint("Invalid packet\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	if (!strlen(id)) {
		struct pkg_info *pkg;

		pkg = package_find(pkgname);
		if (!pkg) {
			ret = LB_STATUS_ERROR_NOT_EXIST;
		} else if (package_is_fault(pkg)) {
			ret = LB_STATUS_ERROR_FAULT;
		} else {
			Eina_List *inst_list;
			Eina_List *l;

			inst_list = package_instance_list(pkg);
			EINA_LIST_FOREACH(inst_list, l, inst) {
				ret = instance_set_period(inst, period);
				if (ret < 0) {
					ErrPrint("Failed to change the period of %s to (%lf)\n", pkgname, period);
				}
			}
		}
	} else {
		ret = validate_request(pkgname, id, &inst, NULL);
		if (ret == LB_STATUS_SUCCESS) {
			if (instance_state(inst) == INST_DESTROYED) {
				ErrPrint("Package[%s] instance is already destroyed\n", pkgname);
				ret = LB_STATUS_ERROR_INVALID;
				goto out;
			}

			ret = instance_set_period(inst, period);
		}
	}

	DbgPrint("Change the update period: %s, %lf : %d\n", pkgname, period, ret);
out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *service_update(pid_t pid, int handle, const struct packet *packet)
{
	Eina_List *inst_list;
	struct pkg_info *pkg;
	struct packet *result;
	const char *pkgname;
	const char *id;
	const char *cluster;
	const char *category;
	char *lb_pkgname;
	int ret;

	ret = packet_get(packet, "ssss", &pkgname, &id, &cluster, &category);
	if (ret != 4) {
		ErrPrint("Invalid Packet\n");
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	lb_pkgname = package_lb_pkgname(pkgname);
	if (!lb_pkgname) {
		ErrPrint("Invalid package %s\n", pkgname);
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	pkg = package_find(lb_pkgname);
	if (!pkg) {
		ret = LB_STATUS_ERROR_NOT_EXIST;
		DbgFree(lb_pkgname);
		goto out;
	}

	if (package_is_fault(pkg)) {
		ret = LB_STATUS_ERROR_FAULT;
		DbgFree(lb_pkgname);
		goto out;
	}

	inst_list = package_instance_list(pkg);
	if (!eina_list_count(inst_list)) {
		ret = LB_STATUS_ERROR_NOT_EXIST;
		DbgFree(lb_pkgname);
		goto out;
	}

	if (id && strlen(id)) {
		Eina_List *l;
		struct inst_info *inst;

		ret = LB_STATUS_ERROR_NOT_EXIST;
		EINA_LIST_FOREACH(inst_list, l, inst) {
			if (!strcmp(instance_id(inst), id)) {
				ret = LB_STATUS_SUCCESS;
				break;
			}
		}

		if (ret == LB_STATUS_ERROR_NOT_EXIST) {
			DbgFree(lb_pkgname);
			goto out;
		}
	}

	/*!
	 * \TODO
	 * Validate the update requstor.
	 */
	slave_rpc_request_update(lb_pkgname, id, cluster, category);
	DbgFree(lb_pkgname);
	ret = LB_STATUS_SUCCESS;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result) {
		ErrPrint("Failed to create a packet\n");
	}

	return result;
}

static struct packet *liveinfo_hello(pid_t pid, int handle, const struct packet *packet)
{
	struct liveinfo *info;
	struct packet *result;
	int ret;
	const char *fifo_name;
	double timestamp;

	DbgPrint("Request arrived from %d\n", pid);

	if (packet_get(packet, "d", &timestamp) != 1) {
		ErrPrint("Invalid packet\n");
		fifo_name = "";
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	info = liveinfo_create(pid, handle);
	if (!info) {
		ErrPrint("Failed to create a liveinfo object\n");
		fifo_name = "";
		ret = LB_STATUS_ERROR_INVALID;
		goto out;
	}

	ret = 0;
	fifo_name = liveinfo_filename(info);
	DbgPrint("FIFO Created: %s (Serve for %d)\n", fifo_name, pid);

out:
	result = packet_create_reply(packet, "si", fifo_name, ret);
	if (!result) {
		ErrPrint("Failed to create a result packet\n");
	}

	return result;
}

static struct packet *liveinfo_slave_list(pid_t pid, int handle, const struct packet *packet)
{
	Eina_List *l;
	Eina_List *list;
	struct liveinfo *info;
	struct slave_node *slave;
	FILE *fp;
	double timestamp;

	if (packet_get(packet, "d", &timestamp) != 1) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	info = liveinfo_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	liveinfo_open_fifo(info);
	fp = liveinfo_fifo(info);
	if (!fp) {
		liveinfo_close_fifo(info);
		goto out;
	}

	list = (Eina_List *)slave_list();
	EINA_LIST_FOREACH(list, l, slave) {
		fprintf(fp, "%d %s %s %s %d %d %d %s %d %d %lf\n", 
			slave_pid(slave),
			slave_name(slave),
			slave_pkgname(slave),
			slave_abi(slave),
			slave_is_secured(slave),
			slave_refcnt(slave),
			slave_fault_count(slave),
			slave_state_string(slave),
			slave_loaded_instance(slave),
			slave_loaded_package(slave),
			slave_ttl(slave)
		);
	}

	fprintf(fp, "EOD\n");
	liveinfo_close_fifo(info);
out:
	return NULL;
}

static inline const char *visible_state_string(enum livebox_visible_state state)
{
	switch (state) {
	case LB_SHOW:
		return "Show";
	case LB_HIDE:
		return "Hide";
	case LB_HIDE_WITH_PAUSE:
		return "Paused";
	default:
		break;
	}

	return "Unknown";
}

static struct packet *liveinfo_inst_list(pid_t pid, int handle, const struct packet *packet)
{
	const char *pkgname;
	struct liveinfo *info;
	struct pkg_info *pkg;
	Eina_List *l;
	Eina_List *inst_list;
	struct inst_info *inst;
	FILE *fp;

	if (packet_get(packet, "s", &pkgname) != 1) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	info = liveinfo_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	liveinfo_open_fifo(info);
	fp = liveinfo_fifo(info);
	if (!fp) {
		ErrPrint("Invalid fp\n");
		liveinfo_close_fifo(info);
		goto out;
	}

	if (!package_is_lb_pkgname(pkgname)) {
		ErrPrint("Invalid package name\n");
		goto close_out;
	}

	pkg = package_find(pkgname);
	if (!pkg) {
		ErrPrint("Package is not exists\n");
		goto close_out;
	}

	inst_list = package_instance_list(pkg);
	EINA_LIST_FOREACH(inst_list, l, inst) {
		fprintf(fp, "%s %s %s %lf %s %d %d\n",
			instance_id(inst),
			instance_cluster(inst),
			instance_category(inst),
			instance_period(inst),
			visible_state_string(instance_visible_state(inst)),
			instance_lb_width(inst),
			instance_lb_height(inst));
	}

close_out:
	fprintf(fp, "EOD\n");
	liveinfo_close_fifo(info);

out:
	return NULL;
}

static struct packet *liveinfo_pkg_list(pid_t pid, int handle, const struct packet *packet)
{
	Eina_List *l;
	Eina_List *list;
	Eina_List *inst_list;
	struct liveinfo *info;
	struct pkg_info *pkg;
	struct slave_node *slave;
	FILE *fp;
	const char *slavename;
	double timestamp;

	if (packet_get(packet, "d", &timestamp) != 1) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	info = liveinfo_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	liveinfo_open_fifo(info);
	fp = liveinfo_fifo(info);
	if (!fp) {
		liveinfo_close_fifo(info);
		goto out;
	}

	list = (Eina_List *)package_list();
	EINA_LIST_FOREACH(list, l, pkg) {
		slave = package_slave(pkg);

		if (slave) {
			slavename = slave_name(slave);
			pid = slave_pid(slave);
		} else {
			pid = (pid_t)-1;
			slavename = "";
		}

		inst_list = (Eina_List *)package_instance_list(pkg);
		fprintf(fp, "%d %s %s %s %d %d %d\n",
			pid,
			strlen(slavename) ? slavename : "(none)",
			package_name(pkg),
			package_abi(pkg),
			package_refcnt(pkg),
			package_fault_count(pkg),
			eina_list_count(inst_list)
		);
	}

	fprintf(fp, "EOD\n");
	liveinfo_close_fifo(info);
out:
	return NULL;
}

static struct packet *liveinfo_slave_ctrl(pid_t pid, int handle, const struct packet *packet)
{
	return NULL;
}

static struct packet *liveinfo_pkg_ctrl(pid_t pid, int handle, const struct packet *packet)
{
	struct liveinfo *info;
	FILE *fp;
	char *cmd;
	char *pkgname;
	char *id;

	if (packet_get(packet, "sss", &cmd, &pkgname, &id) != 3) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	info = liveinfo_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	liveinfo_open_fifo(info);
	fp = liveinfo_fifo(info);
	if (!fp) {
		liveinfo_close_fifo(info);
		goto out;
	}

	if (!strcmp(cmd, "rmpack")) {
		fprintf(fp, "%d\n", ENOSYS);
	} else if (!strcmp(cmd, "rminst")) {
		struct inst_info *inst;
		inst = package_find_instance_by_id(pkgname, id);
		if (!inst) {
			fprintf(fp, "%d\n", ENOENT);
		} else {
			(void)instance_destroy(inst, INSTANCE_DESTROY_DEFAULT);
			fprintf(fp, "%d\n", 0);
		}
	}

	fprintf(fp, "EOD\n");
	liveinfo_close_fifo(info);

out:
	return NULL;
}

static struct packet *liveinfo_master_ctrl(pid_t pid, int handle, const struct packet *packet)
{
	struct liveinfo *info;
	char *cmd;
	char *var;
	char *val;
	FILE *fp;
	int ret = LB_STATUS_ERROR_INVALID;

	if (packet_get(packet, "sss", &cmd, &var, &val) != 3) {
		ErrPrint("Invalid argument\n");
		goto out;
	}

	info = liveinfo_find_by_pid(pid);
	if (!info) {
		ErrPrint("Invalid request\n");
		goto out;
	}

	if (!strcasecmp(var, "debug")) {
		if (!strcasecmp(cmd, "set")) {
			g_conf.debug_mode = !strcasecmp(val, "on");
		} else if (!strcasecmp(cmd, "get")) {
		}
		ret = g_conf.debug_mode;
	} else if (!strcasecmp(var, "slave_max_load")) {
		if (!strcasecmp(cmd, "set")) {
			g_conf.slave_max_load = atoi(val);
		} else if (!strcasecmp(cmd, "get")) {
		}
		ret = g_conf.slave_max_load;
	}

	liveinfo_open_fifo(info);
	fp = liveinfo_fifo(info);
	if (!fp) {
		liveinfo_close_fifo(info);
		goto out;
	}
	fprintf(fp, "%d\nEOD\n", ret);
	liveinfo_close_fifo(info);

out:
	return NULL;
}

static struct method s_info_table[] = {
	{
		.cmd = "liveinfo_hello",
		.handler = liveinfo_hello,
	},
	{
		.cmd = "slave_list",
		.handler = liveinfo_slave_list,
	},
	{
		.cmd = "pkg_list",
		.handler = liveinfo_pkg_list,
	},
	{
		.cmd = "inst_list",
		.handler = liveinfo_inst_list,
	},
	{
		.cmd = "slave_ctrl",
		.handler = liveinfo_slave_ctrl,
	},
	{
		.cmd = "pkg_ctrl",
		.handler = liveinfo_pkg_ctrl,
	},
	{
		.cmd = "master_ctrl",
		.handler = liveinfo_master_ctrl,
	},
	{
		.cmd = NULL,
		.handler = NULL,
	},
};

static struct method s_client_table[] = {
	{
		.cmd = "pd_mouse_move",
		.handler = client_pd_mouse_move, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "lb_mouse_move",
		.handler = client_lb_mouse_move, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "pd_mouse_down",
		.handler = client_pd_mouse_down, /* pid, pkgname, id, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "pd_mouse_up",
		.handler = client_pd_mouse_up, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "lb_mouse_down",
		.handler = client_lb_mouse_down, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "lb_mouse_up",
		.handler = client_lb_mouse_up, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "pd_mouse_enter",
		.handler = client_pd_mouse_enter, /* pid, pkgname, id, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "pd_mouse_leave",
		.handler = client_pd_mouse_leave, /* pid, pkgname, id, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "lb_mouse_enter",
		.handler = client_lb_mouse_enter, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "lb_mouse_leave",
		.handler = client_lb_mouse_leave, /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
	},
	{
		.cmd = "lb_mouse_set",
		.handler = client_lb_mouse_set,
	},
	{
		.cmd = "lb_mouse_unset",
		.handler = client_lb_mouse_unset,
	},
	{
		.cmd = "pd_mouse_set",
		.handler = client_pd_mouse_set,
	},
	{
		.cmd = "pd_mouse_unset",
		.handler = client_pd_mouse_unset,
	},
	{
		.cmd = "change,visibility",
		.handler = client_change_visibility,
	},
	{
		.cmd = "lb_acquire_pixmap",
		.handler = client_lb_acquire_pixmap,
	},
	{
		.cmd = "lb_release_pixmap",
		.handler = client_lb_release_pixmap,
	},
	{
		.cmd = "pd_acquire_pixmap",
		.handler = client_pd_acquire_pixmap,
	},
	{
		.cmd = "pd_release_pixmap",
		.handler = client_pd_release_pixmap,
	},
	{
		.cmd = "acquire",
		.handler = client_acquire, /*!< pid, ret */
	},
	{
		.cmd = "release",
		.handler = cilent_release, /*!< pid, ret */
	},
	{
		.cmd = "clicked",
		.handler = client_clicked, /*!< pid, pkgname, filename, event, timestamp, x, y, ret */
	},
	{
		.cmd = "text_signal",
		.handler = client_text_signal, /* pid, pkgname, filename, emission, source, s, sy, ex, ey, ret */
	},
	{
		.cmd = "delete",
		.handler = client_delete, /* pid, pkgname, filename, ret */
	},
	{
		.cmd = "resize",
		.handler = client_resize, /* pid, pkgname, filename, w, h, ret */
	},
	{
		.cmd = "new",
		.handler = client_new, /* pid, timestamp, pkgname, content, cluster, category, period, ret */
	},
	{
		.cmd = "set_period",
		.handler = client_set_period, /* pid, pkgname, filename, period, ret, period */
	},
	{
		.cmd = "change_group",
		.handler = client_change_group, /* pid, pkgname, filename, cluster, category, ret */
	},
	{
		.cmd = "pinup_changed",
		.handler = client_pinup_changed, /* pid, pkgname, filename, pinup, ret */
	},
	{
		.cmd = "create_pd",
		.handler = client_create_pd, /* pid, pkgname, filename, ret */
	},
	{
		.cmd = "pd_move",
		.handler = client_pd_move, /* pkgname, id, x, y */
	},
	{
		.cmd = "destroy_pd",
		.handler = client_destroy_pd, /* pid, pkgname, filename, ret */
	},
	{
		.cmd = "activate_package",
		.handler = client_activate_package, /* pid, pkgname, ret */
	},
	{
		.cmd = "subscribe", /* pid, cluster, sub-cluster */
		.handler = client_subscribed,
	},
	{
		.cmd = "unsubscribe", /* pid, cluster, sub-cluster */
		.handler = client_unsubscribed,
	},
	{
		.cmd = "delete_cluster",
		.handler = client_delete_cluster,
	},
	{
		.cmd = "delete_category",
		.handler = client_delete_category,
	},
	{
		.cmd = "refresh_group",
		.handler = client_refresh_group,
	},
	{
		.cmd = "update",
		.handler = client_update,
	},

	{
		.cmd = "pd_access_hl",
		.handler = client_pd_access_hl,
	},
	{
		.cmd = "pd_access_hl_prev",
		.handler = client_pd_access_hl_prev,
	},
	{
		.cmd = "pd_access_hl_next",
		.handler = client_pd_access_hl_next,
	},
	{
		.cmd = "pd_access_activate",
		.handler = client_pd_access_activate,
	},
	{
		.cmd = "pd_access_action_up",
		.handler = client_pd_access_action_up,
	},
	{
		.cmd = "pd_access_action_down",
		.handler = client_pd_access_action_down,
	},
	{
		.cmd = "pd_access_unhighlight",
		.handler = client_pd_access_unhighlight,
	},
	{
		.cmd = "pd_access_scroll_down",
		.handler = client_pd_access_scroll_down,
	},
	{
		.cmd = "pd_access_scroll_move",
		.handler = client_pd_access_scroll_move,
	},
	{
		.cmd = "pd_access_scroll_up",
		.handler = client_pd_access_scroll_up,
	},

	{
		.cmd = "lb_access_hl",
		.handler = client_lb_access_hl,
	},
	{
		.cmd = "lb_access_hl_prev",
		.handler = client_lb_access_hl_prev,
	},
	{
		.cmd = "lb_access_hl_next",
		.handler = client_lb_access_hl_next,
	},
	{
		.cmd = "lb_access_activate",
		.handler = client_lb_access_activate,
	},
	{
		.cmd = "lb_access_action_up",
		.handler = client_lb_access_action_up,
	},
	{
		.cmd = "lb_access_action_down",
		.handler = client_lb_access_action_down,
	},
	{
		.cmd = "lb_access_unhighlight",
		.handler = client_lb_access_unhighlight,
	},
	{
		.cmd = "lb_access_scroll_down",
		.handler = client_lb_access_scroll_down,
	},
	{
		.cmd = "lb_access_scroll_move",
		.handler = client_lb_access_scroll_move,
	},
	{
		.cmd = "lb_access_scroll_up",
		.handler = client_lb_access_scroll_up,
	},

	{
		.cmd = "lb_key_down",
		.handler = client_lb_key_down,
	},
	{
		.cmd = "lb_key_up",
		.handler = client_lb_key_up,
	},

	{
		.cmd = "pd_key_down",
		.handler = client_pd_key_down,
	},
	{
		.cmd = "pd_key_up",
		.handler = client_pd_key_up,
	},

	{
		.cmd = "client_paused",
		.handler = client_pause_request,
	},
	{
		.cmd = "client_resumed",
		.handler = client_resume_request,
	},

	{
		.cmd = "update_mode",
		.handler = client_update_mode,
	},

	{
		.cmd = NULL,
		.handler = NULL,
	},
};

static struct method s_service_table[] = {
	{
		.cmd = "service_update",
		.handler = service_update,
	},
	{
		.cmd = "service_change_period",
		.handler = service_change_period,
	},
	{
		.cmd = NULL,
		.handler = NULL,
	},
};

static struct method s_slave_table[] = {
	{
		.cmd = "call",
		.handler = slave_call, /* slave_name, pkgname, filename, function, ret */
	},
	{
		.cmd = "ret",
		.handler = slave_ret, /* slave_name, pkgname, filename, function, ret */
	},
	{
		.cmd = "updated",
		.handler = slave_updated, /* slave_name, pkgname, filename, width, height, priority, ret */
	},
	{
		.cmd = "desc_updated",
		.handler = slave_desc_updated, /* slave_name, pkgname, filename, decsfile, ret */
	},
	{
		.cmd = "deleted",
		.handler = slave_deleted, /* slave_name, pkgname, filename, ret */
	},
	{
		.cmd = "acquire_buffer",
		.handler = slave_acquire_buffer, /* slave_name, id, w, h, size, - out - type, shmid */
	},
	{
		.cmd = "resize_buffer",
		.handler = slave_resize_buffer,
	},
	{
		.cmd = "release_buffer",
		.handler = slave_release_buffer, /* slave_name, id - ret */
	},
	{
		.cmd = "faulted",
		.handler = slave_faulted, /* slave_name, pkgname, id, funcname */
	},
	{
		.cmd = "scroll",
		.handler = slave_hold_scroll, /* slave_name, pkgname, id, seize */
	},

	{
		.cmd = "lb_update_begin",
		.handler = slave_lb_update_begin,
	},
	{
		.cmd = "lb_update_end",
		.handler = slave_lb_update_end,
	},
	{
		.cmd = "pd_update_begin",
		.handler = slave_pd_update_begin,
	},
	{
		.cmd = "pd_update_end",
		.handler = slave_pd_update_end,
	},

	{
		.cmd = "access_status",
		.handler = slave_access_status,
	},

	{
		.cmd = "hello",
		.handler = slave_hello, /* slave_name, ret */
	},
	{
		.cmd = "ping",
		.handler = slave_ping, /* slave_name, ret */
	},

	{
		.cmd = NULL,
		.handler = NULL,
	},
};

HAPI int server_init(void)
{
	com_core_packet_use_thread(COM_CORE_THREAD);

	if (unlink(INFO_SOCKET) < 0) {
		ErrPrint("info socket: %s\n", strerror(errno));
	}

	if (unlink(SLAVE_SOCKET) < 0) {
		ErrPrint("slave socket: %s\n", strerror(errno));
	}

	if (unlink(CLIENT_SOCKET) < 0) {
		ErrPrint("client socket: %s\n", strerror(errno));
	}

	if (unlink(SERVICE_SOCKET) < 0) {
		ErrPrint("service socket: %s\n", strerror(errno));
	}

	s_info.info_fd = com_core_packet_server_init(INFO_SOCKET, s_info_table);
	if (s_info.info_fd < 0) {
		ErrPrint("Failed to create a info socket\n");
	}

	s_info.slave_fd = com_core_packet_server_init(SLAVE_SOCKET, s_slave_table);
	if (s_info.slave_fd < 0) {
		ErrPrint("Failed to create a slave socket\n");
	}

	s_info.client_fd = com_core_packet_server_init(CLIENT_SOCKET, s_client_table);
	if (s_info.client_fd < 0) {
		ErrPrint("Failed to create a client socket\n");
	}

	/*!
	 * \note
	 * remote://:8208
	 * Skip address to use the NULL.
	 */
	s_info.remote_client_fd = com_core_packet_server_init("remote://:"CLIENT_PORT, s_client_table);
	if (s_info.client_fd < 0) {
		ErrPrint("Failed to create a remote client socket\n");
	}

	s_info.service_fd = com_core_packet_server_init(SERVICE_SOCKET, s_service_table);
	if (s_info.service_fd < 0) {
		ErrPrint("Faild to create a service socket\n");
	}

	if (chmod(INFO_SOCKET, 0600) < 0) {
		ErrPrint("info socket: %s\n", strerror(errno));
	}

	if (chmod(SLAVE_SOCKET, 0666) < 0) {
		ErrPrint("slave socket: %s\n", strerror(errno));
	}

	if (chmod(CLIENT_SOCKET, 0666) < 0) {
		ErrPrint("client socket: %s\n", strerror(errno));
	}

	if (chmod(SERVICE_SOCKET, 0666) < 0) {
		ErrPrint("service socket: %s\n", strerror(errno));
	}

	return 0;
}

HAPI int server_fini(void)
{
	if (s_info.info_fd > 0) {
		com_core_packet_server_fini(s_info.info_fd);
		s_info.info_fd = -1;
	}

	if (s_info.slave_fd > 0) {
		com_core_packet_server_fini(s_info.slave_fd);
		s_info.slave_fd = -1;
	}

	if (s_info.client_fd > 0) {
		com_core_packet_server_fini(s_info.client_fd);
		s_info.client_fd = -1;
	}

	if (s_info.remote_client_fd > 0) {
		com_core_packet_server_fini(s_info.remote_client_fd);
		s_info.remote_client_fd = -1;
	}

	if (s_info.service_fd > 0) {
		com_core_packet_server_fini(s_info.service_fd);
		s_info.service_fd = -1;
	}

	return 0;
}

/* End of a file */
