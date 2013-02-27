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
#include <unistd.h>
#include <errno.h>

#include <dlog.h>
#include <Evas.h>
#include <Ecore_Evas.h> /* fb.h */
#include <aul.h>
#include <Ecore.h>

#include <packet.h>
#include <com-core_packet.h>

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

static struct info {
	int info_fd;
	int client_fd;
	int service_fd;
	int slave_fd;
} s_info = {
	.info_fd = -1,
	.client_fd = -1,
	.service_fd = -1,
	.slave_fd = -1,
};

/* Share this with provider */
enum target_type {
	TYPE_LB,
	TYPE_PD,
	TYPE_ERROR,
};

struct deleted_item {
	struct client_node *client;
	struct inst_info *inst;
};

static struct packet *client_acquire(pid_t pid, int handle, const struct packet *packet) /*!< timestamp, ret */
{
	struct client_node *client;
	struct packet *result;
	double timestamp;
	int ret;

	client = client_find_by_pid(pid);
	if (client) {
		ErrPrint("Client is already exists %d\n", pid);
		ret = -EEXIST;
		goto out;
	}

	if (packet_get(packet, "d", &timestamp) != 1) {
		ErrPrint("Invalid arguemnt\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("Acquired %lf\n", timestamp);

	ret = 0;
	/*!
	 * \note
	 * client_create will invoke the client created callback
	 */
	client = client_create(pid, handle);
	if (!client) {
		ErrPrint("Failed to create a new client for %d\n", pid);
		ret = -EFAULT;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *cilent_release(pid_t pid, int handle, const struct packet *packet) /*!< pid, ret */
{
	struct client_node *client;
	struct packet *result;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	client_destroy(client);
	ret = 0;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "sssddd", &pkgname, &id, &event, &timestamp, &x, &y);
	if (ret != 6) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s] event[%s] timestamp[%lf] x[%lf] y[%lf]\n", pid, pkgname, id, event, timestamp, x, y);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else
		ret = instance_clicked(inst, event, timestamp, x, y);

out:
	/*! \note No reply packet */
	return NULL;
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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssssdddd", &pkgname, &id, &emission, &source, &sx, &sy, &ex, &ey);
	if (ret != 8) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s] emission[%s] source[%s] sx[%lf] sy[%lf] ex[%lf] ey[%lf]\n", pid, pkgname, id, emission, source, sx, sy, ex, ey);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else
		ret = instance_text_signal_emit(inst, emission, source, sx, sy, ex, ey);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static Eina_Bool lazy_delete_cb(void *data)
{
	struct deleted_item *item = data;

	DbgPrint("Send delete event to the client\n");

	/*!
	 * Before invoke this callback, the instance is able to already remove this client
	 * So check it again
	 */
	if (instance_has_client(item->inst, item->client)) {
		instance_unicast_deleted_event(item->inst, item->client);
		instance_del_client(item->inst, item->client);
	}

	client_unref(item->client);
	instance_unref(item->inst);
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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s]\n", pid, pkgname, id);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ret = -EFAULT;
	} else if (instance_client(inst) != client) {
		if (instance_has_client(inst, client)) {
			struct deleted_item *item;

			item = malloc(sizeof(*item));
			if (!item) {
				ErrPrint("Heap: %s\n", strerror(errno));
				ret = -ENOMEM;
			} else {
				ret = 0;
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
					client_unref(client);
					instance_unref(inst);
					DbgFree(item);
					ret = -EFAULT;
				}
			}
		} else {
			ret = -EPERM;
		}
	} else {
		ret = instance_destroy(inst);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssii", &pkgname, &id, &w, &h);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s] w[%d] h[%d]\n", pid, pkgname, id, w, h);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ret = -EFAULT;
	} else if (instance_client(inst) != client) {
		ret = -EPERM;
	} else {
		ret = instance_resize(inst, w, h);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "dssssdii", &timestamp, &pkgname, &content, &cluster, &category, &period, &width, &height);
	if (ret != 8) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] period[%lf] pkgname[%s] content[%s] cluster[%s] category[%s] period[%lf]\n",
						pid, timestamp, pkgname, content, cluster, category, period);

	lb_pkgname = package_lb_pkgname(pkgname);
	if (!lb_pkgname) {
		ErrPrint("This %s has no livebox package\n", pkgname);
		ret = -EINVAL;
		goto out;
	}

	info = package_find(lb_pkgname);
	if (!info)
		info = package_create(lb_pkgname);

	if (!info) {
		ret = -EFAULT;
	} else if (package_is_fault(info)) {
		ret = -EFAULT;
	} else if (util_free_space(IMAGE_PATH) < MINIMUM_SPACE) {
		ErrPrint("Not enough space\n");
		ret = -ENOSPC;
	} else {
		struct inst_info *inst;

		if (period > 0.0f && period < MINIMUM_PERIOD)
			period = MINIMUM_PERIOD;

		if (!strlen(content))
			content = DEFAULT_CONTENT;

		inst = instance_create(client, timestamp, lb_pkgname, content, cluster, category, period, width, height);
		/*!
		 * \note
		 * Using the "inst" without validate its value is at my disposal. ;)
		 */
		ret = inst ? 0 : -EFAULT;
	}

	DbgFree(lb_pkgname);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, (int *)&state);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s] state[%d]\n", pid, pkgname, id, state);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ret = -EFAULT;
	} else if (instance_client(inst) != client) {
		ret = -EPERM;
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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssd", &pkgname, &id, &period);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s] period[%lf]\n", pid, pkgname, id, period);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ret = -EFAULT;
	} else if (instance_client(inst) != client) {
		ret = -EPERM;
	} else {
		ret = instance_set_period(inst, period);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssss", &pkgname, &id, &cluster, &category);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s] cluster[%s] category[%s]\n", pid, pkgname, id, cluster, category);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ret = -EFAULT;
	} else if (instance_client(inst) != client) {
		ret = -EPERM;
	} else {
		ret = instance_change_group(inst, cluster, category);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_pd_mouse_enter(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_enter", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		evas_event_feed_mouse_in(e, timestamp, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_leave", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		evas_event_feed_mouse_out(e, timestamp, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_down", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		evas_event_feed_mouse_down(e, 1, EVAS_BUTTON_NONE, timestamp + 0.01f, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_up", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		evas_event_feed_mouse_up(e, 1, EVAS_BUTTON_NONE, timestamp + 0.1f, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*!
		 * Reuse the packet.
		packet = packet_create_noack("pd_mouse_move", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		 */
		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_move", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/
		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_mouse_enter(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, filename, width, height, timestamp, x, y, ret */
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_enter", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/
		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		evas_event_feed_mouse_in(e, timestamp, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_leave", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		evas_event_feed_mouse_out(e, timestamp, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_down", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 1);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		evas_event_feed_mouse_down(e, 1, EVAS_BUTTON_NONE, timestamp + 0.01f, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_up", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, 0);
		evas_event_feed_mouse_move(e, x * w, y * h, timestamp, NULL);
		evas_event_feed_mouse_up(e, 1, EVAS_BUTTON_NONE, timestamp + 0.1f, NULL);
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_access_read(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_enter", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		/*!
		 * \TODO: Push up the ACCESS_READ event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_access_read_prev(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_enter", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		/*!
		 * \TODO: Push up the ACCESS_READ_PREV event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_access_read_next(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_enter", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		/*!
		 * \TODO: Push up the ACCESS_READ_NEXT event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_access_activate(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_enter", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		/*!
		 * \TODO: Push up the ACCESS_READ_ACTIVATE event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_pd_key_down(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_enter", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		/*!
		 * \TODO: Push up the KEY_DOWN event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is paused - manually reported\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "d", &timestamp);
	if (ret != 1) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	if (USE_XMONITOR)
		DbgPrint("XMONITOR enabled. ignore client paused request\n");
	else
		xmonitor_pause(client);

out:
	return NULL;
}

static struct packet *client_resume_request(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	double timestamp;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is paused - manually reported\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "d", &timestamp);
	if (ret != 1) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	if (USE_XMONITOR)
		DbgPrint("XMONITOR enabled. ignore client resumed request\n");
	else
		xmonitor_resume(client);

out:
	return NULL;
}

static struct packet *client_pd_key_up(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not found\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		// struct packet *packet;

		buffer = instance_pd_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("pd_mouse_enter", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_pd_type(pkg) == PD_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_pd_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);
		/*!
		 * \TODO: Push up the KEY_UP event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_access_read(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_leave", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);

		/*!
		 * \TODO: Feed up this ACCESS_READ event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_access_read_prev(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_leave", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);

		/*!
		 * \TODO: Feed up this ACCESS_READ_PREV event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_access_read_next(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_leave", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);

		/*!
		 * \TODO: Feed up this ACCESS_READ_NEXT event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_access_activate(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_leave", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);

		/*!
		 * \TODO: Feed up this ACCESS_ACTIVATE event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
	}

out:
	/*! \note No reply packet */
	return NULL;
}

static struct packet *client_lb_key_down(pid_t pid, int handle, const struct packet *packet)
{
	struct client_node *client;
	const char *pkgname;
	const char *id;
	int ret;
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_leave", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);

		/*!
		 * \TODO: Feed up this KEY_DOWN event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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
	int w;
	int h;
	double timestamp;
	double x;
	double y;
	struct inst_info *inst;
	const struct pkg_info *pkg;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssiiddd", &pkgname, &id, &w, &h, &timestamp, &x, &y);
	if (ret != 7) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance[%s] is not exists\n", id);
		ret = -ENOENT;
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		ErrPrint("Package[%s] info is not exists\n", pkgname);
		ret = -EFAULT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		/*!
		 * \note
		 * If the package is registered as fault module,
		 * slave has not load it, so we don't need to do anything at here!
		 */
		DbgPrint("Package[%s] is faulted\n", pkgname);
		ret = -EFAULT;
	} else if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
		struct buffer_info *buffer;
		struct slave_node *slave;
		//struct packet *packet;

		buffer = instance_lb_buffer(inst);
		if (!buffer) {
			ErrPrint("Instance[%s] has no buffer\n", id);
			ret = -EFAULT;
			goto out;
		}

		slave = package_slave(pkg);
		if (!slave) {
			ErrPrint("Package[%s] has no slave\n", pkgname);
			ret = -EINVAL;
			goto out;
		}

		/*
		packet = packet_create_noack("lb_mouse_leave", "ssiiddd", pkgname, id, w, h, timestamp, x, y);
		if (!packet) {
			ErrPrint("Failed to create a packet[%s]\n", pkgname);
			ret = -EFAULT;
			goto out;
		}
		*/

		packet_ref((struct packet *)packet);
		ret = slave_rpc_request_only(slave, pkgname, (struct packet *)packet, 0);
	} else if (package_lb_type(pkg) == LB_TYPE_SCRIPT) {
		struct script_info *script;
		Evas *e;

		script = instance_lb_script(inst);
		if (!script) {
			ret = -EFAULT;
			goto out;
		}

		e = script_handler_evas(script);
		if (!e) {
			ret = -EFAULT;
			goto out;
		}

		script_handler_update_pointer(script, x, y, -1);

		/*!
		 * \TODO: Feed up this KEY_UP event
		 */
		ret = 0;
	} else {
		ErrPrint("Unsupported package\n");
		ret = -EINVAL;
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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Failed to find an instance (%s - %s)\n", pkgname, id);
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s]\n", pid, pkgname, id);

	buf_ptr = buffer_handler_pixmap_ref(instance_lb_buffer(inst));
	if (!buf_ptr) {
		ErrPrint("Failed to ref pixmap\n");
		goto out;
	}

	ret = client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr);
	if (ret < 0) {
		ErrPrint("Failed to add a new client deactivate callback\n");
		buffer_handler_pixmap_unref(buf_ptr);
		pixmap = 0;
	} else {
		pixmap = buffer_handler_pixmap(instance_lb_buffer(inst));
	}

out:
	result = packet_create_reply(packet, "i", pixmap);
	if (!result)
		ErrPrint("Failed to create a reply packet\n");

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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pixmap);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}
	DbgPrint("pid[%d] pkgname[%s] id[%s] Pixmap[0x%X]\n", pid, pkgname, id, pixmap);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Failed to find an instance (%s - %s)\n", pkgname, id);
		goto out;
	}

	buf_ptr = buffer_handler_pixmap_find(pixmap);
	if (!buf_ptr) {
		ErrPrint("Failed to find a buf_ptr of 0x%X\n", pixmap);
		goto out;
	}

	if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr) == 0)
		buffer_handler_pixmap_unref(buf_ptr);

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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Failed to find an instance (%s - %s)\n", pkgname, id);
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s]\n", pid, pkgname, id);

	buf_ptr = buffer_handler_pixmap_ref(instance_pd_buffer(inst));
	if (!buf_ptr) {
		ErrPrint("Failed to ref pixmap\n");
		goto out;
	}

	ret = client_event_callback_add(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr);
	if (ret < 0)
		buffer_handler_pixmap_unref(buf_ptr);

	pixmap = buffer_handler_pixmap(instance_pd_buffer(inst));
out:
	result = packet_create_reply(packet, "i", pixmap);
	if (!result)
		ErrPrint("Failed to create a reply packet\n");

	return result;
}

static struct packet *client_pd_release_pixmap(pid_t pid, int handle, const struct packet *packet)
{
	const char *pkgname;
	const char *id;
	struct client_node *client;
	struct inst_info *inst;
	int pixmap;
	void *buf_ptr;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pixmap);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		goto out;
	}
	DbgPrint("pid[%d] pkgname[%s] id[%s]\n", pid, pkgname, id);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Failed to find an instance (%s - %s)\n", pkgname, id);
		goto out;
	}

	buf_ptr = buffer_handler_pixmap_find(pixmap);
	if (!buf_ptr) {
		ErrPrint("Failed to find a buf_ptr of 0x%X\n", pixmap);
		goto out;
	}

	if (client_event_callback_del(client, CLIENT_EVENT_DEACTIVATE, release_pixmap_cb, buf_ptr) == 0)
		buffer_handler_pixmap_unref(buf_ptr);

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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		pinup = 0;
		goto out;
	}

	ret = packet_get(packet, "ssi", &pkgname, &id, &pinup);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		pinup = 0;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s] pinup[%d]\n", pid, pkgname, id, pinup);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else
		ret = instance_set_pinup(inst, pinup);

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static Eina_Bool lazy_pd_created_cb(void *data)
{
	DbgPrint("Send PD Create event\n");
	instance_client_pd_created(data, 0);

	instance_unref(data);
	return ECORE_CALLBACK_CANCEL;
}

static Eina_Bool lazy_pd_destroyed_cb(void *data)
{
	DbgPrint("Send PD Destroy event\n");
	instance_client_pd_destroyed(data, 0);

	instance_unref(data);
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
	double x;
	double y;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssdd", &pkgname, &id, &x, &y);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s]\n", pid, pkgname, id);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else if (util_free_space(IMAGE_PATH) < MINIMUM_SPACE)
		ret = -ENOSPC;
	else if (package_pd_type(instance_package(inst)) == PD_TYPE_BUFFER) {
		instance_slave_set_pd_pos(inst, x, y);
		ret = instance_slave_open_pd(inst, client);
		ret = instance_signal_emit(inst,
				"pd,show", util_uri_to_path(instance_id(inst)),
				0.0, 0.0, 0.0, 0.0, x, y, 0);
		/*!
		 * \note
		 * PD craeted event will be send by the acquire_buffer function.
		 * Because the slave will make request the acquire_buffer to
		 * render the PD
		 *
		 * instance_client_pd_created(inst);
		 */
	} else if (package_pd_type(instance_package(inst)) == PD_TYPE_SCRIPT) {
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
		ret = instance_slave_open_pd(inst, client);
		script_handler_update_pointer(instance_pd_script(inst), x, y, 0);
		ret = script_handler_load(instance_pd_script(inst), 1);

		/*!
		 * \note
		 * Send the PD created event to the clients,
		 */
		if (ret == 0) {
			/*!
			 * \note
			 * But the created event has to be send afte return
			 * from this function or the viewer couldn't care
			 * the event correctly.
			 */
			inst = instance_ref(inst); /* To guarantee the inst */
			if (!ecore_timer_add(DELAY_TIME, lazy_pd_created_cb, inst))
				instance_unref(inst);
		}
	} else {
		ErrPrint("Invalid PD TYPE\n");
		ret = -EINVAL;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] pkgname[%s] id[%s]\n", pid, pkgname, id);

	/*!
	 * \NOTE:
	 * Trust the package name which are sent by the client.
	 * The package has to be a livebox package name.
	 */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else if (package_pd_type(instance_package(inst)) == PD_TYPE_BUFFER) {
		ret = instance_signal_emit(inst,
				"pd,hide", util_uri_to_path(instance_id(inst)),
				0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0);
		ret = instance_slave_close_pd(inst, client);

		/*!
		 * \note
		 * release_buffer will be called by the slave after this.
		 * Then it will send the "pd_destroyed" event to the client
		 *
		 * instance_client_pd_destroyed(inst);
		 */

	} else if (package_pd_type(instance_package(inst)) == PD_TYPE_SCRIPT) {
		ret = instance_slave_close_pd(inst, client);

		ret = script_handler_unload(instance_pd_script(inst), 1);

		/*!
		 * \note
		 * Send the destroyed PD event to the client
		 */
		if (ret == 0) {
			inst = instance_ref(inst);
			if (!ecore_timer_add(DELAY_TIME, lazy_pd_destroyed_cb, inst))
				instance_unref(inst);
		}
	} else {
		ErrPrint("Invalid PD TYPE\n");
		ret = -EINVAL;
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_activate_package(pid_t pid, int handle, const struct packet *packet) /* pid, pkgname, ret */
{
	struct client_node *client;
	struct packet *result;
	const char *pkgname;
	int ret;
	struct pkg_info *info;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		pkgname = "";
		goto out;
	}

	ret = packet_get(packet, "s", &pkgname);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
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
		ret = -EINVAL;
		goto out;
	}

	info = package_find(pkgname);
	if (!info)
		ret = -ENOENT;
	else
		ret = package_clear_fault(info);

out:
	result = packet_create_reply(packet, "is", ret, pkgname);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *client_subscribed(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid argument\n");
		ret = -EINVAL;
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
	if (ret == 0)
		package_alter_instances_to_client(client, ALTER_CREATE);

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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "s", &cluster);
	if (ret != 1) {
		ErrPrint("Invalid parameters\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] cluster[%s]\n", pid, cluster);

	if (!strlen(cluster) || !strcasecmp(cluster, DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster: %s\n", cluster);
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \todo
	 */
	ret = -ENOSYS;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");
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
		inst = instance_create(NULL, timestamp, pkgname, DEFAULT_CONTENT, c_name, s_name, DEFAULT_PERIOD, 0, 0);
		if (!inst)
			ErrPrint("Failed to create a new instance\n");
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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Cilent %d is not exists\n", pid);
		ret = -ENOMEM;
		goto out;
	}

	ret = packet_get(packet, "ss", &pkgname, &id);
	if (ret != 2) {
		ErrPrint("Invalid argument\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ret = -EFAULT;
	} else if (instance_client(inst) != client) {
		ret = -EPERM;
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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Cilent %d is not exists\n", pid);
		ret = -ENOMEM;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster_id, &category_id);
	if (ret != 2) {
		ErrPrint("Invalid parameter\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("[%d] cluster[%s] category[%s]\n", pid, cluster_id, category_id);

	if (!strlen(cluster_id) || !strcasecmp(cluster_id, DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster name: %s\n", cluster_id);
		ret = -EINVAL;
		goto out;
	}

	cluster = group_find_cluster(cluster_id);
	if (!cluster) {
		ErrPrint("Cluster [%s] is not registered\n", cluster_id);
		ret = -EINVAL;
		goto out;
	}

	category = group_find_category(cluster, category_id);
	if (!category) {
		ErrPrint("Category [%s] is not registered\n", category_id);
		ret = -EINVAL;
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

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid paramenters\n");
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("pid[%d] cluster[%s] category[%s]\n", pid, cluster, category);
	if (!strlen(cluster) || !strcasecmp(cluster, DEFAULT_CLUSTER)) {
		ErrPrint("Invalid cluster: %s\n", cluster);
		ret = -EINVAL;
		goto out;
	}

	/*!
	 * \todo
	 */
	ret = -ENOSYS;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");
	return result;
}

static struct packet *client_unsubscribed(pid_t pid, int handle, const struct packet *packet)
{
	const char *cluster;
	const char *category;
	struct client_node *client;
	int ret;

	client = client_find_by_pid(pid);
	if (!client) {
		ErrPrint("Client %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ss", &cluster, &category);
	if (ret != 2) {
		ErrPrint("Invalid argument\n");
		ret = -EINVAL;
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
	if (ret == 0)
		package_alter_instances_to_client(client, ALTER_DESTROY);

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
		ret = -EINVAL;
		goto out;
	}

	DbgPrint("New slave[%s](%d) is arrived\n", slavename, pid);

	slave = slave_find_by_pid(pid);
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

				slave = slave_create(slavename, 1, abi, pkgname);
				if (!slave) {
					ErrPrint("Failed to create a new slave for %s\n", slavename);
					ret = -EFAULT;
					goto out;
				}

				DbgPrint("New slave is created\n");
			} else {
				DbgPrint("Registered slave is replaced with this new one\n");
				abi = slave_abi(slave);
			}

			slave_set_pid(slave, pid);
			DbgPrint("Provider is forcely activated, pkgname(%s), abi(%s), slavename(%s)\n", pkgname, abi, slavename);
		} else {
			ErrPrint("Slave[%d] is not exists\n", pid);
			ret = -ENOENT;
			goto out;
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
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "s", &slavename);
	if (ret != 1) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	slave_rpc_ping(slave);

out:
	return NULL;
}

static struct packet *slave_faulted(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct inst_info *inst;
	const char *slavename;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssss", &slavename, &pkgname, &id, &func);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	ret = fault_info_set(slave, pkgname, id, func);
	DbgPrint("Slave Faulted: %s (%d)\n", slavename, ret);

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		DbgPrint("There is a no such instance(%s)\n", id);
		ret = -ENOENT;
	} else if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Instance(%s) is already destroyed\n", id);
		ret = -EINVAL;
	} else {
		DbgPrint("Destroy instance(%s)\n", id);
		ret = instance_destroy(inst);
	}

out:
	return NULL;
}

static struct packet *slave_call(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, function, ret */
{
	struct slave_node *slave;
	const char *slavename;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssss", &slavename, &pkgname, &id, &func);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
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
	const char *slavename;
	const char *pkgname;
	const char *id;
	const char *func;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssss", &slavename, &pkgname, &id, &func);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
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
	const char *slavename;
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
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "sssiidss", &slavename, &pkgname, &id,
						&w, &h, &priority,
						&content_info, &title);
	if (ret != 8) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ErrPrint("Faulted instance cannot make any event.\n");
		ret = -EFAULT;
	} else if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Instance is already destroyed\n");
		ret = -EINVAL;
	} else {
		char *filename;
		int resized;

		resized = (instance_lb_width(inst) != w) || (instance_lb_height(inst) != h);
		instance_set_lb_info(inst, w, h, priority, content_info, title);

		switch (package_lb_type(instance_package(inst))) {
		case LB_TYPE_SCRIPT:
			script_handler_resize(instance_lb_script(inst), w, h);

			filename = util_get_file_kept_in_safe(id);
			if (filename) {
				ret = script_handler_parse_desc(pkgname, id,
								filename, 0);
				DbgFree(filename);
			} else {
				ret = script_handler_parse_desc(pkgname, id,
							util_uri_to_path(id), 0);
			}
			break;
		case LB_TYPE_BUFFER:
		default:
			/*!
			 * \check
			 * text format (inst)
			 */
			if (resized)
				instance_send_resized_event(inst, IS_LB, w, h, 0);
			instance_lb_updated_by_instance(inst);
			ret = 0;
			break;
		}

		slave_give_more_ttl(slave);
	}

out:
	return NULL;
}

static struct packet *slave_desc_updated(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, filename, decsfile, ret */
{
	struct slave_node *slave;
	const char *slavename;
	const char *pkgname;
	const char *id;
	const char *descfile;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "ssss", &slavename, &pkgname, &id, &descfile);
	if (ret != 4) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ret = -ENOENT;
	} else if (package_is_fault(instance_package(inst))) {
		ErrPrint("Faulted package cannot make event\n");
		ret = -EFAULT;
	} else if (instance_state(inst) == INST_DESTROYED) {
		ErrPrint("Instance is already destroyed\n");
		ret = -EINVAL;
	} else {
		switch (package_pd_type(instance_package(inst))) {
		case PD_TYPE_SCRIPT:
			DbgPrint("Script (%s)\n", id);
			if (script_handler_is_loaded(instance_pd_script(inst))) {
				ret = script_handler_parse_desc(pkgname, id,
								descfile, 1);
			}
			break;
		case PD_TYPE_TEXT:
			instance_set_pd_info(inst, 0, 0);
		case PD_TYPE_BUFFER:
			instance_pd_updated(pkgname, id, descfile);
			ret = 0;
			break;
		default:
			DbgPrint("Ignore updated DESC(%s - %s - %s)\n",
							pkgname, id, descfile);
			ret = 0;
			break;
		}
	}

out:
	return NULL;
}

static struct packet *slave_deleted(pid_t pid, int handle, const struct packet *packet) /* slave_name, pkgname, id, ret */
{
	struct slave_node *slave;
	const char *slavename;
	const char *pkgname;
	const char *id;
	int ret;
	struct inst_info *inst;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Slave %d is not exists\n", pid);
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "sss", &slavename, &pkgname, &id);
	if (ret != 3) {
		ErrPrint("Parameter is not matched\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst)
		ret = -ENOENT;
	else if (package_is_fault(instance_package(inst)))
		ret = -EFAULT;
	else
		ret = instance_destroyed(inst);

out:
	return NULL;
}

/*!
 * \note for the BUFFER Type slave
 */
static struct packet *slave_acquire_buffer(pid_t pid, int handle, const struct packet *packet) /* type, id, w, h, size */
{
	enum target_type target;
	const char *slavename;
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
		ret = -ENOENT;
		goto out;
	}

	ret = packet_get(packet, "isssiii", &target, &slavename, &pkgname, &id, &w, &h, &pixel_size);
	if (ret != 7) {
		ErrPrint("Invalid argument\n");
		id = "";
		ret = -EINVAL;
		goto out;
	}

	if (util_free_space(IMAGE_PATH) < MINIMUM_SPACE) {
		DbgPrint("No space\n");
		ret = -ENOSPC;
		id = "";
		goto out;
	}

	/* TODO: */
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		DbgPrint("Package[%s] Id[%s] is not found\n", pkgname, id);
		ret = -EINVAL;
		id = "";
		goto out;
	}

	pkg = instance_package(inst);
	id = "";
	ret = -EINVAL;
	if (target == TYPE_LB) {
		if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
			struct buffer_info *info;

			info = instance_lb_buffer(inst);
			if (!info) {
				if (!instance_create_lb_buffer(inst)) {
					ErrPrint("Failed to create a LB buffer\n");
				} else {
					info = instance_lb_buffer(inst);
					if (!info) {
						ErrPrint("LB buffer is not valid\n");
						ret = -EINVAL;
						id = "";
						goto out;
					}
				}
			}

			ret = buffer_handler_resize(info, w, h);
			DbgPrint("Buffer resize returns %d\n", ret);

			ret = buffer_handler_load(info);
			if (ret == 0) {
				int resized;
				resized = (instance_lb_width(inst) != w) || (instance_lb_height(inst) != h);
				instance_set_lb_info(inst, w, h, -1.0f, NULL, NULL);
				id = buffer_handler_id(info);
				if (resized)
					instance_send_resized_event(inst, IS_LB, w, h, 0);
				DbgPrint("Buffer handler ID: %s\n", id);
			} else {
				DbgPrint("Failed to load a buffer(%d)\n", ret);
			}
		}
	} else if (target == TYPE_PD) {
		if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
			struct buffer_info *info;

			DbgPrint("Slave acquire buffer for PD\n");

			info = instance_pd_buffer(inst);
			if (!info) {
				if (!instance_create_pd_buffer(inst)) {
					ErrPrint("Failed to create a PD buffer\n");
				} else {
					info = instance_pd_buffer(inst);
					if (!info) {
						ErrPrint("PD buffer is not valid\n");
						ret = -EINVAL;
						id = "";
						instance_client_pd_created(inst, ret);
						goto out;
					}
				}
			}

			ret = buffer_handler_resize(info, w, h);
			DbgPrint("Buffer resize returns %d\n", ret);

			ret = buffer_handler_load(info);
			if (ret == 0) {
				int resized;
				resized = (instance_pd_width(inst) != w) || (instance_pd_height(inst) != h);
				instance_set_pd_info(inst, w, h);
				id = buffer_handler_id(info);
				if (resized)
					instance_send_resized_event(inst, IS_PD, w, h, 0);
				DbgPrint("Buffer handler ID: %s\n", id);
			} else {
				DbgPrint("Failed to load a buffer (%d)\n", ret);
			}

			/*!
			 * Send the PD created event to the client
			 */
			instance_client_pd_created(inst, ret);
		}
	}

out:
	result = packet_create_reply(packet, "is", ret, id);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_resize_buffer(pid_t pid, int handle, const struct packet *packet)
{
	struct slave_node *slave;
	struct packet *result;
	enum target_type type;
	const char *slavename;
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
		ret = -ENOENT;
		id = "";
		goto out;
	}

	if (util_free_space(IMAGE_PATH) < MINIMUM_SPACE) {
		ErrPrint("Not enough space\n");
		ret = -ENOSPC;
		id = "";
		goto out;
	}

	ret = packet_get(packet, "isssii", &type, &slavename, &pkgname, &id, &w, &h);
	if (ret != 6) {
		ErrPrint("Invalid argument\n");
		ret = -EINVAL;
		id = "";
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		DbgPrint("Instance is not found[%s] [%s]\n", pkgname, id);
		ret = -ENOENT;
		id = "";
		goto out;
	}

	pkg = instance_package(inst);
	if (!pkg) {
		/*!
		 * \note
		 * THIS statement should not be entered.
		 */
		ErrPrint("PACKAGE INFORMATION IS NOT VALID\n");
		ret = -EFAULT;
		id = "";
		goto out;
	}

	ret = -EINVAL;
	/*!
	 * \note
	 * Reset "id", It will be re-used from here
	 */
	id = "";
	if (type == TYPE_LB) {
		if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
			struct buffer_info *info;

			info = instance_lb_buffer(inst);
			if (info) {
				ret = buffer_handler_resize(info, w, h);
				/*!
				 * \note
				 * id is resued for newly assigned ID
				 */
				if (!ret) {
					int resized;

					id = buffer_handler_id(info);
					resized = (instance_lb_width(inst) != w) || (instance_lb_height(inst) != h);
					instance_set_lb_info(inst, w, h, -1.0f, NULL, NULL);
					if (resized)
						instance_send_resized_event(inst, IS_LB, w, h, 0);
				}
			}
		}
	} else if (type == TYPE_PD) {
		if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
			struct buffer_info *info;

			info = instance_pd_buffer(inst);
			if (info) {
				ret = buffer_handler_resize(info, w, h);
				/*!
				 * \note
				 * id is resued for newly assigned ID
				 */
				if (!ret) {
					int resized;
					id = buffer_handler_id(info);
					resized = (instance_pd_width(inst) != w) || (instance_pd_height(inst) != h);
					instance_set_pd_info(inst, w, h);
					if (resized)
						instance_send_resized_event(inst, IS_PD, w, h, 0);
				}
			}
		}
	}

out:
	result = packet_create_reply(packet, "is", ret, id);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *slave_release_buffer(pid_t pid, int handle, const struct packet *packet)
{
	enum target_type type;
	const char *slavename;
	const char *pkgname;
	const char *id;
	struct packet *result;
	struct slave_node *slave;
	struct inst_info *inst;
	int ret;

	slave = slave_find_by_pid(pid);
	if (!slave) {
		ErrPrint("Failed to find a slave\n");
		ret = -ENOENT;
		goto out;
	}

	if (packet_get(packet, "isss", &type, &slavename, &pkgname, &id) != 4) {
		ErrPrint("Inavlid argument\n");
		ret = -EINVAL;
		goto out;
	}

	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance is not found [%s - %s]\n", pkgname, id);
		ret = -ENOENT;
		goto out;
	}

	ret = -EINVAL;
	if (type == TYPE_LB) {
		struct buffer_info *info;

		info = instance_lb_buffer(inst);
		ret = buffer_handler_unload(info);
	} else if (type == TYPE_PD) {
		struct buffer_info *info;

		DbgPrint("Slave release buffer for PD\n");

		info = instance_pd_buffer(inst);
		ret = buffer_handler_unload(info);

		/*!
		 * \note
		 * Send the PD destroyed event to the client
		 */
		instance_client_pd_destroyed(inst, ret);
	}

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

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
		ret = -EINVAL;
		goto out;
	}

	if (!strlen(id)) {
		struct pkg_info *pkg;

		pkg = package_find(pkgname);
		if (!pkg) {
			ret = -ENOENT;
		} else if (package_is_fault(pkg)) {
			ret = -EFAULT;
		} else {
			Eina_List *inst_list;
			Eina_List *l;

			inst_list = package_instance_list(pkg);
			EINA_LIST_FOREACH(inst_list, l, inst) {
				ret = instance_set_period(inst, period);
				if (ret < 0)
					DbgPrint("Failed to change the period of %s to (%lf)\n", instance_id(inst), period);
			}
		}
	} else {
		inst = package_find_instance_by_id(pkgname, id);
		if (!inst)
			ret = -ENOENT;
		else if (package_is_fault(instance_package(inst)))
			ret = -EFAULT;
		else
			ret = instance_set_period(inst, period);
	}

	DbgPrint("Change the update period: %s(%s), %lf : %d\n", pkgname, id, period, ret);
out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

	return result;
}

static struct packet *service_update(pid_t pid, int handle, const struct packet *packet)
{
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
		ret = -EINVAL;
		goto out;
	}

	lb_pkgname = package_lb_pkgname(pkgname);
	if (!lb_pkgname) {
		ErrPrint("Invalid package %s\n", pkgname);
		ret = -EINVAL;
		goto out;
	}

	pkg = package_find(lb_pkgname);
	if (!pkg) {
		ret = -ENOENT;
		goto out;
	}

	if (package_is_fault(pkg)) {
		ret = -EFAULT;
		goto out;
	}

	/*!
	 * \TODO
	 * Validate the update requstor.
	 */
	slave_rpc_request_update(lb_pkgname, id, cluster, category);
	DbgFree(lb_pkgname);
	ret = 0;

out:
	result = packet_create_reply(packet, "i", ret);
	if (!result)
		ErrPrint("Failed to create a packet\n");

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
		ret = -EINVAL;
		goto out;
	}

	info = liveinfo_create(pid, handle);
	if (!info) {
		ErrPrint("Failed to create a liveinfo object\n");
		fifo_name = "";
		ret = -EINVAL;
		goto out;
	}

	ret = 0;
	fifo_name = liveinfo_filename(info);
	DbgPrint("FIFO Created: %s (Serve for %d)\n", fifo_name, pid);

out:
	result = packet_create_reply(packet, "si", fifo_name, ret);
	if (!result)
		ErrPrint("Failed to create a result packet\n");

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
			instance_destroy(inst);
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
	int ret = -EINVAL;

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
		.cmd = "pd_access_read",
		.handler = client_pd_access_read,
	},
	{
		.cmd = "pd_access_read_prev",
		.handler = client_pd_access_read_prev,
	},
	{
		.cmd = "pd_access_read_next",
		.handler = client_pd_access_read_next,
	},
	{
		.cmd = "pd_access_activate",
		.handler = client_pd_access_activate,
	},

	{
		.cmd = "lb_access_read",
		.handler = client_lb_access_read,
	},
	{
		.cmd = "lb_access_read_prev",
		.handler = client_lb_access_read_prev,
	},
	{
		.cmd = "lb_access_read_next",
		.handler = client_lb_access_read_next,
	},
	{
		.cmd = "lb_access_activate",
		.handler = client_lb_access_activate,
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
		.cmd = "hello",
		.handler = slave_hello, /* slave_name, ret */
	},
	{
		.cmd = "ping",
		.handler = slave_ping, /* slave_name, ret */
	},
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
		.cmd = NULL,
		.handler = NULL,
	},
};

HAPI int server_init(void)
{
	com_core_packet_use_thread(COM_CORE_THREAD);

	if (unlink(INFO_SOCKET) < 0)
		ErrPrint("info socket: %s\n", strerror(errno));

	if (unlink(SLAVE_SOCKET) < 0)
		ErrPrint("slave socket: %s\n", strerror(errno));

	if (unlink(CLIENT_SOCKET) < 0)
		ErrPrint("client socket: %s\n", strerror(errno));

	if (unlink(SERVICE_SOCKET) < 0)
		ErrPrint("service socket: %s\n", strerror(errno));

	s_info.info_fd = com_core_packet_server_init(INFO_SOCKET, s_info_table);
	if (s_info.info_fd < 0)
		ErrPrint("Failed to create a info socket\n");

	s_info.slave_fd = com_core_packet_server_init(SLAVE_SOCKET, s_slave_table);
	if (s_info.slave_fd < 0)
		ErrPrint("Failed to create a slave socket\n");

	s_info.client_fd = com_core_packet_server_init(CLIENT_SOCKET, s_client_table);
	if (s_info.client_fd < 0)
		ErrPrint("Failed to create a client socket\n");

	s_info.service_fd = com_core_packet_server_init(SERVICE_SOCKET, s_service_table);
	if (s_info.service_fd < 0)
		ErrPrint("Faild to create a service socket\n");

	if (chmod(INFO_SOCKET, 0600) < 0)
		ErrPrint("info socket: %s\n", strerror(errno));

	if (chmod(SLAVE_SOCKET, 0666) < 0)
		ErrPrint("slave socket: %s\n", strerror(errno));

	if (chmod(CLIENT_SOCKET, 0666) < 0)
		ErrPrint("client socket: %s\n", strerror(errno));

	if (chmod(SERVICE_SOCKET, 0666) < 0)
		ErrPrint("service socket: %s\n", strerror(errno));

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

	if (s_info.service_fd > 0) {
		com_core_packet_server_fini(s_info.service_fd);
		s_info.service_fd = -1;
	}

	return 0;
}

/* End of a file */
