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
#include <stdlib.h>
#include <errno.h>

#include <dlog.h>
#include <Ecore_Evas.h>
#include <Eina.h>
#include <gio/gio.h>
#include <Ecore.h>

#include <packet.h>
#include <com-core_packet.h>
#include <livebox-service.h>

#include "conf.h"
#include "util.h"
#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "package.h"
#include "script_handler.h"
#include "buffer_handler.h"
#include "fb.h"
#include "setting.h"

int errno;

static struct info {
	enum buffer_type env_buf_type;
} s_info = {
	.env_buf_type = BUFFER_TYPE_FILE,
};

struct set_pinup_cbdata {
	struct inst_info *inst;
	int pinup;
};

struct resize_cbdata {
	struct inst_info *inst;
	int w;
	int h;
};

struct change_group_cbdata {
	struct inst_info *inst;
	char *cluster;
	char *category;
};

struct period_cbdata {
	struct inst_info *inst;
	double period;
};

struct inst_info {
	struct pkg_info *info;

	enum instance_state state; /*!< Represents current state */
	enum instance_state requested_state; /*!< Only ACTIVATED | DESTROYED is acceptable */
	int changing_state;

	char *id;
	double timestamp;

	char *content;
	char *cluster;
	char *category;
	char *title;
	int is_pinned_up;
	double sleep_at;

	enum livebox_visible_state visible;

	struct {
		int width;
		int height;
		double priority;

		union {
			struct script_info *script;
			struct buffer_info *buffer;
		} canvas;

		const char *auto_launch;
	} lb;

	struct {
		int width;
		int height;
		double x;
		double y;

		union {
			struct script_info *script;
			struct buffer_info *buffer;
		} canvas;

		struct client_node *owner;
		int is_opened_for_reactivate;
		int need_to_send_close_event;
		char *pended_update_desc;
		int pended_update_cnt;
	} pd;

	int timeout;
	double period;

	struct client_node *client; /*!< Owner - creator */
	Eina_List *client_list; /*!< Viewer list */
	int refcnt;

	Ecore_Timer *update_timer; /*!< Only used for secured livebox */
};

#define CLIENT_SEND_EVENT(instance, packet)	((instance)->client ? client_rpc_async_request((instance)->client, (packet)) : client_broadcast((instance), (packet)))

static Eina_Bool update_timer_cb(void *data);

static inline void timer_thaw(struct inst_info *inst)
{
	struct timeval tv;
	double pending;
	double compensate;
	double sleep_time;

	ecore_timer_thaw(inst->update_timer);

	if (inst->sleep_at == 0.0f)
		return;

	pending = ecore_timer_pending_get(inst->update_timer);

	if (gettimeofday(&tv, NULL) < 0) {
		ErrPrint("Failed to get timeofday: %s\n", strerror(errno));
		return;
	}
	compensate = 60.0f - ((double)(tv.tv_sec % 60) + ((double)tv.tv_usec / 1000000.0f));
	sleep_time = (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0f) - inst->sleep_at;

	ecore_timer_delay(inst->update_timer, compensate - pending);
	DbgPrint("Compensate: %lf\n", compensate - pending);

	if (sleep_time > pending) {
		DbgPrint("Update time elapsed\n");
		(void)update_timer_cb(inst);
	}

	inst->sleep_at = 0.0f;
}

static inline void timer_freeze(struct inst_info *inst)
{
	struct timeval tv;
	ecore_timer_freeze(inst->update_timer);

	if (ecore_timer_interval_get(inst->update_timer) <= 1.0f)
		return;

	gettimeofday(&tv, NULL);
	inst->sleep_at = (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
}


static int viewer_deactivated_cb(struct client_node *client, void *data)
{
	struct inst_info *inst = data;

	DbgPrint("%d is deleted from the list of viewer of %s(%s)\n", client_pid(client), package_name(instance_package(inst)), instance_id(inst));
	if (!eina_list_data_find(inst->client_list, client)) {
		DbgPrint("Not found\n");
		return -ENOENT;
	}

	inst->client_list = eina_list_remove(inst->client_list, client);
	if (!inst->client_list && !inst->client) {
		DbgPrint("Has no clients\n");
		instance_destroy(inst);
	}

	instance_unref(inst);
	return -1; /*!< Remove this callback from the cb list */
}

static inline int pause_livebox(struct inst_info *inst)
{
	struct packet *packet;

	packet = packet_create_noack("lb_pause", "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Failed to create a new packet\n");
		return -EFAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

/*! \TODO Wake up the freeze'd timer */
static inline int resume_livebox(struct inst_info *inst)
{
	struct packet *packet;

	packet = packet_create_noack("lb_resume", "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Failed to create a new packet\n");
		return -EFAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

static inline int instance_recover_visible_state(struct inst_info *inst)
{
	int ret;

	switch (inst->visible) {
	case LB_SHOW:
	case LB_HIDE:
		instance_thaw_updator(inst);

		ret = 0;
		break;
	case LB_HIDE_WITH_PAUSE:
		ret = pause_livebox(inst);

		instance_freeze_updator(inst);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	DbgPrint("Visible state is recovered to %d\n", ret);
	return ret;
}

HAPI int instance_unicast_created_event(struct inst_info *inst, struct client_node *client)
{
	struct packet *packet;
	enum lb_type lb_type;
	enum pd_type pd_type;
	const char *lb_file;
	const char *pd_file;

	if (!client) {
		client = inst->client;
		if (!client)
			return 0;
	}

	lb_type = package_lb_type(inst->info);
	pd_type = package_pd_type(inst->info);

	if (lb_type == LB_TYPE_SCRIPT)
		lb_file = fb_id(script_handler_fb(inst->lb.canvas.script));
	else if (lb_type == LB_TYPE_BUFFER)
		lb_file = buffer_handler_id(inst->lb.canvas.buffer);
	else
		lb_file = "";

	if (pd_type == PD_TYPE_SCRIPT)
		pd_file = fb_id(script_handler_fb(inst->pd.canvas.script));
	else if (pd_type == PD_TYPE_BUFFER)
		pd_file = buffer_handler_id(inst->pd.canvas.buffer);
	else
		pd_file = "";

	packet = packet_create_noack("created", "dsssiiiisssssdiiiiidsi",
			inst->timestamp,
			package_name(inst->info), inst->id, inst->content,
			inst->lb.width, inst->lb.height,
			inst->pd.width, inst->pd.height,
			inst->cluster, inst->category,
			lb_file, pd_file,
			inst->lb.auto_launch,
			inst->lb.priority,
			package_size_list(inst->info),
			!!inst->client,
			package_pinup(inst->info),
			lb_type, pd_type,
			inst->period, inst->title,
			inst->is_pinned_up);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	return client_rpc_async_request(client, packet);
}

static int update_client_list(struct client_node *client, void *data)
{
	struct inst_info *inst = data;
	if (!instance_has_client(inst, client)) {
		instance_add_client(inst, client);
	}
	return 0;
}

static int instance_broadcast_created_event(struct inst_info *inst)
{
	struct packet *packet;
	enum lb_type lb_type;
	enum pd_type pd_type;
	const char *lb_file;
	const char *pd_file;

	lb_type = package_lb_type(inst->info);
	pd_type = package_pd_type(inst->info);

	if (lb_type == LB_TYPE_SCRIPT)
		lb_file = fb_id(script_handler_fb(inst->lb.canvas.script));
	else if (lb_type == LB_TYPE_BUFFER)
		lb_file = buffer_handler_id(inst->lb.canvas.buffer);
	else
		lb_file = "";

	if (pd_type == PD_TYPE_SCRIPT)
		pd_file = fb_id(script_handler_fb(inst->pd.canvas.script));
	else if (pd_type == PD_TYPE_BUFFER)
		pd_file = buffer_handler_id(inst->pd.canvas.buffer);
	else
		pd_file = "";

	if (!inst->client)
		client_browse_list(inst->cluster, inst->category, update_client_list, inst);

	packet = packet_create_noack("created", "dsssiiiisssssdiiiiidsi", 
			inst->timestamp,
			package_name(inst->info), inst->id, inst->content,
			inst->lb.width, inst->lb.height,
			inst->pd.width, inst->pd.height,
			inst->cluster, inst->category,
			lb_file, pd_file,
			inst->lb.auto_launch,
			inst->lb.priority,
			package_size_list(inst->info),
			!!inst->client,
			package_pinup(inst->info),
			lb_type, pd_type,
			inst->period, inst->title,
			inst->is_pinned_up);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	return CLIENT_SEND_EVENT(inst, packet);
}

HAPI int instance_unicast_deleted_event(struct inst_info *inst, struct client_node *client)
{
	struct packet *packet;

	if (!client) {
		client = inst->client;
		if (!client)
			return -EINVAL;
	}

	packet = packet_create_noack("deleted", "ssd", package_name(inst->info), inst->id, inst->timestamp);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}
		
	return client_rpc_async_request(client, packet);
}

static int instance_broadcast_deleted_event(struct inst_info *inst)
{
	struct packet *packet;
	struct client_node *client;
	Eina_List *l;
	Eina_List *n;
	int ret;

	packet = packet_create_noack("deleted", "ssd", package_name(inst->info), inst->id, inst->timestamp);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}
		
	ret = CLIENT_SEND_EVENT(inst, packet);

	EINA_LIST_FOREACH_SAFE(inst->client_list, l, n, client) {
		instance_del_client(inst, client);
	}

	return ret;
}

static int client_deactivated_cb(struct client_node *client, void *data)
{
	struct inst_info *inst = data;
	instance_destroy(inst);
	return 0;
}

static int send_pd_destroyed_to_client(struct inst_info *inst, int status)
{
	struct packet *packet;

	packet = packet_create_noack("pd_destroyed", "ssi", package_name(inst->info), inst->id, status);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	return CLIENT_SEND_EVENT(inst, packet);
}

static inline void destroy_instance(struct inst_info *inst)
{
	struct pkg_info *pkg;
	enum lb_type lb_type;
	enum pd_type pd_type;
	struct slave_node *slave;

	pkg = inst->info;

	lb_type = package_lb_type(pkg);
	pd_type = package_pd_type(pkg);
	slave = package_slave(inst->info);

	DbgPrint("Instance is destroyed (%p), slave(%p)\n", inst, slave);

	if (inst->pd.need_to_send_close_event)
		send_pd_destroyed_to_client(inst, 0);

	if (lb_type == LB_TYPE_SCRIPT) {
		script_handler_unload(inst->lb.canvas.script, 0);
		script_handler_destroy(inst->lb.canvas.script);
	} else if (lb_type == LB_TYPE_BUFFER) {
		buffer_handler_unload(inst->lb.canvas.buffer);
		buffer_handler_destroy(inst->lb.canvas.buffer);
	}

	if (pd_type == PD_TYPE_SCRIPT) {
		script_handler_unload(inst->pd.canvas.script, 1);
		script_handler_destroy(inst->pd.canvas.script);
	} else if (pd_type == PD_TYPE_BUFFER) {
		buffer_handler_unload(inst->pd.canvas.buffer);
		buffer_handler_destroy(inst->pd.canvas.buffer);
	}

	if (inst->client) {
		client_event_callback_del(inst->client, CLIENT_EVENT_DEACTIVATE, client_deactivated_cb, inst);
		client_unref(inst->client);
	}

	if (inst->update_timer)
		ecore_timer_del(inst->update_timer);

	/*!
	 * \note
	 *
	 * inst->lb.auto_launch
	 *
	 * will be released by the package object
	 * it is readonly value for instances
	 */
	DbgFree(inst->category);
	DbgFree(inst->cluster);
	DbgFree(inst->content);
	DbgFree(inst->title);
	util_unlink(inst->id);
	DbgFree(inst->id);
	package_del_instance(inst->info, inst);
	DbgFree(inst);

	slave = slave_unload_instance(slave);
}

static Eina_Bool update_timer_cb(void *data)
{
	struct inst_info *inst = (struct inst_info *)data;

	DbgPrint("Update instance %s (%s) %s/%s\n", package_name(inst->info), inst->id, inst->cluster, inst->category);
	slave_rpc_request_update(package_name(inst->info), inst->id, inst->cluster, inst->category);
	return ECORE_CALLBACK_RENEW;
}

static inline int fork_package(struct inst_info *inst, const char *pkgname)
{
	struct pkg_info *info;
	int len;

	info = package_find(pkgname);
	if (!info) {
		ErrPrint("%s is not found\n", pkgname);
		return -ENOENT;
	}

	len = strlen(SCHEMA_FILE "%s%s_%d_%lf.png") + strlen(IMAGE_PATH) + strlen(package_name(info)) + 50;
	inst->id = malloc(len);
	if (!inst->id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	snprintf(inst->id, len, SCHEMA_FILE "%s%s_%d_%lf.png", IMAGE_PATH, package_name(info), client_pid(inst->client), inst->timestamp);
	inst->lb.auto_launch = package_auto_launch(info);

	instance_set_pd_info(inst, package_pd_width(info), package_pd_height(info));

	inst->timeout = package_timeout(info);
	inst->period = package_period(info);

	inst->info = info;

	if (package_secured(info)) {
		DbgPrint("Register the update timer for secured livebox [%s]\n", package_name(info));
		inst->update_timer = util_timer_add(inst->period, update_timer_cb, inst);
		if (!inst->update_timer)
			ErrPrint("Failed to add an update timer for instance %s\n", inst->id);
		else
			timer_freeze(inst); /* Freeze the update timer as default */
	}

	return 0;
}

HAPI struct inst_info *instance_create(struct client_node *client, double timestamp, const char *pkgname, const char *content, const char *cluster, const char *category, double period, int width, int height)
{
	struct inst_info *inst;

	inst = calloc(1, sizeof(*inst));
	if (!inst) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	inst->timestamp = timestamp;
	inst->lb.width = width;
	inst->lb.height = height;

	inst->content = strdup(content);
	if (!inst->content) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(inst);
		return NULL;
	}

	inst->cluster = strdup(cluster);
	if (!inst->cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(inst->content);
		DbgFree(inst);
		return NULL;
	}

	inst->category = strdup(category);
	if (!inst->category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(inst->cluster);
		DbgFree(inst->content);
		DbgFree(inst);
		return NULL;
	}

	inst->title = strdup(DEFAULT_TITLE); /*!< Use the DEFAULT Title "" */
	if (!inst->title) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(inst->category);
		DbgFree(inst->cluster);
		DbgFree(inst->content);
		DbgFree(inst);
		return NULL;
	}

	if (client) {
		inst->client = client_ref(client);
		client_event_callback_add(inst->client, CLIENT_EVENT_DEACTIVATE, client_deactivated_cb, inst);
	}

	if (fork_package(inst, pkgname) < 0) {
		client_event_callback_del(inst->client, CLIENT_EVENT_DEACTIVATE, client_deactivated_cb, inst);
		client_unref(inst->client);
		DbgFree(inst->title);
		DbgFree(inst->category);
		DbgFree(inst->cluster);
		DbgFree(inst->content);
		DbgFree(inst);
		return NULL;
	}

	inst->state = INST_INIT;
	inst->requested_state = INST_INIT;
	instance_ref(inst);

	if (package_add_instance(inst->info, inst) < 0) {
		instance_state_reset(inst);
		instance_destroy(inst);
		return NULL;
	}

	slave_load_instance(package_slave(inst->info));

	if (instance_activate(inst) < 0) {
		instance_state_reset(inst);
		instance_destroy(inst);
		inst = NULL;
	}

	return inst;
}

HAPI struct inst_info *instance_ref(struct inst_info *inst)
{
	if (!inst)
		return NULL;

	inst->refcnt++;
	return inst;
}

HAPI struct inst_info *instance_unref(struct inst_info *inst)
{
	if (!inst)
		return NULL;

	if (inst->refcnt == 0) {
		ErrPrint("Instance refcnt is not valid\n");
		return NULL;
	}

	inst->refcnt--;
	if (inst->refcnt == 0) {
		destroy_instance(inst);
		inst = NULL;
	}

	return inst;
}

static void deactivate_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct inst_info *inst = data;
	struct pkg_info *info;
	int ret;

	/*!
	 * \note
	 * In this callback, we cannot trust the "client" information.
	 * It could be cleared before reach to here.
	 */

	if (!packet) {
		DbgPrint("Consuming a request of a dead process\n");
		/*!
		 * \note
		 * The instance_reload will care this.
		 * And it will be called from the slave activate callback.
		 */
		inst->changing_state = 0;
		instance_unref(inst);
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid argument\n");
		inst->changing_state = 0;
		instance_unref(inst);
		return;
	}

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
		inst->changing_state = 0;
		instance_unref(inst);
		return;
	}

	switch (ret) {
	case 0:
		/*!
		 * \note
		 * Successfully unloaded
		 */
		switch (inst->requested_state) {
		case INST_ACTIVATED:
			DbgPrint("REQ: ACTIVATED\n");
			instance_state_reset(inst);
			instance_reactivate(inst);
			break;
		case INST_DESTROYED:
			info = inst->info;
			instance_broadcast_deleted_event(inst);
			instance_state_reset(inst);
			instance_destroy(inst);
			DbgPrint("== %s\n", package_name(info));
		default:
			/*!< Unable to reach here */
			break;
		}

		break;
	case -EINVAL:
		/*!
		 * \note
		 * Slave has no instance of this package.
		 */
	case -ENOENT:
		/*!
		 * \note
		 * This instance's previous state is only can be the INST_ACTIVATED.
		 * So we should care the slave_unload_instance from here.
		 * And we should send notification to clients, about this is deleted.
		 */
		/*!
		 * \note
		 * Slave has no instance of this.
		 * In this case, ignore the requested_state
		 * Because, this instance is already met a problem.
		 */
	default:
		/*!
		 * \note
		 * Failed to unload this instance.
		 * This is not possible, slave will always return -ENOENT, -EINVAL, or 0.
		 * but care this exceptional case.
		 */
		DbgPrint("[%s] instance destroying ret(%d)\n", package_name(inst->info), ret);
		info = inst->info;
		instance_broadcast_deleted_event(inst);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	}

	inst->changing_state = 0;
	instance_unref(inst);
}

static void reactivate_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct inst_info *inst = data;
	struct pkg_info *info;
	enum lb_type lb_type;
	enum pd_type pd_type;
	int ret;
	const char *content;
	const char *title;
	int is_pinned_up;

	if (!packet) {
		DbgPrint("Consuming a request of a dead process\n");
		/*!
		 * \note
		 * instance_reload function will care this.
		 * and it will be called from the slave_activate callback
		 */
		goto out;
	}

	if (packet_get(packet, "issi", &ret, &content, &title, &is_pinned_up) != 4) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	if (strlen(content)) {
		char *tmp;

		tmp = strdup(content);
		if (!tmp) {
			ErrPrint("Heap: %s\n", strerror(errno));
			goto out;
		}

		DbgFree(inst->content);
		inst->content = tmp;

		DbgPrint("Update content info %s\n", tmp);
	}

	if (strlen(title)) {
		char *tmp;

		tmp = strdup(title);
		if (!tmp) {
			ErrPrint("Heap: %s\n", strerror(errno));
			goto out;
		}

		DbgFree(inst->title);
		inst->title = tmp;

		DbgPrint("Update title info %s\n", tmp);
	}

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
		goto out;
	}

	switch (ret) {
	case 0: /*!< normally created */
		inst->state = INST_ACTIVATED;
		switch (inst->requested_state) {
		case INST_DESTROYED:
			instance_destroy(inst);
			break;
		case INST_ACTIVATED:
			inst->is_pinned_up = is_pinned_up;
			info = inst->info;
			lb_type = package_lb_type(info);
			pd_type = package_pd_type(info);

			/*!
			 * \note
			 * Optimization point.
			 *   In case of the BUFFER type,
			 *   the slave will request the buffer to render its contents.
			 *   so the buffer will be automatcially recreated when it gots the
			 *   buffer request packet.
			 *   so load a buffer from here is not neccessary.
			 *   I should to revise it and concrete the concept.
			 *   Just leave it only for now.
			 */

			if (lb_type == LB_TYPE_SCRIPT && inst->lb.canvas.script) {
				script_handler_load(inst->lb.canvas.script, 0);
			} else if (lb_type == LB_TYPE_BUFFER && inst->lb.canvas.buffer) {
				buffer_handler_load(inst->lb.canvas.buffer);
			}

			if (pd_type == PD_TYPE_SCRIPT && inst->pd.canvas.script && inst->pd.is_opened_for_reactivate) {
				double x, y;

				instance_slave_get_pd_pos(inst, &x, &y);

				/*!
				 * \note
				 * We should to send a request to open a PD to slave.
				 * if we didn't send it, the slave will not recognize the state of a PD.
				 * We have to keep the view of PD seamless even if the livebox is reactivated.
				 * To do that, send open request from here.
				 */
				ret = instance_slave_open_pd(inst, NULL);

				/*!
				 * \note
				 * In this case, master already loads the PD script.
				 * So just send the pd,show event to the slave again.
				 */
				ret = instance_signal_emit(inst,
						"pd,show", util_uri_to_path(instance_id(inst)),
						0.0, 0.0, 0.0, 0.0, x, y, 0);
			} else if (pd_type == PD_TYPE_BUFFER && inst->pd.canvas.buffer && inst->pd.is_opened_for_reactivate) {
				double x, y;

				buffer_handler_load(inst->pd.canvas.buffer);
				instance_slave_get_pd_pos(inst, &x, &y);

				/*!
				 * \note
				 * We should to send a request to open a PD to slave.
				 * if we didn't send it, the slave will not recognize the state of a PD.
				 * We have to keep the view of PD seamless even if the livebox is reactivated.
				 * To do that, send open request from here.
				 */
				ret = instance_slave_open_pd(inst, NULL);

				/*!
				 * \note
				 * In this case, just send the pd,show event for keeping the compatibility
				 */
				ret = instance_signal_emit(inst,
						"pd,show", util_uri_to_path(instance_id(inst)),
						0.0, 0.0, 0.0, 0.0, x, y, 0);
			}

			/*!
			 * \note
			 * After create an instance again,
			 * Send resize request to the livebox.
			 * instance_resize(inst, inst->lb.width, inst->lb.height);
			 *
			 * renew request will resize the livebox while creating it again
			 */

			/*!
			 * \note
			 * This function will check the visiblity of a livebox and
			 * make decision whether it thaw the update timer or not.
			 */
			instance_recover_visible_state(inst);
		default:
			break;
		}
		break;
	default:
		info = inst->info;
		DbgPrint("[%s] instance destroying ret(%d)\n", package_name(info), ret);
		instance_broadcast_deleted_event(inst);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	}

out:
	inst->changing_state = 0;
	instance_unref(inst);
}

static void activate_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct inst_info *inst = data;
	int ret;
	int w;
	int h;
	double priority;
	char *content;
	char *title;
	int is_pinned_up;

	if (!packet) {
		DbgPrint("Consuming a request of a dead process\n");
		/*!
		 * \note
		 * instance_reload will care this
		 * it will be called from the slave_activate callback
		 */
		goto out;
	}

	if (packet_get(packet, "iiidssi", &ret, &w, &h, &priority, &content, &title, &is_pinned_up) != 7) {
		ErrPrint("Invalid parameter\n");
		goto out;
	}

	if (inst->state == INST_DESTROYED) {
		/*!
		 * \note
		 * Already destroyed.
		 * Do nothing at here anymore.
		 */
		goto out;
	}

	switch (ret) {
	case 1: /*!< need to create */
		if (util_free_space(IMAGE_PATH) > MINIMUM_SPACE) {
			struct inst_info *new_inst;
			new_inst = instance_create(inst->client, util_timestamp(), package_name(inst->info),
							inst->content, inst->cluster, inst->category,
							inst->period, 0, 0);
			if (!new_inst)
				ErrPrint("Failed to create a new instance\n");
		} else {
			ErrPrint("Not enough space\n");
		}
	case 0: /*!< normally created */
		/*!
		 * \note
		 * Anyway this instance is loaded to the slave,
		 * so just increase the loaded instance counter
		 * After that, do reset jobs.
		 */
		inst->state = INST_ACTIVATED;

		instance_set_lb_info(inst, w, h, priority, content, title);

		switch (inst->requested_state) {
		case INST_DESTROYED:
			instance_unicast_deleted_event(inst, NULL);
			instance_state_reset(inst);
			instance_destroy(inst);
			break;
		case INST_ACTIVATED:
		default:
			/*!
			 * \note
			 * LB should be created at the create time
			 */
			inst->is_pinned_up = is_pinned_up;
			if (package_lb_type(inst->info) == LB_TYPE_SCRIPT) {
				if (inst->lb.width == 0 && inst->lb.height == 0)
					livebox_service_get_size(LB_SIZE_TYPE_1x1, &inst->lb.width, &inst->lb.height);

				inst->lb.canvas.script = script_handler_create(inst,
								package_lb_path(inst->info),
								package_lb_group(inst->info),
								inst->lb.width, inst->lb.height);

				if (!inst->lb.canvas.script)
					ErrPrint("Failed to create LB\n");
				else
					script_handler_load(inst->lb.canvas.script, 0);
			} else if (package_lb_type(inst->info) == LB_TYPE_BUFFER) {
				instance_create_lb_buffer(inst);
			}

			if (package_pd_type(inst->info) == PD_TYPE_SCRIPT) {
				if (inst->pd.width == 0 && inst->pd.height == 0)
					instance_set_pd_info(inst, package_pd_width(inst->info), package_pd_height(inst->info));

				inst->pd.canvas.script = script_handler_create(inst,
								package_pd_path(inst->info),
								package_pd_group(inst->info),
								inst->pd.width, inst->pd.height);

				if (!inst->pd.canvas.script)
					ErrPrint("Failed to create PD\n");
			} else if (package_pd_type(inst->info) == PD_TYPE_BUFFER) {
				instance_create_pd_buffer(inst);
			}

			instance_broadcast_created_event(inst);

			instance_thaw_updator(inst);
			break;
		}
		break;
	default:
		DbgPrint("[%s] instance destroying ret(%d)\n", package_name(inst->info), ret);
		instance_unicast_deleted_event(inst, NULL);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	}

out:
	inst->changing_state = 0;
	instance_unref(inst);
}

HAPI int instance_create_pd_buffer(struct inst_info *inst)
{
	if (inst->pd.width == 0 && inst->pd.height == 0)
		instance_set_pd_info(inst, package_pd_width(inst->info), package_pd_height(inst->info));

	if (!inst->pd.canvas.buffer) {
		inst->pd.canvas.buffer = buffer_handler_create(inst, s_info.env_buf_type, inst->pd.width, inst->pd.height, sizeof(int));
		if (!inst->pd.canvas.buffer)
			ErrPrint("Failed to create PD Buffer\n");
	}

	return !!inst->pd.canvas.buffer;
}

HAPI int instance_create_lb_buffer(struct inst_info *inst)
{
	if (inst->lb.width == 0 && inst->lb.height == 0)
		livebox_service_get_size(LB_SIZE_TYPE_1x1, &inst->lb.width, &inst->lb.height);

	if (!inst->lb.canvas.buffer) {
		/*!
		 * \note
		 * Slave doesn't call the acquire_buffer.
		 * In this case, create the buffer from here.
		 */
		inst->lb.canvas.buffer = buffer_handler_create(inst, s_info.env_buf_type, inst->lb.width, inst->lb.height, sizeof(int));
		if (!inst->lb.canvas.buffer)
			ErrPrint("Failed to create LB\n");
	}

	return !!inst->lb.canvas.buffer;
}

HAPI int instance_destroy(struct inst_info *inst)
{
	struct packet *packet;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	switch (inst->state) {
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_DESTROY:
	case INST_REQUEST_TO_REACTIVATE:
		inst->requested_state = INST_DESTROYED;
		return 0;
	case INST_INIT:
		inst->state = INST_DESTROYED;
		inst->requested_state = INST_DESTROYED;
		instance_unref(inst);
		return 0;
	case INST_DESTROYED:
		inst->requested_state = INST_DESTROYED;
		return 0;
	default:
		break;
	}

	packet = packet_create("delete", "ss", package_name(inst->info), inst->id);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	inst->requested_state = INST_DESTROYED;
	inst->state = INST_REQUEST_TO_DESTROY;
	inst->changing_state = 1;
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, deactivate_cb, instance_ref(inst), 0);
}

/* Client Deactivated Callback */
static int pd_buffer_close_cb(struct client_node *client, void *inst)
{
	int ret;

	ret = instance_slave_close_pd(inst, client);
	DbgPrint("Forcely close the PD ret: %d\n", ret);
	return -1; /* Delete this callback */
}

/* Client Deactivated Callback */
static int pd_script_close_cb(struct client_node *client, void *inst)
{
	int ret;

	ret = instance_slave_close_pd(inst, client);
	DbgPrint("Forcely close the PD ret: %d\n", ret);

	ret = script_handler_unload(instance_pd_script(inst), 1);
	return -1; /* Delete this callback */
}

static inline void release_resource_for_closing_pd(struct pkg_info *info, struct inst_info *inst, struct client_node *client)
{
	if (!client) {
		client = inst->pd.owner;
		if (!client)
			return;
	}

	/*!
	 * \note
	 * Clean up the resources
	 */
	if (package_pd_type(info) == PD_TYPE_BUFFER) {
		if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, pd_buffer_close_cb, inst) == 0) {
			/*!
			 * \note
			 * Only if this function succeed to remove the pd_buffer_close_cb,
			 * Decrease the reference count of this instance
			 */
		}
		instance_unref(inst);
	} else if (package_pd_type(info) == PD_TYPE_SCRIPT) {
		if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, pd_script_close_cb, inst) == 0) {
			/*!
			 * \note
			 * Only if this function succeed to remove the script_close_cb,
			 * Decrease the reference count of this instance
			 */
		}
		instance_unref(inst);
	} else {
		ErrPrint("Unknown PD type\n");
	}

}

HAPI int instance_state_reset(struct inst_info *inst)
{
	enum lb_type lb_type;
	enum pd_type pd_type;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (inst->state == INST_DESTROYED)
		return 0;

	lb_type = package_lb_type(inst->info);
	pd_type = package_pd_type(inst->info);

	if (lb_type == LB_TYPE_SCRIPT && inst->lb.canvas.script)
		script_handler_unload(inst->lb.canvas.script, 0);
	else if (lb_type == LB_TYPE_BUFFER && inst->lb.canvas.buffer)
		buffer_handler_unload(inst->lb.canvas.buffer);

	if (pd_type == PD_TYPE_SCRIPT && inst->pd.canvas.script) {
		inst->pd.is_opened_for_reactivate = script_handler_is_loaded(inst->pd.canvas.script);
		release_resource_for_closing_pd(instance_package(inst), inst, NULL);
		script_handler_unload(inst->pd.canvas.script, 1);
	} else if (pd_type == PD_TYPE_BUFFER && inst->pd.canvas.buffer) {
		inst->pd.is_opened_for_reactivate = buffer_handler_is_loaded(inst->pd.canvas.buffer);
		release_resource_for_closing_pd(instance_package(inst), inst, NULL);
		buffer_handler_unload(inst->pd.canvas.buffer);
	}

	inst->state = INST_INIT;
	inst->requested_state = INST_INIT;
	return 0;
}

HAPI int instance_reactivate(struct inst_info *inst)
{
	struct packet *packet;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	switch (inst->state) {
	case INST_REQUEST_TO_DESTROY:
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_REACTIVATE:
		inst->requested_state = INST_ACTIVATED;
		return 0;
	case INST_DESTROYED:
	case INST_ACTIVATED:
		return 0;
	case INST_INIT:
	default:
		break;
	}

	packet = packet_create("renew", "sssiidssiis",
			package_name(inst->info),
			inst->id,
			inst->content,
			inst->timeout,
			!!package_lb_path(inst->info),
			inst->period,
			inst->cluster,
			inst->category,
			inst->lb.width, inst->lb.height,
			package_abi(inst->info));
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	ret = slave_activate(package_slave(inst->info));
	if (ret < 0 && ret != -EALREADY) {
		/*!
		 * \note
		 * If the master failed to launch the slave,
		 * Do not send any requests to the slave.
		 */
		ErrPrint("Failed to launch the slave\n");
		packet_destroy(packet);
		return ret;
	}

	inst->requested_state = INST_ACTIVATED;
	inst->state = INST_REQUEST_TO_REACTIVATE;
	inst->changing_state = 1;

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, reactivate_cb, instance_ref(inst), 1);
}

HAPI int instance_activate(struct inst_info *inst)
{
	struct packet *packet;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	switch (inst->state) {
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_ACTIVATE:
	case INST_REQUEST_TO_DESTROY:
		inst->requested_state = INST_ACTIVATED;
		return 0;
	case INST_ACTIVATED:
	case INST_DESTROYED:
		return 0;
	case INST_INIT:
	default:
		break;
	}

	packet = packet_create("new", "sssiidssisii",
			package_name(inst->info),
			inst->id,
			inst->content,
			inst->timeout,
			!!package_lb_path(inst->info),
			inst->period,
			inst->cluster,
			inst->category,
			!!inst->client,
			package_abi(inst->info),
			inst->lb.width,
			inst->lb.height);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	ret = slave_activate(package_slave(inst->info));
	if (ret < 0 && ret != -EALREADY) {
		/*!
		 * \note
		 * If the master failed to launch the slave,
		 * Do not send any requests to the slave.
		 */
		ErrPrint("Failed to launch the slave\n");
		packet_destroy(packet);
		return ret;
	}

	inst->state = INST_REQUEST_TO_ACTIVATE;
	inst->requested_state = INST_ACTIVATED;
	inst->changing_state = 1;

	/*!
	 * \note
	 * Try to activate a slave if it is not activated
	 */
	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, activate_cb, instance_ref(inst), 1);
}

HAPI void instance_lb_updated(const char *pkgname, const char *id)
{
	struct inst_info *inst;

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		return;

	instance_lb_updated_by_instance(inst);
}

HAPI void instance_lb_updated_by_instance(struct inst_info *inst)
{
	struct packet *packet;
	const char *id;
	enum lb_type lb_type;
	const char *title;
	const char *content;

	if (inst->client && inst->visible != LB_SHOW) {
		if (inst->visible == LB_HIDE) {
			DbgPrint("Ignore update event %s(HIDE)\n", inst->id);
			return;
		}
		DbgPrint("Livebox(%s) is PAUSED. But content is updated.\n", inst->id);
	}

	lb_type = package_lb_type(inst->info);
	if (lb_type == LB_TYPE_SCRIPT)
		id = fb_id(script_handler_fb(inst->lb.canvas.script));
	else if (lb_type == LB_TYPE_BUFFER)
		id = buffer_handler_id(inst->lb.canvas.buffer);
	else
		id = "";

	if (inst->content)
		content = inst->content;
	else
		content = "";

	if (inst->title)
		title = inst->title;
	else
		title = "";

	packet = packet_create_noack("lb_updated", "sssiidss",
			package_name(inst->info), inst->id, id,
			inst->lb.width, inst->lb.height, inst->lb.priority, content, title);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	(void)CLIENT_SEND_EVENT(inst, packet);
}

HAPI void instance_pd_updated_by_instance(struct inst_info *inst, const char *descfile)
{
	struct packet *packet;
	const char *id;

	if (inst->client && inst->visible != LB_SHOW) {
		DbgPrint("Livebox is hidden. ignore update event\n");
		return;
	}

	if (!inst->pd.need_to_send_close_event) {
		DbgPrint("PD is not created yet. Ignore update event - %s\n", descfile);

		if (inst->pd.pended_update_desc) {
			DbgFree(inst->pd.pended_update_desc);
			inst->pd.pended_update_desc = NULL;
		}

		if (descfile) {
			inst->pd.pended_update_desc = strdup(descfile);
			if (!inst->pd.pended_update_desc)
				ErrPrint("Heap: %s\n", strerror(errno));
		}

		inst->pd.pended_update_cnt++;
		return;
	}

	if (!descfile)
		descfile = inst->id;

	switch (package_pd_type(inst->info)) {
	case PD_TYPE_SCRIPT:
		id = fb_id(script_handler_fb(inst->pd.canvas.script));
		break;
	case PD_TYPE_BUFFER:
		id = buffer_handler_id(inst->pd.canvas.buffer);
		break;
	case PD_TYPE_TEXT:
	default:
		id = "";
		break;
	}

	packet = packet_create_noack("pd_updated", "ssssii",
			package_name(inst->info), inst->id, descfile, id,
			inst->pd.width, inst->pd.height);
	if (!packet) {
		ErrPrint("Failed to create param (%s - %s)\n", package_name(inst->info), inst->id);
		return;
	}

	(void)CLIENT_SEND_EVENT(inst, packet);
}

HAPI void instance_pd_updated(const char *pkgname, const char *id, const char *descfile)
{
	struct inst_info *inst;

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		return;

	instance_pd_updated_by_instance(inst, descfile);
}

HAPI void instance_set_lb_info(struct inst_info *inst, int w, int h, double priority, const char *content, const char *title)
{
	char *_content = NULL;
	char *_title = NULL;

	if (content && strlen(content)) {
		_content = strdup(content);
		if (!_content)
			ErrPrint("Heap: %s\n", strerror(errno));
	}

	if (title && strlen(title)) {
		_title = strdup(title);
		if (!_title)
			ErrPrint("Heap: %s\n", strerror(errno));
	}

	inst->lb.width = w;
	inst->lb.height = h;

	if (_content) {
		DbgFree(inst->content);
		inst->content= _content;
	}

	if (_title) {
		DbgFree(inst->title);
		inst->title = _title;
	}

	if (priority >= 0.0f && priority <= 1.0f)
		inst->lb.priority = priority;
}

HAPI void instance_set_pd_info(struct inst_info *inst, int w, int h)
{
	inst->pd.width = w;
	inst->pd.height = h;
}

static void pinup_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct set_pinup_cbdata *cbdata = data;
	const char *content;
	struct packet *result;
	int ret;

	if (!packet) {
		/*!
		 * \todo
		 * Send pinup failed event to client.
		 */
		ret = -EINVAL;
		goto out;
	}

	if (packet_get(packet, "is", &ret, &content) != 2) {
		/*!
		 * \todo
		 * Send pinup failed event to client
		 */
		ret = -EINVAL;
		goto out;
	}

	if (ret == 0) {
		char *new_content;

		new_content = strdup(content);
		if (!new_content) {
			ErrPrint("Heap: %s\n", strerror(errno));
			/*!
			 * \note
			 * send pinup failed event to client
			 */
			ret = -ENOMEM;
			goto out;
		}
	
		cbdata->inst->is_pinned_up = cbdata->pinup;
		DbgFree(cbdata->inst->content);

		cbdata->inst->content = new_content;
	}

out:
	/*!
	 * \node
	 * Send PINUP Result to client.
	 * Client should wait this event.
	 */
	result = packet_create_noack("pinup", "iisss", ret, cbdata->inst->is_pinned_up,
							package_name(cbdata->inst->info), cbdata->inst->id, cbdata->inst->content);
	if (result)
		(void)CLIENT_SEND_EVENT(cbdata->inst, result);
	else
		ErrPrint("Failed to build a packet for %s\n", package_name(cbdata->inst->info));

	instance_unref(cbdata->inst);
	DbgFree(cbdata);
}

HAPI int instance_set_pinup(struct inst_info *inst, int pinup)
{
	struct set_pinup_cbdata *cbdata;
	struct packet *packet;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	if (!package_pinup(inst->info))
		return -EINVAL;

	if (pinup == inst->is_pinned_up)
		return -EINVAL;

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata)
		return -ENOMEM;

	cbdata->inst = instance_ref(inst);
	cbdata->pinup = pinup;

	packet = packet_create("pinup", "ssi", package_name(inst->info), inst->id, pinup);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, pinup_cb, cbdata, 0);
}

HAPI int instance_freeze_updator(struct inst_info *inst)
{
	if (!inst->update_timer)
		return -EINVAL;

	DbgPrint("Freeze the update timer (%s)\n", inst->id);
	timer_freeze(inst);
	return 0;
}

HAPI int instance_thaw_updator(struct inst_info *inst)
{
	if (!inst->update_timer)
		return -EINVAL;

	if (client_is_all_paused() || setting_is_lcd_off()) {
		DbgPrint("Skip thaw (%s)\n", inst->id);
		return -EINVAL;
	}

	if (inst->visible == LB_HIDE_WITH_PAUSE) {
		DbgPrint("Live box is invisible (%s)\n", inst->id);
		return -EINVAL;
	}

	DbgPrint("Thaw the update timer (%s)\n", inst->id);
	timer_thaw(inst);
	return 0;
}

HAPI enum livebox_visible_state instance_visible_state(struct inst_info *inst)
{
	return inst->visible;
}

HAPI int instance_set_visible_state(struct inst_info *inst, enum livebox_visible_state state)
{
	if (inst->visible == state) {
		DbgPrint("Visibility has no changed\n");
		return 0;
	}

	switch (state) {
	case LB_SHOW:
	case LB_HIDE:
		if (inst->visible == LB_HIDE_WITH_PAUSE) {
			if (resume_livebox(inst) == 0)
				inst->visible = state;

			instance_thaw_updator(inst);
		} else {
			inst->visible = state;
		}
		break;

	case LB_HIDE_WITH_PAUSE:
		if (pause_livebox(inst) == 0)
			inst->visible = LB_HIDE_WITH_PAUSE;

		instance_freeze_updator(inst);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static void resize_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct resize_cbdata *cbdata = data;
	int ret;

	if (!packet) {
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid parameter\n");
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return;
	}

	if (ret == 0) {
		cbdata->inst->lb.width = cbdata->w;
		cbdata->inst->lb.height = cbdata->h;
	} else {
		ErrPrint("Failed to change the size of a livebox (%d)\n", ret);
	}

	instance_unref(cbdata->inst);
	DbgFree(cbdata);
}

HAPI int instance_resize(struct inst_info *inst, int w, int h)
{
	struct resize_cbdata *cbdata;
	struct packet *packet;
	int ret;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		ErrPrint("Fault package: %s\n", package_name(inst->info));
		return -EFAULT;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	cbdata->inst = instance_ref(inst);
	cbdata->w = w;
	cbdata->h = h;

	/* NOTE: param is resued from here */
	packet = packet_create("resize", "ssii", package_name(inst->info), inst->id, w, h);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return -EFAULT;
	}

	ret = slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, resize_cb, cbdata, 0);
	return ret;
}

static void set_period_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	int ret;
	struct period_cbdata *cbdata = data;
	struct packet *result;

	if (!packet) {
		ret = -EFAULT;
		goto out;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ret = -EINVAL;
		goto out;
	}

	if (ret == 0)
		cbdata->inst->period = cbdata->period;
	else
		ErrPrint("Failed to set period %d\n", ret);

out:
	result = packet_create_noack("period_changed", "idss", ret, cbdata->inst->period, package_name(cbdata->inst->info), cbdata->inst->id);
	if (result)
		(void)CLIENT_SEND_EVENT(cbdata->inst, result);
	else
		ErrPrint("Failed to build a packet for %s\n", package_name(cbdata->inst->info));

	instance_unref(cbdata->inst);
	DbgFree(cbdata);
	return;
}

static Eina_Bool timer_updator_cb(void *data)
{
	struct period_cbdata *cbdata = data;
	struct inst_info *inst;
	double period;
	struct packet *result;

	period = cbdata->period;
	inst = cbdata->inst;
	DbgFree(cbdata);

	DbgPrint("Update period is changed to %lf from %lf\n", period, inst->period);

	inst->period = period;
	if (inst->update_timer) {
		if (inst->period == 0.0f) {
			ecore_timer_del(inst->update_timer);
			inst->update_timer = NULL;
		} else {
			util_timer_interval_set(inst->update_timer, inst->period);
		}
	} else if (inst->period > 0.0f) {
		inst->update_timer = util_timer_add(inst->period, update_timer_cb, inst);
		if (!inst->update_timer)
			ErrPrint("Failed to add an update timer for instance %s\n", inst->id);
		else
			timer_freeze(inst); /* Freeze the update timer as default */
	}

	result = packet_create_noack("period_changed", "idss", 0, inst->period, package_name(inst->info), inst->id);
	if (result)
		(void)CLIENT_SEND_EVENT(inst, result);
	else
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));

	instance_unref(inst);
	return ECORE_CALLBACK_CANCEL;
}

HAPI int instance_set_period(struct inst_info *inst, double period)
{
	struct packet *packet;
	struct period_cbdata *cbdata;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	if (period < 0.0f) { /* Use the default period */
		period = package_period(inst->info);
	} else if (period > 0.0f && period < MINIMUM_PERIOD) {
		period = MINIMUM_PERIOD; /* defined at conf.h */
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	cbdata->period = period;
	cbdata->inst = instance_ref(inst);

	if (package_secured(inst->info)) {
		/*!
		 * \note
		 * Secured livebox doesn't need to send its update period to the slave.
		 * Slave has no local timer for updating liveboxes
		 *
		 * So update its timer at here.
		 */
		if (!ecore_timer_add(DELAY_TIME, timer_updator_cb, cbdata))
			timer_updator_cb(cbdata);
		return 0;
	}

	packet = packet_create("set_period", "ssd", package_name(inst->info), inst->id, period);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		DbgFree(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, set_period_cb, cbdata, 0);
}

HAPI int instance_clicked(struct inst_info *inst, const char *event, double timestamp, double x, double y)
{
	struct packet *packet;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	/* NOTE: param is resued from here */
	packet = packet_create_noack("clicked", "sssddd", package_name(inst->info), inst->id, event, timestamp, x, y);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

HAPI int instance_signal_emit(struct inst_info *inst, const char *signal, const char *part, double sx, double sy, double ex, double ey, double x, double y, int down)
{
	const char *pkgname;
	const char *id;
	struct slave_node *slave;
	struct packet *packet;
	int ret;

	pkgname = package_name(instance_package(inst));
	id = instance_id(inst);
	if (!pkgname || !id) {
		ErrPrint("Invalid instance\n");
		return -EINVAL;
	}

	slave = package_slave(instance_package(inst));
	if (!slave) {
		ErrPrint("Slave is not valid\n");
		return -EINVAL;
	}

	packet = packet_create_noack("script", "ssssddddddi",
			pkgname, id,
			signal, part,
			sx, sy, ex, ey,
			x, y, down);
	if (!packet) {
		ErrPrint("Failed to create param\n");
		return -EFAULT;
	}

	ret = slave_rpc_request_only(slave, pkgname, packet, 0); 
	return ret;
}

HAPI int instance_text_signal_emit(struct inst_info *inst, const char *emission, const char *source, double sx, double sy, double ex, double ey)
{
	struct packet *packet;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	packet = packet_create_noack("text_signal", "ssssdddd", package_name(inst->info), inst->id, emission, source, sx, sy, ex, ey);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		return -EFAULT;
	}

	return slave_rpc_request_only(package_slave(inst->info), package_name(inst->info), packet, 0);
}

static void change_group_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	struct change_group_cbdata *cbdata = data;
	struct packet *result;
	int ret;

	if (!packet) {
		DbgFree(cbdata->cluster);
		DbgFree(cbdata->category);
		ret = -EFAULT;
		goto out;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid packet\n");
		DbgFree(cbdata->cluster);
		DbgFree(cbdata->category);
		ret = -EINVAL;
		goto out;
	}

	if (ret == 0) {
		DbgFree(cbdata->inst->cluster);
		cbdata->inst->cluster = cbdata->cluster;

		DbgFree(cbdata->inst->category);
		cbdata->inst->category = cbdata->category;
	} else {
		DbgFree(cbdata->cluster);
		DbgFree(cbdata->category);
	}

out:
	result = packet_create_noack("group_changed", "ssiss",
				package_name(cbdata->inst->info), cbdata->inst->id, ret,
				cbdata->inst->cluster, cbdata->inst->category);
	if (!result)
		ErrPrint("Failed to build a packet %s\n", package_name(cbdata->inst->info));
	else
		(void)CLIENT_SEND_EVENT(cbdata->inst, result);

	instance_unref(cbdata->inst);
	DbgFree(cbdata);
}

HAPI int instance_change_group(struct inst_info *inst, const char *cluster, const char *category)
{
	struct packet *packet;
	struct change_group_cbdata *cbdata;

	if (!inst) {
		ErrPrint("Invalid instance handle\n");
		return -EINVAL;
	}

	if (package_is_fault(inst->info)) {
		DbgPrint("Fault package [%s]\n", package_name(inst->info));
		return -EFAULT;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	cbdata->cluster = strdup(cluster);
	if (!cbdata->cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(cbdata);
		return -ENOMEM;
	}

	cbdata->category = strdup(category);
	if (!cbdata->category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(cbdata->cluster);
		DbgFree(cbdata);
		return -ENOMEM;
	}

	cbdata->inst = instance_ref(inst);

	packet = packet_create("change_group","ssss", package_name(inst->info), inst->id, cluster, category);
	if (!packet) {
		ErrPrint("Failed to build a packet for %s\n", package_name(inst->info));
		instance_unref(cbdata->inst);
		DbgFree(cbdata->category);
		DbgFree(cbdata->cluster);
		DbgFree(cbdata);
		return -EFAULT;
	}

	return slave_rpc_async_request(package_slave(inst->info), package_name(inst->info), packet, change_group_cb, cbdata, 0);
}

HAPI const char * const instance_auto_launch(const struct inst_info *inst)
{
	return inst->lb.auto_launch;
}

HAPI const int const instance_priority(const struct inst_info *inst)
{
	return inst->lb.priority;
}

HAPI const struct client_node *const instance_client(const struct inst_info *inst)
{
	return inst->client;
}

HAPI const double const instance_period(const struct inst_info *inst)
{
	return inst->period;
}

HAPI const int const instance_lb_width(const struct inst_info *inst)
{
	return inst->lb.width;
}

HAPI const int const instance_lb_height(const struct inst_info *inst)
{
	return inst->lb.height;
}

HAPI const int const instance_pd_width(const struct inst_info *inst)
{
	return inst->pd.width;
}

HAPI const int const instance_pd_height(const struct inst_info *inst)
{
	return inst->pd.height;
}

HAPI struct pkg_info *const instance_package(const struct inst_info *inst)
{
	return inst->info;
}

HAPI struct script_info *const instance_lb_script(const struct inst_info *inst)
{
	return (package_lb_type(inst->info) == LB_TYPE_SCRIPT) ? inst->lb.canvas.script : NULL;
}

HAPI struct script_info *const instance_pd_script(const struct inst_info *inst)
{
	return (package_pd_type(inst->info) == PD_TYPE_SCRIPT) ? inst->pd.canvas.script : NULL;
}

HAPI struct buffer_info *const instance_lb_buffer(const struct inst_info *inst)
{
	return (package_lb_type(inst->info) == LB_TYPE_BUFFER) ? inst->lb.canvas.buffer : NULL;
}

HAPI struct buffer_info *const instance_pd_buffer(const struct inst_info *inst)
{
	return (package_pd_type(inst->info) == PD_TYPE_BUFFER) ? inst->pd.canvas.buffer : NULL;
}

HAPI const char *const instance_id(const struct inst_info *inst)
{
	return inst->id;
}

HAPI const char *const instance_content(const struct inst_info *inst)
{
	return inst->content;
}

HAPI const char *const instance_category(const struct inst_info *inst)
{
	return inst->category;
}

HAPI const char *const instance_cluster(const struct inst_info *inst)
{
	return inst->cluster;
}

HAPI const char * const instance_title(const struct inst_info *inst)
{
	return inst->title;
}

HAPI const double const instance_timestamp(const struct inst_info *inst)
{
	return inst->timestamp;
}

HAPI const enum instance_state const instance_state(const struct inst_info *inst)
{
	return inst->state;
}

HAPI int instance_destroyed(struct inst_info *inst)
{
	switch (inst->state) {
	case INST_INIT:
	case INST_REQUEST_TO_ACTIVATE:
		/*!
		 * \note
		 * No other clients know the existence of this instance,
		 * only who added this knows it.
		 * So send deleted event to only it.
		 */
		DbgPrint("Send deleted event - unicast - %p\n", inst->client);
		instance_unicast_deleted_event(inst, NULL);
		instance_state_reset(inst);
		instance_destroy(inst);
		break;
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
	case INST_ACTIVATED:
		DbgPrint("Send deleted event - multicast\n");
		instance_broadcast_deleted_event(inst);
		instance_state_reset(inst);
		instance_destroy(inst);
	case INST_DESTROYED:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*!
 * Invoked when a slave is activated
 */
HAPI int instance_recover_state(struct inst_info *inst)
{
	struct pkg_info *info;
	int ret = 0;

	if (inst->changing_state) {
		DbgPrint("Doesn't need to recover the state\n");
		return 0;
	}

	switch (inst->state) {
	case INST_ACTIVATED:
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
		switch (inst->requested_state) {
		case INST_ACTIVATED:
			DbgPrint("Req. to RE-ACTIVATED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_reactivate(inst);
			ret = 1;
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			info = inst->info;
			instance_state_reset(inst);
			instance_destroy(inst);
			break;
		default:
			break;
		}
		break;
	case INST_INIT:
	case INST_REQUEST_TO_ACTIVATE:
		switch (inst->requested_state) {
		case INST_ACTIVATED:
		case INST_INIT:
			DbgPrint("Req. to ACTIVATED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			if (instance_activate(inst) < 0) {
				DbgPrint("Failed to reactivate the instance\n");
				instance_broadcast_deleted_event(inst);
				instance_state_reset(inst);
				instance_destroy(inst);
			} else {
				ret = 1;
			}
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_destroy(inst);
			ret = 1;
			break;
		default:
			break;
		}
		break;
	case INST_DESTROYED:
	default:
		break;
	}

	return ret;
}

/*!
 * Invoked when a slave is deactivated
 */
HAPI int instance_need_slave(struct inst_info *inst)
{
	int ret = 0;
	struct pkg_info *info;

	if (inst->client && client_is_faulted(inst->client)) {
		info = inst->info;

		/*!
		 * \note
		 * In this case, the client is faulted(disconnected)
		 * when the client is deactivated, its liveboxes should be removed too.
		 * So if the current inst is created by the faulted client,
		 * remove it and don't try to recover its states
		 */

		DbgPrint("CLIENT FAULT: Req. to DESTROYED (%s)\n", package_name(info));
		switch (inst->state) {
		case INST_INIT:
		case INST_ACTIVATED:
		case INST_REQUEST_TO_REACTIVATE:
		case INST_REQUEST_TO_DESTROY:
		case INST_REQUEST_TO_ACTIVATE:
			instance_state_reset(inst);
			instance_destroy(inst);
			break;
		case INST_DESTROYED:
			break;
		}

		return 0;
	}

	switch (inst->state) {
	case INST_ACTIVATED:
	case INST_REQUEST_TO_REACTIVATE:
	case INST_REQUEST_TO_DESTROY:
		switch (inst->requested_state) {
		case INST_INIT:
		case INST_ACTIVATED:
			DbgPrint("Req. to ACTIVATED (%s)\n", package_name(inst->info));
			ret = 1;
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			info = inst->info;
			instance_state_reset(inst);
			instance_destroy(inst);
			break;
		default:
			break;
		}
		break;
	case INST_INIT:
	case INST_REQUEST_TO_ACTIVATE:
		switch (inst->requested_state) {
		case INST_INIT:
		case INST_ACTIVATED:
			DbgPrint("Req. to ACTIVATED (%s)\n", package_name(inst->info));
			ret = 1;
			break;
		case INST_DESTROYED:
			DbgPrint("Req. to DESTROYED (%s)\n", package_name(inst->info));
			instance_state_reset(inst);
			instance_destroy(inst);
			break;
		default:
			break;
		}
		break;
	case INST_DESTROYED:
	default:
		break;
	}

	return ret;
}

HAPI void instance_slave_set_pd_pos(struct inst_info *inst, double x, double y)
{
	inst->pd.x = x;
	inst->pd.y = y;
}

HAPI void instance_slave_get_pd_pos(struct inst_info *inst, double *x, double *y)
{
	if (x)
		*x = inst->pd.x;
	if (y)
		*y = inst->pd.y;
}

HAPI int instance_slave_open_pd(struct inst_info *inst, struct client_node *client)
{
	const char *pkgname;
	const char *id;
	struct packet *packet;
	struct slave_node *slave;
	const struct pkg_info *info;
	int ret;

	if (!client) {
		client = inst->pd.owner;
		if (!client) {
			ErrPrint("Client is not valid\n");
			return -EINVAL;
		}
	} else if (inst->pd.owner) {
		if (inst->pd.owner != client) {
			ErrPrint("Client is already owned\n");
			return -EBUSY;
		}
	}

	slave = package_slave(instance_package(inst));
	if (!slave)
		return -EFAULT;

	info = instance_package(inst);
	if (!info)
		return -EINVAL;

	pkgname = package_name(info);
	id = instance_id(inst);

	if (!pkgname || !id)
		return -EINVAL;

	packet = packet_create_noack("pd_show", "ssiidd", pkgname, id, instance_pd_width(inst), instance_pd_height(inst), inst->pd.x, inst->pd.y);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	slave_freeze_ttl(slave);

	ret = slave_rpc_request_only(slave, pkgname, packet, 0);

	/*!
	 * \note
	 * If a client is disconnected, the slave has to close the PD
	 * So the pd_buffer_close_cb/pd_script_close_cb will catch the disconnection event
	 * then it will send the close request to the slave
	 */
	if (package_pd_type(info) == PD_TYPE_BUFFER) {
		instance_ref(inst);
		if (client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, pd_buffer_close_cb, inst) < 0) {
			instance_unref(inst);
		}
	} else if (package_pd_type(info) == PD_TYPE_SCRIPT) {
		instance_ref(inst);
		if (client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, pd_script_close_cb, inst) < 0) {
			instance_unref(inst);
		}
	}

	inst->pd.owner = client;
	return ret;
}

HAPI int instance_slave_close_pd(struct inst_info *inst, struct client_node *client)
{
	const char *pkgname;
	const char *id;
	struct packet *packet;
	struct slave_node *slave;
	struct pkg_info *info;
	int ret;

	if (inst->pd.owner != client) {
		ErrPrint("PD owner is not matched\n");
		return -EINVAL;
	}

	slave = package_slave(instance_package(inst));
	if (!slave)
		return -EFAULT;

	info = instance_package(inst);
	if (!info)
		return -EINVAL;

	pkgname = package_name(info);
	id = instance_id(inst);

	if (!pkgname || !id)
		return -EINVAL;

	packet = packet_create_noack("pd_hide", "ss", pkgname, id);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	slave_thaw_ttl(slave);

	ret = slave_rpc_request_only(slave, pkgname, packet, 0);
	release_resource_for_closing_pd(info, inst, client);
	inst->pd.owner = NULL;
	return ret;
}

HAPI int instance_client_pd_created(struct inst_info *inst, int status)
{
	struct packet *packet;
	const char *buf_id;
	int ret;

	if (inst->pd.need_to_send_close_event) {
		DbgPrint("PD is already created\n");
		return -EINVAL;
	}

	switch (package_pd_type(inst->info)) {
	case PD_TYPE_SCRIPT:
		buf_id = fb_id(script_handler_fb(inst->pd.canvas.script));
		break;
	case PD_TYPE_BUFFER:
		buf_id = buffer_handler_id(inst->pd.canvas.buffer);
		break;
	case PD_TYPE_TEXT:
	default:
		buf_id = "";
		break;
	}

	inst->pd.need_to_send_close_event = 1;

	packet = packet_create_noack("pd_created", "sssiii", 
			package_name(inst->info), inst->id, buf_id,
			inst->pd.width, inst->pd.height, status);
	if (!packet) {
		ErrPrint("Failed to create a packet\n");
		return -EFAULT;
	}

	ret = CLIENT_SEND_EVENT(inst, packet);

	if (inst->pd.pended_update_cnt) {
		DbgPrint("Apply pended desc(%d) - %s\n", inst->pd.pended_update_cnt, inst->pd.pended_update_desc);
		instance_pd_updated_by_instance(inst, inst->pd.pended_update_desc);
		inst->pd.pended_update_cnt = 0;
		DbgFree(inst->pd.pended_update_desc);
		inst->pd.pended_update_desc = NULL;
	}

	return ret;
}

HAPI int instance_client_pd_destroyed(struct inst_info *inst, int status)
{
	if (!inst->pd.need_to_send_close_event) {
		DbgPrint("PD is not created\n");
		return -EINVAL;
	}

	inst->pd.need_to_send_close_event = 0;

	return send_pd_destroyed_to_client(inst, status);
}

HAPI int instance_add_client(struct inst_info *inst, struct client_node *client)
{
	if (inst->client == client) {
		ErrPrint("Owner cannot be the viewer\n");
		return -EINVAL;
	}

	DbgPrint("%d is added to the list of viewer of %s(%s)\n", client_pid(client), package_name(instance_package(inst)), instance_id(inst));
	if (client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, viewer_deactivated_cb, inst) < 0) {
		ErrPrint("Failed to add a deactivate callback\n");
		return -EFAULT;
	}

	instance_ref(inst);
	inst->client_list = eina_list_append(inst->client_list, client);
	return 0;
}

HAPI int instance_del_client(struct inst_info *inst, struct client_node *client)
{
	if (inst->client == client) {
		ErrPrint("Owner is not in the viewer list\n");
		return -EINVAL;
	}

	client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, viewer_deactivated_cb, inst);
	viewer_deactivated_cb(client, inst);
	return 0;
}

HAPI int instance_has_client(struct inst_info *inst, struct client_node *client)
{
	return !!eina_list_data_find(inst->client_list, client);
}

HAPI void *instance_client_list(struct inst_info *inst)
{
	return inst->client_list;
}

HAPI void instance_init(void)
{
	if (!strcasecmp(PROVIDER_METHOD, "shm"))
		s_info.env_buf_type = BUFFER_TYPE_SHM;
	else if (!strcasecmp(PROVIDER_METHOD, "pixmap"))
		s_info.env_buf_type = BUFFER_TYPE_PIXMAP;
}

HAPI void instance_fini(void)
{
}

/* End of a file */
