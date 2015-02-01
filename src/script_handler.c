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
#include <stdlib.h> /* free */
#include <ctype.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <Ecore.h>
#include <Eina.h>

#include <dlog.h>
#include <packet.h>
#include <dynamicbox_errno.h>
#include <dynamicbox_service.h>
#include <dynamicbox_conf.h>

#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "package.h"
#include "instance.h"
#include "buffer_handler.h"
#include "script_handler.h"
#include "debug.h"
#include "conf.h"
#include "util.h"

#define INFO_SIZE "size"
#define INFO_CATEGORY "category"

static const char *type_list[] = {
	"access",
	"access,operation",
	"color",
	"drag",
	"image",
	"info",
	"script",
	"signal",
	"text",
	NULL
};

static const char *field_list[] = {
	"type",
	"part",
	"data",
	"option",
	"id",
	"target",
	"file",
	NULL
};

int errno;

static struct info {
	Eina_List *script_port_list;
	enum dynamicbox_fb_type env_buf_type;
} s_info = {
	.script_port_list = NULL,
	.env_buf_type = DBOX_FB_TYPE_FILE,
};

struct script_port {
	void *handle;

	const char *(*magic_id)(void);
	int (*update_color)(void *handle, const char *id, const char *part, const char *rgba);
	int (*update_text)(void *handle, const char *id, const char *part, const char *text);
	int (*update_image)(void *handle, const char *id, const char *part, const char *path, const char *option);
	int (*update_access)(void *handle, const char *id, const char *part, const char *text, const char *option);
	int (*operate_access)(void *handle, const char *id, const char *part, const char *operation, const char *option);
	int (*update_script)(void *handle, const char *src_id, const char *target_id, const char *part, const char *path, const char *option);
	int (*update_signal)(void *handle, const char *id, const char *part, const char *signal);
	int (*update_drag)(void *handle, const char *id, const char *part, double x, double y);
	int (*update_size)(void *handle, const char *id, int w, int h);
	int (*update_category)(void *handle, const char *id, const char *category);
	int (*feed_event)(void *handle, int event_type, int x, int y, int down, unsigned int keycode, double timestamp);

	void *(*create)(void *buffer_info, const char *file, const char *option);
	int (*destroy)(void *handle);

	int (*load)(void *handle, int (*render_pre)(void *buffer_info, void *data), int (*render_post)(void *buffer_info, void *data), void *data);
	int (*unload)(void *handle);

	int (*init)(double scale, int premultiplied);
	int (*fini)(void);
};

enum block_type {
	TYPE_ACCESS,
	TYPE_ACCESS_OP,
	TYPE_COLOR,
	TYPE_DRAG,
	TYPE_IMAGE,
	TYPE_INFO,
	TYPE_SCRIPT,
	TYPE_SIGNAL,
	TYPE_TEXT,
	TYPE_MAX
};

enum field_type {
	FIELD_TYPE,
	FIELD_PART,
	FIELD_DATA,
	FIELD_OPTION,
	FIELD_ID,
	FIELD_TARGET,
	FIELD_FILE
};

struct block {
	enum block_type type;
	char *part;
	char *data;
	char *option;
	char *id;
	char *target;
	char *file;

	/* Should be released */
	char *filebuf;
	const char *filename;
};

struct script_info {
	struct buffer_info *buffer_handle; 
	int loaded;

	int w;
	int h;

	int x;
	int y;
	int down;

	unsigned int keycode;

	struct script_port *port;
	void *port_data;

	Eina_List *cached_blocks;
};

static inline void consuming_parsed_block(struct inst_info *inst, int is_pd, struct block *block);

static int load_all_ports(void)
{
	struct script_port *item;
	struct dirent *ent;
	DIR *dir;
	char *path;
	int pathlen;

	dir = opendir(DYNAMICBOX_CONF_SCRIPT_PORT_PATH);
	if (!dir) {
		ErrPrint("Error: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_IO_ERROR;
	}

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.') {
			continue;
		}

		pathlen = strlen(ent->d_name) + strlen(DYNAMICBOX_CONF_SCRIPT_PORT_PATH) + 1;
		path = malloc(pathlen);
		if (!path) {
			ErrPrint("Heap: %s %d\n", strerror(errno), pathlen);
			if (closedir(dir) < 0) {
				ErrPrint("closedir: %s\n", strerror(errno));
			}
			return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
		}

		snprintf(path, pathlen, "%s%s", DYNAMICBOX_CONF_SCRIPT_PORT_PATH, ent->d_name);

		item = malloc(sizeof(*item));
		if (!item) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(path);
			if (closedir(dir) < 0) {
				ErrPrint("closedir: %s\n", strerror(errno));
			}
			return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
		}

		DbgPrint("Open SCRIPT PORT: %s\n", path);
		item->handle = dlopen(path, RTLD_GLOBAL | RTLD_NOW | RTLD_DEEPBIND);
		DbgFree(path);
		if (!item->handle) {
			ErrPrint("Error: %s\n", dlerror());
			DbgFree(item);
			if (closedir(dir) < 0) {
				ErrPrint("closedir: %s\n", strerror(errno));
			}
			return DBOX_STATUS_ERROR_FAULT;
		}

		item->magic_id = dlsym(item->handle, "script_magic_id");
		if (!item->magic_id) {
			goto errout;
		}

		DbgPrint("SCRIPT PORT magic id: %s\n", item->magic_id());

		item->update_color = dlsym(item->handle, "script_update_color");
		if (!item->update_color) {
			goto errout;
		}

		item->update_text = dlsym(item->handle, "script_update_text");
		if (!item->update_text) {
			goto errout;
		}

		item->update_image = dlsym(item->handle, "script_update_image");
		if (!item->update_image) {
			goto errout;
		}

		item->update_access = dlsym(item->handle, "script_update_access");
		if (!item->update_access) {
			goto errout;
		}

		item->operate_access = dlsym(item->handle, "script_operate_access");
		if (!item->operate_access) {
			goto errout;
		}

		item->update_script = dlsym(item->handle, "script_update_script");
		if (!item->update_script) {
			goto errout;
		}

		item->update_signal = dlsym(item->handle, "script_update_signal");
		if (!item->update_signal) {
			goto errout;
		}

		item->update_drag = dlsym(item->handle, "script_update_drag");
		if (!item->update_drag) {
			goto errout;
		}

		item->update_size = dlsym(item->handle, "script_update_size");
		if (!item->update_size) {
			goto errout;
		}

		item->update_category = dlsym(item->handle, "script_update_category");
		if (!item->update_category) {
			goto errout;
		}

		item->create = dlsym(item->handle, "script_create");
		if (!item->create) {
			goto errout;
		}

		item->destroy = dlsym(item->handle, "script_destroy");
		if (!item->destroy) {
			goto errout;
		}

		item->load = dlsym(item->handle, "script_load");
		if (!item->load) {
			goto errout;
		}

		item->unload = dlsym(item->handle, "script_unload");
		if (!item->unload) {
			goto errout;
		}

		item->init = dlsym(item->handle, "script_init");
		if (!item->init) {
			goto errout;
		}

		item->fini = dlsym(item->handle, "script_fini");
		if (!item->fini) {
			goto errout;
		}

		item->feed_event = dlsym(item->handle, "script_feed_event");
		if (!item->feed_event) {
			goto errout;
		}

		if (item->init(DYNAMICBOX_CONF_SCALE_WIDTH_FACTOR, DYNAMICBOX_CONF_PREMULTIPLIED_COLOR) < 0) {
			ErrPrint("Failed to initialize script engine\n");
			goto errout;
		}

		s_info.script_port_list = eina_list_append(s_info.script_port_list, item);
	}

	if (closedir(dir) < 0) {
		ErrPrint("closedir: %s\n", strerror(errno));
	}

	return DBOX_STATUS_ERROR_NONE;

errout:
	ErrPrint("Error: %s\n", dlerror());
	if (dlclose(item->handle) != 0) {
		ErrPrint("dlclose: %s\n", strerror(errno));
	}
	DbgFree(item);
	if (closedir(dir) < 0) {
		ErrPrint("closedir: %s\n", strerror(errno));
	}
	return DBOX_STATUS_ERROR_FAULT;
}

static inline struct script_port *find_port(const char *magic_id)
{
	Eina_List *l;
	struct script_port *item;

	if (!s_info.script_port_list) {
		int ret;

		ret = load_all_ports();
		if (ret < 0) {
			ErrPrint("load_all_ports: %d\n", ret);
		}
	}

	EINA_LIST_FOREACH(s_info.script_port_list, l, item) {
		if (!strcmp(item->magic_id(), magic_id)) {
			return item;
		}
	}

	return NULL;
}

static inline void delete_block(struct block *block)
{
	DbgFree(block->filebuf);
	DbgFree(block);
}

static int render_post_cb(void *_buffer_handle, void *data)
{
	PERF_INIT();
	PERF_BEGIN();
	struct inst_info *inst;
	struct buffer_info *buffer_handle = _buffer_handle;
	struct script_info *info;

	inst = buffer_handler_instance(buffer_handle);
	if (!inst) {
		goto out;
	}

	if (instance_state(inst) != INST_ACTIVATED) {
		ErrPrint("Render post invoked but instance is not activated\n");
		PERF_MARK(__func__);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = instance_dbox_script(inst);
	if (info && info == data) {
		buffer_handler_flush(buffer_handle);
		instance_dbox_updated_by_instance(inst, NULL, info->x, info->y, info->w, info->h);
		PERF_MARK("lb,update");
		return DBOX_STATUS_ERROR_NONE;
	}

	info = instance_gbar_script(inst);
	if (info && info == data) {
		buffer_handler_flush(buffer_handle);
		instance_gbar_updated_by_instance(inst, NULL, info->x, info->y, info->w, info->h);
		PERF_MARK("pd,update");
		return DBOX_STATUS_ERROR_NONE;
	}

out:
	ErrPrint("Failed to sync\n");
	PERF_MARK(__func__);
	return DBOX_STATUS_ERROR_FAULT;
}

/*!
 * \NOTE
 * Exported API
 */
EAPI int script_signal_emit(void *buffer_handle, const char *part, const char *signal, double sx, double sy, double ex, double ey)
{
	struct script_info *info;
	struct inst_info *inst;
	int w;
	int h;
	double fx;
	double fy;

	if (!buffer_handle) {
		ErrPrint("Invalid handle\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = buffer_handler_data(buffer_handle);
	if (!info) {
		ErrPrint("Invalid handle\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	inst = buffer_handler_instance(buffer_handle);
	if (!inst) {
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (!signal || strlen(signal) == 0) {
		signal = "";
	}

	if (!part || strlen(part) == 0) {
		part = "";
	}

	buffer_handler_get_size(buffer_handle, &w, &h);

	fx = (double)info->x / (double)w;
	fy = (double)info->y / (double)h;

	return instance_signal_emit(inst, signal, part, sx, sy, ex, ey, fx, fy, info->down);
}

static inline void flushing_cached_block(struct script_info *info)
{
	struct block *block;
	struct inst_info *inst;
	int is_pd;

	inst = buffer_handler_instance(info->buffer_handle);
	if (!inst) {
		ErrPrint("Instance is not valid\n");
		EINA_LIST_FREE(info->cached_blocks, block) {
			delete_block(block);
		}
		return;
	}

	is_pd = instance_gbar_script(inst) == info;

	EINA_LIST_FREE(info->cached_blocks, block) {
		consuming_parsed_block(inst, is_pd, block);
	}
}

HAPI int script_handler_load(struct script_info *info, int is_pd)
{
	struct inst_info *inst;

	if (!info || !info->port) {
		ErrPrint("Script handler is not created\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (info->loaded > 0) {
		info->loaded++;
		return DBOX_STATUS_ERROR_NONE;
	}

	if (info->port->load(info->port_data, NULL, render_post_cb, info) < 0) {
		ErrPrint("Unable to load the script\n");
		return DBOX_STATUS_ERROR_FAULT;
	}

	info->loaded = 1;
	flushing_cached_block(info);

	inst = buffer_handler_instance(info->buffer_handle);
	if (inst) {
		script_signal_emit(info->buffer_handle, instance_id(inst),
				is_pd ? "gbar,show" : "dbox,show", 0.0f, 0.0f, 0.0f, 0.0f);
	}
	buffer_handler_flush(info->buffer_handle);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int script_handler_unload(struct script_info *info, int is_pd)
{
	struct inst_info *inst;

	if (!info || !info->port) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info->loaded--;
	if (info->loaded > 0) {
		return DBOX_STATUS_ERROR_BUSY;
	}

	if (info->loaded < 0) {
		info->loaded = 0;
		return DBOX_STATUS_ERROR_ALREADY;
	}

	inst = buffer_handler_instance(info->buffer_handle);
	if (inst) {
		script_signal_emit(info->buffer_handle, instance_id(inst),
				is_pd ? "gbar,hide" : "dbox,hide", 0.0f, 0.0f, 0.0f, 0.0f);
	}

	if (info->port->unload(info->port_data) < 0) {
		ErrPrint("Failed to unload script object. but go ahead\n");
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI struct script_info *script_handler_create(struct inst_info *inst, const char *file, const char *option, int w, int h)
{
	struct script_info *info;

	DbgPrint("Create script: %s (%s) %dx%d\n", file, option, w, h);

	if (!file) {
		return NULL;
	}

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	info->buffer_handle = buffer_handler_create(inst, s_info.env_buf_type, w, h, DYNAMICBOX_CONF_DEFAULT_PIXELS);
	if (!info->buffer_handle) {
		/* buffer_handler_create will prints some log */
		DbgFree(info);
		return NULL;
	}

	(void)buffer_handler_set_data(info->buffer_handle, info);

	info->port = find_port(package_script(instance_package(inst)));
	if (!info->port) {
		ErrPrint("Failed to find a proper port for [%s]%s\n",
				instance_package(inst), package_script(instance_package(inst)));
		buffer_handler_destroy(info->buffer_handle);
		DbgFree(info);
		return NULL;
	}

	info->port_data = info->port->create(info->buffer_handle, file, option);
	if (!info->port_data) {
		ErrPrint("Failed to create a port (%s - %s)\n", file, option);
		buffer_handler_destroy(info->buffer_handle);
		DbgFree(info);
		return NULL;
	}

	return info;
}

HAPI int script_handler_destroy(struct script_info *info)
{
	struct block *block;
	int ret;

	if (!info || !info->port) {
		ErrPrint("port is not valid\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (info->loaded != 0) {
		ErrPrint("Script handler is not unloaded\n");
		return DBOX_STATUS_ERROR_BUSY;
	}

	ret = info->port->destroy(info->port_data);
	if (ret < 0) {
		ErrPrint("Failed to destroy port, but go ahead: %d\n", ret);
	}

	(void)buffer_handler_destroy(info->buffer_handle);

	EINA_LIST_FREE(info->cached_blocks, block) {
		delete_block(block);
	}

	DbgFree(info);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int script_handler_is_loaded(struct script_info *info)
{
	return info ? info->loaded > 0 : 0;
}

HAPI struct buffer_info *script_handler_buffer_info(struct script_info *info)
{
	if (!info) {
		return NULL;
	}

	return info->buffer_handle;
}

static int update_script_color(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	int ret;

	if (!block || !block->part || !block->data) {
		ErrPrint("Block or part or data is not valid\n");
		PERF_MARK("color");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("color");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("color");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	ret = info->port->update_color(info->port_data, block->id, block->part, block->data);
	PERF_MARK("color");
	return ret;
}

static int update_script_text(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	int ret;

	if (!block || !block->part || !block->data) {
		ErrPrint("Block or part or data is not valid\n");
		PERF_MARK("text");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("text");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		PERF_MARK("text");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	DbgPrint("[%s] %s (%s)\n", block->id, block->part, block->data);
	ret = info->port->update_text(info->port_data, block->id, block->part, block->data);

	PERF_MARK("text");
	return ret;
}

static int update_script_image(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	int ret;

	if (!block || !block->part) {
		ErrPrint("Block or part is not valid\n");
		PERF_MARK("image");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("image");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		PERF_MARK("image");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	DbgPrint("[%s] %s (%s)\n", block->id, block->part, block->data);
	ret = info->port->update_image(info->port_data, block->id, block->part, block->data, block->option);
	PERF_MARK("image");
	return ret;
}

static int update_access(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	int ret;

	if (!block || !block->part) {
		ErrPrint("Block or block->part is NIL\n");
		PERF_MARK("access");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("access");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		PERF_MARK("access");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	ret = info->port->update_access(info->port_data, block->id, block->part, block->data, block->option);
	PERF_MARK("access");
	return ret;
}

static int operate_access(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	int ret;

	if (!block || !block->part) {
		ErrPrint("Block or block->part is NIL\n");
		PERF_MARK("operate_access");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("operate_access");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		PERF_MARK("operate_access");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	ret = info->port->operate_access(info->port_data, block->id, block->part, block->data, block->option);
	PERF_MARK("operate_access");
	return ret;
}

static int update_script_script(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	int ret;

	if (!block || !block->part) {
		ErrPrint("Block or part is NIL\n");
		PERF_MARK("script");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("script");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		PERF_MARK("script");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	ret = info->port->update_script(info->port_data, block->id, block->target, block->part, block->data, block->option);
	PERF_MARK("script");
	return ret;
}

static int update_script_signal(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	int ret;

	if (!block) {
		ErrPrint("block is NIL\n");
		PERF_MARK("signal");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("signal");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		PERF_MARK("signal");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	ret = info->port->update_signal(info->port_data, block->id, block->part, block->data);
	PERF_MARK("signal");
	return ret;
}

static int update_script_drag(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	double dx, dy;
	int ret;

	if (!block || !block->data || !block->part) {
		ErrPrint("block or block->data or block->part is NIL\n");
		PERF_MARK("drag");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("drag");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (sscanf(block->data, "%lfx%lf", &dx, &dy) != 2) {
		ErrPrint("Invalid format of data (DRAG data [%s])\n", block->data);
		PERF_MARK("drag");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		PERF_MARK("drag");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	ret = info->port->update_drag(info->port_data, block->id, block->part, dx, dy);
	PERF_MARK("drag");
	return ret;
}

static void update_size_for_script(struct script_info *info, struct inst_info *inst, int w, int h)
{
	/*!
	 * \note
	 * After update the buffer size,
	 * If it required to be unload and load.
	 * New size of buffer will be allocated
	 */
	buffer_handler_update_size(info->buffer_handle, w, h);

	if (info->port->update_size) {
		(void)info->port->update_size(info->port_data, NULL, w, h);
	}

	if (instance_dbox_script(inst) == info) {
		instance_set_dbox_size(inst, w, h);
	} else if (instance_gbar_script(inst) == info) {
		instance_set_gbar_size(inst, w, h);
	} else {
		ErrPrint("Unknown script\n");
	}
}

HAPI int script_handler_resize(struct script_info *info, int w, int h)
{
	PERF_INIT();
	PERF_BEGIN();
	struct inst_info *inst;

	if (!info) {
		ErrPrint("info[%p] resize is ignored\n", info);
		PERF_MARK("resize");
		return DBOX_STATUS_ERROR_NONE;
	}

	inst = buffer_handler_instance(info->buffer_handle);
	if (!inst) {
		ErrPrint("Instance is not valid\n");
		PERF_MARK("resize");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	update_size_for_script(info, inst, w, h);

	PERF_MARK("resize");
	return DBOX_STATUS_ERROR_NONE;
}

HAPI const char *script_handler_buffer_id(struct script_info *info)
{
	if (!info || !info->buffer_handle) {
		ErrPrint("Invalid info\n");
		return "";
	}

	return buffer_handler_id(info->buffer_handle);
}

static int update_info(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;

	if (!block || !block->part || !block->data) {
		ErrPrint("block or block->part or block->data is NIL\n");
		PERF_MARK("info");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		PERF_MARK("info");
		return DBOX_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		PERF_MARK("info");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (!strcasecmp(block->part, INFO_SIZE)) {
		int w, h;

		if (sscanf(block->data, "%dx%d", &w, &h) != 2) {
			ErrPrint("Invalid format for SIZE(%s)\n", block->data);
			PERF_MARK("info");
			return DBOX_STATUS_ERROR_INVALID_PARAMETER;
		}

		if (!block->id) {
			update_size_for_script(info, inst, w, h);
		} else {
			(void)info->port->update_size(info->port_data, block->id, w, h);
		}
	} else if (!strcasecmp(block->part, INFO_CATEGORY)) {
		(void)info->port->update_category(info->port_data, block->id, block->data);
	}
	PERF_MARK("info");

	return DBOX_STATUS_ERROR_NONE;
}

static inline void consuming_parsed_block(struct inst_info *inst, int is_pd, struct block *block)
{
	struct script_info *info;
	typedef int (*update_function_t)(struct inst_info *inst, struct block *block, int is_pd);
	update_function_t updators[] = {
		update_access,
		operate_access,
		update_script_color,
		update_script_drag,
		update_script_image,
		update_info,
		update_script_script,
		update_script_signal,
		update_script_text,
		NULL
	};

	info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		goto free_out;
	}

	if (script_handler_is_loaded(info)) {
		if (block->type >= 0 || block->type < TYPE_MAX) {
			(void)updators[block->type](inst, block, is_pd);
		} else {
			ErrPrint("Block type[%d] is not valid\n", block->type);
		}
	} else {
		info->cached_blocks = eina_list_append(info->cached_blocks, block);
		DbgPrint("Block is cached (%p), %d, %s\n", block, eina_list_count(info->cached_blocks), instance_id(inst));
		return;
	}

free_out:
	delete_block(block);
}

HAPI int script_init(void)
{
	if (!strcasecmp(DYNAMICBOX_CONF_PROVIDER_METHOD, "shm")) {
		s_info.env_buf_type = DBOX_FB_TYPE_SHM;
	} else if (!strcasecmp(DYNAMICBOX_CONF_PROVIDER_METHOD, "pixmap")) {
		s_info.env_buf_type = DBOX_FB_TYPE_PIXMAP;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int script_fini(void)
{
	struct script_port *item;
	/*!
	 * \TODO: Release all handles
	 */
	EINA_LIST_FREE(s_info.script_port_list, item) {
		item->fini();
		if (dlclose(item->handle) != 0) {
			ErrPrint("dlclose: %s\n", strerror(errno));
		}
		DbgFree(item);
	}

	return 0;
}

HAPI int script_handler_update_pointer(struct script_info *info, int x, int y, int down)
{
	if (!info) {
		return DBOX_STATUS_ERROR_NONE;
	}

	info->x = x;
	info->y = y;

	if (down == 0) {
		info->down = 0;
	} else if (down == 1) {
		info->down = 1;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int script_handler_update_keycode(struct script_info *info, unsigned int keycode)
{
	if (!info) {
		return DBOX_STATUS_ERROR_NONE;
	}

	info->keycode = keycode;

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int script_handler_feed_event(struct script_info *info, int event, double timestamp)
{
	int ret;

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	ret = info->port->feed_event(info->port_data, event, info->x, info->y, info->down, info->keycode, timestamp);
	return ret;
}

static inline char *load_file(const char *filename)
{
	char *filebuf = NULL;
	int fd;
	off_t filesize;
	int ret;
	size_t readsize = 0;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		ErrPrint("open: %s\n", strerror(errno));
		return NULL;
	}

	filesize = lseek(fd, 0L, SEEK_END);
	if (filesize == (off_t)-1) {
		ErrPrint("lseek: %s\n", strerror(errno));
		goto errout;
	}

	if (lseek(fd, 0L, SEEK_SET) < 0) {
		ErrPrint("lseek: %s\n", strerror(errno));
		goto errout;
	}

	filebuf = malloc(filesize + 1);
	if (!filebuf) {
		ErrPrint("malloc: %s\n", strerror(errno));
		goto errout;
	}

	while (readsize < filesize) {
		ret = read(fd, filebuf + readsize, (size_t)filesize - readsize);
		if (ret < 0) {
			if (errno == EINTR) {
				DbgPrint("Read is interrupted\n");
				continue;
			}

			ErrPrint("read: %s\n", strerror(errno));
			free(filebuf);
			filebuf = NULL;
			break;
		}

		readsize += ret;
	}

	if (filebuf) {
		filebuf[readsize] = '\0';
	}

	/*!
	 * \note
	 * Now, we are ready to parse the filebuf.
	 */

errout:
	if (close(fd) < 0) {
		ErrPrint("close: %s\n", strerror(errno));
	}

	return filebuf;
}

#if defined(_APPLY_SCRIPT_ASYNC_UPDATE)
struct apply_data {
	struct inst_info *inst;
	Eina_List *block_list;
	int is_pd;
};

static Eina_Bool apply_changes_cb(void *_data)
{
	struct apply_data *data = _data;
	struct block *block;

	block = eina_list_nth(data->block_list, 0);
	data->block_list = eina_list_remove(data->block_list, block);
	consuming_parsed_block(data->inst, data->is_pd, block);

	if (!data->block_list) {
		free(data);
		return ECORE_CALLBACK_CANCEL;
	}

	return ECORE_CALLBACK_RENEW;
}
#endif

HAPI int script_handler_parse_desc(struct inst_info *inst, const char *filename, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	int type_idx = 0;
	int type_len = 0;
	int field_idx = 0;
	int field_len = 0;
	char *filebuf;
	char *fileptr;
	char *ptr = NULL;
	struct block *block = NULL;
	Eina_List *block_list = NULL;
	enum state {
		BEGIN,
		FIELD,
		DATA,
		END,
		DONE,
		ERROR,
	} state;

	filebuf = load_file(filename);
	if (!filebuf) {
		return DBOX_STATUS_ERROR_IO_ERROR;
	}

	fileptr = filebuf;

	state = BEGIN;
	while (*fileptr && state != ERROR) {
		switch (state) {
		case BEGIN:
			if (*fileptr == '{') {
				block = calloc(1, sizeof(*block));
				if (!block) {
					ErrPrint("calloc: %s\n", strerror(errno));
					state = ERROR;
					continue;
				}
				state = FIELD;
				ptr = NULL;
			}
			break;
		case FIELD:
			if (isspace(*fileptr)) {
				if (ptr != NULL) {
					*fileptr = '\0';
				}
			} else if (*fileptr == '=') {
				*fileptr = '\0';
				ptr = NULL;
				state = DATA;
			} else if (ptr == NULL) {
				ptr = fileptr;
				field_idx = 0;
				field_len = 0;

				while (field_list[field_idx]) {
					if (field_list[field_idx][field_len] == *fileptr) {
						break;
					}
					field_idx++;
				}

				if (!field_list[field_idx]) {
					ErrPrint("Invalid field\n");
					state = ERROR;
					continue;
				}

				field_len++;
			} else {
				if (field_list[field_idx][field_len] != *fileptr) {
					field_idx++;
					while (field_list[field_idx]) {
						if (!strncmp(field_list[field_idx], fileptr - field_len, field_len)) {
							break;
						} else {
							field_idx++;
						}
					}

					if (!field_list[field_idx]) {
						state = ERROR;
						ErrPrint("field is not valid\n");
						continue;
					}
				}

				field_len++;
			}
			break;
		case DATA:
			switch (field_idx) {
			case FIELD_TYPE:
				if (ptr == NULL) {
					if (isspace(*fileptr)) {
						break;
					}

					if (*fileptr == '\0') {
						state = ERROR;
						ErrPrint("Type is not valid\n");
						continue;
					}

					ptr = fileptr;
					type_idx = 0;
					type_len = 0;
				}

				if (*fileptr && (*fileptr == '\n' || *fileptr == '\r' || *fileptr == '\f')) {
					*fileptr = '\0';
				}

				if (type_list[type_idx][type_len] != *fileptr) {
					type_idx++;
					while (type_list[type_idx]) {
						if (!strncmp(type_list[type_idx], fileptr - type_len, type_len)) {
							break;
						} else {
							type_idx++;
						}
					}

					if (!type_list[type_idx]) {
						state = ERROR;
						ErrPrint("type is not valid (%s)\n", fileptr - type_len);
						continue;
					}
				}

				if (!*fileptr) {
					block->type = type_idx;
					state = DONE;
					ptr = NULL;
				}

				type_len++;
				break;
			case FIELD_PART:
				if (ptr == NULL) {
					ptr = fileptr;
				}

				if (*fileptr && (*fileptr == '\n' || *fileptr == '\r' || *fileptr == '\f')) {
					*fileptr = '\0';
				}

				if (!*fileptr) {
					block->part = ptr;
					state = DONE;
					ptr = NULL;
				}
				break;
			case FIELD_DATA:
				if (ptr == NULL) {
					ptr = fileptr;
				}

				if (*fileptr && (*fileptr == '\n' || *fileptr == '\r' || *fileptr == '\f')) {
					*fileptr = '\0';
				}

				if (!*fileptr) {
					block->data = ptr;
					state = DONE;
					ptr = NULL;
				}
				break;
			case FIELD_OPTION:
				if (ptr == NULL) {
					ptr = fileptr;
				}

				if (*fileptr && (*fileptr == '\n' || *fileptr == '\r' || *fileptr == '\f')) {
					*fileptr = '\0';
				}

				if (!*fileptr) {
					block->option = ptr;
					state = DONE;
					ptr = NULL;
				}
				break;
			case FIELD_ID:
				if (ptr == NULL) {
					ptr = fileptr;
				}

				if (*fileptr && (*fileptr == '\n' || *fileptr == '\r' || *fileptr == '\f')) {
					*fileptr = '\0';
				}

				if (!*fileptr) {
					block->id = ptr;
					state = DONE;
					ptr = NULL;
				}
				break;
			case FIELD_TARGET:
				if (ptr == NULL) {
					ptr = fileptr;
				}

				if (*fileptr && (*fileptr == '\n' || *fileptr == '\r' || *fileptr == '\f')) {
					*fileptr = '\0';
				}

				if (!*fileptr) {
					block->target = ptr;
					state = DONE;
					ptr = NULL;
				}
				break;
			case FIELD_FILE:
				if (ptr == NULL) {
					ptr = fileptr;
				}

				if (*fileptr && (*fileptr == '\n' || *fileptr == '\r' || *fileptr == '\f')) {
					*fileptr = '\0';
				}

				if (!*fileptr) {
					block->target = ptr;
					state = DONE;
					ptr = NULL;
				}
			default:
				break;
			}

			break;
		case DONE:
			if (isspace(*fileptr)) {
			} else if (*fileptr == '}') {
				state = BEGIN;
				block->filename = filename;
				block_list = eina_list_append(block_list, block);
				block = NULL;
			} else {
				state = FIELD;
				continue;
			}
			break;
		case END:
		default:
			break;
		}

		fileptr++;
	}

	if (state != BEGIN) {
		ErrPrint("State %d\n", state);

		free(filebuf);
		free(block);

		EINA_LIST_FREE(block_list, block) {
			free(block);
		}

		PERF_MARK("parser");
		return DBOX_STATUS_ERROR_FAULT;
	}

	block = eina_list_data_get(eina_list_last(block_list));
	if (block) {
		block->filebuf = filebuf;
	} else {
		ErrPrint("Last block is not exists (There is no parsed block)\n");
		free(filebuf);
	}

	PERF_MARK("parser");

#if defined(_APPLY_SCRIPT_ASYNC_UPDATE)
	struct apply_data *data;

	data = malloc(sizeof(*data));
	if (data) {
		data->inst = inst;
		data->is_pd = is_pd;
		data->block_list = block_list;
		if (!ecore_timer_add(0.001f, apply_changes_cb, data)) {
			ErrPrint("Failed to add timer\n");
			free(data);
			EINA_LIST_FREE(block_list, block) {
				consuming_parsed_block(inst, is_pd, block);
			}
		}
	} else {
		ErrPrint("Heap: %s\n", strerror(errno));
		EINA_LIST_FREE(block_list, block) {
			consuming_parsed_block(inst, is_pd, block);
		}
	}
#else
	ErrPrint("Begin: Set content for EDJE object\n");
	EINA_LIST_FREE(block_list, block) {
		consuming_parsed_block(inst, is_pd, block);
	}
	ErrPrint("End: Set content for EDJE object\n");

	/*!
	 * Doesn't need to force to render the contents.
	 struct script_info *info;
	 info = is_pd ? instance_gbar_script(inst) : instance_dbox_script(inst);
	 if (info && info->ee) {
	 ecore_evas_manual_render(info->ee);
	 }
	 */
#endif

	return DBOX_STATUS_ERROR_NONE;
}

/* End of a file */
