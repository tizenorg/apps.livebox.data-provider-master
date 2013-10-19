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

#include <Ecore_Evas.h>
#include <Ecore.h>
#include <Evas.h>
#include <Eina.h>

#include <dlog.h>
#include <packet.h>
#include <livebox-errno.h>

#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "package.h"
#include "instance.h"
#include "buffer_handler.h"
#include "script_handler.h"
#include "fb.h"
#include "debug.h"
#include "conf.h"
#include "util.h"

#define INFO_SIZE "size"
#define INFO_CATEGORY "category"
#define ADDEND 256

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
	enum buffer_type env_buf_type;
} s_info = {
	.script_port_list = NULL,
	.env_buf_type = BUFFER_TYPE_FILE,
};

struct script_port {
	void *handle;

	const char *(*magic_id)(void);
	int (*update_color)(void *handle, Evas *e, const char *id, const char *part, const char *rgba);
	int (*update_text)(void *handle, Evas *e, const char *id, const char *part, const char *text);
	int (*update_image)(void *handle, Evas *e, const char *id, const char *part, const char *path, const char *option);
	int (*update_access)(void *handle, Evas *e, const char *id, const char *part, const char *text, const char *option);
	int (*operate_access)(void *handle, Evas *e, const char *id, const char *part, const char *operation, const char *option);
	int (*update_script)(void *handle, Evas *e, const char *src_id, const char *target_id, const char *part, const char *path, const char *option);
	int (*update_signal)(void *handle, Evas *e, const char *id, const char *part, const char *signal);
	int (*update_drag)(void *handle, Evas *e, const char *id, const char *part, double x, double y);
	int (*update_size)(void *handle, Evas *e, const char *id, int w, int h);
	int (*update_category)(void *handle, Evas *e, const char *id, const char *category);
	int (*feed_event)(void *handle, Evas *e, int event_type, int x, int y, int down, double timestamp);

	void *(*create)(const char *file, const char *option);
	int (*destroy)(void *handle);

	int (*load)(void *handle, Evas *e, int w, int h);
	int (*unload)(void *handle, Evas *e);

	int (*init)(double scale);
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
	Ecore_Evas *ee;
	struct fb_info *fb;
	struct inst_info *inst;
	int loaded;

	int w;
	int h;

	int x;
	int y;
	int down;

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

	dir = opendir(SCRIPT_PORT_PATH);
	if (!dir) {
		ErrPrint("Error: %s\n", strerror(errno));
		return LB_STATUS_ERROR_IO;
	}

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.') {
			continue;
		}

		pathlen = strlen(ent->d_name) + strlen(SCRIPT_PORT_PATH) + 1;
		path = malloc(pathlen);
		if (!path) {
			ErrPrint("Heap: %s %d\n", strerror(errno), pathlen);
			if (closedir(dir) < 0) {
				ErrPrint("closedir: %s\n", strerror(errno));
			}
			return LB_STATUS_ERROR_MEMORY;
		}

		snprintf(path, pathlen, "%s%s", SCRIPT_PORT_PATH, ent->d_name);

		item = malloc(sizeof(*item));
		if (!item) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(path);
			if (closedir(dir) < 0) {
				ErrPrint("closedir: %s\n", strerror(errno));
			}
			return LB_STATUS_ERROR_MEMORY;
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
			return LB_STATUS_ERROR_FAULT;
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

		if (item->init(SCALE_WIDTH_FACTOR) < 0) {
			ErrPrint("Failed to initialize script engine\n");
			goto errout;
		}

		s_info.script_port_list = eina_list_append(s_info.script_port_list, item);
	}

	if (closedir(dir) < 0) {
		ErrPrint("closedir: %s\n", strerror(errno));
	}

	return LB_STATUS_SUCCESS;

errout:
	ErrPrint("Error: %s\n", dlerror());
	if (dlclose(item->handle) != 0) {
		ErrPrint("dlclose: %s\n", strerror(errno));
	}
	DbgFree(item);
	if (closedir(dir) < 0) {
		ErrPrint("closedir: %s\n", strerror(errno));
	}
	return LB_STATUS_ERROR_FAULT;
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

static void render_pre_cb(void *data, Evas *e, void *event_info)
{
	PERF_INIT();
	PERF_BEGIN();
	struct inst_info *inst = data;
	struct script_info *info;

	if (instance_state(inst) != INST_ACTIVATED) {
		ErrPrint("Render pre invoked but instance is not activated\n");
		goto out;
	}

	info = instance_lb_script(inst);
	if (info && script_handler_evas(info) == e) {
		goto out;
	}

	info = instance_pd_script(inst);
	if (info && script_handler_evas(info) == e) {
		goto out;
	}

	ErrPrint("Failed to do sync\n");
out:
	PERF_MARK("render,pre");
	return;
}

static void render_post_cb(void *data, Evas *e, void *event_info)
{
	PERF_INIT();
	PERF_BEGIN();
	struct inst_info *inst;
	struct script_info *info;

	inst = data;

	if (instance_state(inst) != INST_ACTIVATED) {
		ErrPrint("Render post invoked but instance is not activated\n");
		return;
	}

	info = instance_lb_script(inst);
	if (info && script_handler_evas(info) == e) {
		fb_sync(script_handler_fb(info));
		instance_lb_updated_by_instance(inst, NULL);

		PERF_MARK("lb,update");
		return;
	}

	info = instance_pd_script(inst);
	if (info && script_handler_evas(info) == e) {
		fb_sync(script_handler_fb(info));
		instance_pd_updated_by_instance(inst, NULL);
		PERF_MARK("pd,update");
		return;
	}

	ErrPrint("Failed to sync\n");
	return;
}

/*!
 * \NOTE
 * Exported API
 */
int script_signal_emit(Evas *e, const char *part, const char *signal, double sx, double sy, double ex, double ey)
{
	Ecore_Evas *ee;
	struct script_info *info;

	ee = ecore_evas_ecore_evas_get(e);
	if (!ee) {
		ErrPrint("Evas has no Ecore_Evas\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = ecore_evas_data_get(ee, "script,info");
	if (!info) {
		ErrPrint("ecore_evas doesn't carry info data\n");
		return LB_STATUS_ERROR_INVALID;
	}

	if (!signal || strlen(signal) == 0) {
		signal = "";
	}

	if (!part || strlen(part) == 0) {
		part = "";
	}

	return instance_signal_emit(info->inst, signal, part, sx, sy, ex, ey, (double)info->x / (double)info->w, (double)info->y / (double)info->h, info->down);
}

static inline void flushing_cached_block(struct script_info *info)
{
	struct block *block;

	EINA_LIST_FREE(info->cached_blocks, block) {
		consuming_parsed_block(info->inst, (instance_pd_script(info->inst) == info), block);
	}
}

HAPI int script_handler_load(struct script_info *info, int is_pd)
{
	int ret;
	Evas *e;

	if (!info || !info->port) {
		ErrPrint("Script handler is not created\n");
		return LB_STATUS_ERROR_INVALID;
	}

	if (info->loaded > 0) {
		info->loaded++;
		return LB_STATUS_SUCCESS;
	}

	ret = fb_create_buffer(info->fb);
	if (ret < 0) {
		return ret;
	}

	info->ee = fb_canvas(info->fb);
	if (!info->ee) {
		ErrPrint("Failed to get canvas\n");
		fb_destroy_buffer(info->fb);
		return LB_STATUS_ERROR_FAULT;
	}

	ecore_evas_data_set(info->ee, "script,info", info);

	e = script_handler_evas(info);
	if (e) {
		evas_event_callback_add(e, EVAS_CALLBACK_RENDER_PRE, render_pre_cb, info->inst);
		evas_event_callback_add(e, EVAS_CALLBACK_RENDER_POST, render_post_cb, info->inst);
		if (info->port->load(info->port_data, e, info->w, info->h) < 0) {
			ErrPrint("Failed to add new script object\n");
			evas_event_callback_del(e, EVAS_CALLBACK_RENDER_POST, render_post_cb);
			evas_event_callback_del(e, EVAS_CALLBACK_RENDER_PRE, render_pre_cb);
			fb_destroy_buffer(info->fb);
			return LB_STATUS_ERROR_FAULT;
		}
		info->loaded = 1;
		flushing_cached_block(info);
		script_signal_emit(e, instance_id(info->inst),
				is_pd ? "pd,show" : "lb,show", 0.0f, 0.0f, 0.0f, 0.0f);
	} else {
		ErrPrint("Evas: (nil) %dx%d\n", info->w, info->h);
	}

	ecore_evas_manual_render_set(info->ee, EINA_FALSE);
	ecore_evas_resize(info->ee, info->w, info->h);
	ecore_evas_show(info->ee);
	ecore_evas_activate(info->ee);
	fb_sync(info->fb);

	return LB_STATUS_SUCCESS;
}

HAPI int script_handler_unload(struct script_info *info, int is_pd)
{
	Ecore_Evas *ee;
	Evas *e;

	if (!info || !info->port) {
		return LB_STATUS_ERROR_INVALID;
	}

	info->loaded--;
	if (info->loaded > 0) {
		return LB_STATUS_SUCCESS;
	}

	if (info->loaded < 0) {
		info->loaded = 0;
		return LB_STATUS_SUCCESS;
	}

	e = script_handler_evas(info);
	if (e) {
		script_signal_emit(e, instance_id(info->inst),
				is_pd ? "pd,hide" : "lb,hide", 0.0f, 0.0f, 0.0f, 0.0f);
		if (info->port->unload(info->port_data, e) < 0) {
			ErrPrint("Failed to unload script object. but go ahead\n");
		}
		evas_event_callback_del(e, EVAS_CALLBACK_RENDER_POST, render_post_cb);
		evas_event_callback_del(e, EVAS_CALLBACK_RENDER_PRE, render_pre_cb);
	} else {
		ErrPrint("Evas(nil): Unload script\n");
	}

	ee = fb_canvas(info->fb);
	if (ee) {
		ecore_evas_data_set(ee, "script,info", NULL);
	}

	fb_destroy_buffer(info->fb);
	return LB_STATUS_SUCCESS;
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

	info->fb = fb_create(inst, w, h, s_info.env_buf_type);
	if (!info->fb) {
		ErrPrint("Failed to create a FB (%dx%d)\n", w, h);
		DbgFree(info);
		return NULL;
	}

	info->inst = inst;
	info->port = find_port(package_script(instance_package(inst)));
	if (!info->port) {
		ErrPrint("Failed to find a proper port for [%s]%s\n",
					instance_package(inst), package_script(instance_package(inst)));
		fb_destroy(info->fb);
		DbgFree(info);
		return NULL;
	}

	info->w = w;
	info->h = h;

	info->port_data = info->port->create(file, option);
	if (!info->port_data) {
		ErrPrint("Failed to create a port (%s - %s)\n", file, option);
		fb_destroy(info->fb);
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
		return LB_STATUS_ERROR_INVALID;
	}

	if (info->loaded != 0) {
		ErrPrint("Script handler is not unloaded\n");
		return LB_STATUS_ERROR_INVALID;
	}

	ret = info->port->destroy(info->port_data);
	if (ret < 0) {
		ErrPrint("Failed to destroy port, but go ahead: %d\n", ret);
	}

	fb_destroy(info->fb);

	EINA_LIST_FREE(info->cached_blocks, block) {
		delete_block(block);
	}
	DbgFree(info);
	return LB_STATUS_SUCCESS;
}

HAPI int script_handler_is_loaded(struct script_info *info)
{
	return info ? info->loaded > 0 : 0;
}

HAPI struct fb_info *script_handler_fb(struct script_info *info)
{
	return info ? info->fb : NULL;
}

HAPI void *script_handler_evas(struct script_info *info)
{
	if (!info) {
		return NULL;
	}

	if (!info->ee) {
		return NULL;
	}

	return ecore_evas_get(info->ee);
}

static int update_script_color(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	Evas *e;

	if (!block || !block->part || !block->data) {
		ErrPrint("Block or part or data is not valid\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_INVALID;
	}

	e = script_handler_evas(info);
	if (e) {
		info->port->update_color(info->port_data, e, block->id, block->part, block->data);
	} else {
		ErrPrint("Evas(nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);
	}
	PERF_MARK("color");

	return LB_STATUS_SUCCESS;
}

static int update_script_text(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	Evas *e;

	if (!block || !block->part || !block->data) {
		ErrPrint("Block or part or data is not valid\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	e = script_handler_evas(info);
	if (e) {
		info->port->update_text(info->port_data, e, block->id, block->part, block->data);
	} else {
		ErrPrint("Evas(nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);
	}

	PERF_MARK("text");
	return LB_STATUS_SUCCESS;
}

static int update_script_image(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	Evas *e;

	if (!block || !block->part) {
		ErrPrint("Block or part is not valid\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	e = script_handler_evas(info);
	if (e) {
		info->port->update_image(info->port_data, e, block->id, block->part, block->data, block->option);
	} else {
		ErrPrint("Evas: (nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);
	}
	PERF_MARK("image");
	return LB_STATUS_SUCCESS;
}

static int update_access(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	Evas *e;

	if (!block || !block->part) {
		ErrPrint("Block or block->part is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	e = script_handler_evas(info);
	if (e) {
		info->port->update_access(info->port_data, e, block->id, block->part, block->data, block->option);
	} else {
		ErrPrint("Evas: (nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);
	}
	PERF_MARK("access");
	return LB_STATUS_SUCCESS;
}

static int operate_access(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	Evas *e;

	if (!block || !block->part) {
		ErrPrint("Block or block->part is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	e = script_handler_evas(info);
	if (e) {
		info->port->operate_access(info->port_data, e, block->id, block->part, block->data, block->option);
	} else {
		ErrPrint("Evas: (nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);
	}
	PERF_MARK("operate_access");
	return LB_STATUS_SUCCESS;
}

static int update_script_script(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	Evas *e;

	if (!block || !block->part) {
		ErrPrint("Block or part is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	e = script_handler_evas(info);
	if (e) {
		info->port->update_script(info->port_data, e, block->id, block->target, block->part, block->data, block->option);
	} else {
		ErrPrint("Evas: (nil) id[%s] part[%s] data[%s] option[%s]\n",
						block->id, block->part, block->data, block->option);
	}
	PERF_MARK("script");
	return LB_STATUS_SUCCESS;
}

static int update_script_signal(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	Evas *e;

	if (!block) {
		ErrPrint("block is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	e = script_handler_evas(info);
	if (e) {
		info->port->update_signal(info->port_data, e, block->id, block->part, block->data);
	} else {
		ErrPrint("Evas(nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);
	}
	PERF_MARK("signal");
	return LB_STATUS_SUCCESS;
}

static int update_script_drag(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;
	double dx, dy;
	Evas *e;

	if (!block || !block->data || !block->part) {
		ErrPrint("block or block->data or block->part is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_FAULT;
	}

	if (sscanf(block->data, "%lfx%lf", &dx, &dy) != 2) {
		ErrPrint("Invalid format of data (DRAG data [%s])\n", block->data);
		return LB_STATUS_ERROR_INVALID;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	e = script_handler_evas(info);
	if (e) {
		info->port->update_drag(info->port_data, e, block->id, block->part, dx, dy);
	} else {
		ErrPrint("Evas(nil) id[%s] part[%s] %lfx%lf\n", block->id, block->part, dx, dy);
	}
	PERF_MARK("drag");
	return LB_STATUS_SUCCESS;
}

HAPI int script_handler_resize(struct script_info *info, int w, int h)
{
	PERF_INIT();
	PERF_BEGIN();
	if (!info) {
	//|| (info->w == w && info->h == h)) {
		ErrPrint("info[%p] resize is ignored\n", info);
		return LB_STATUS_SUCCESS;
	}

	fb_resize(script_handler_fb(info), w, h);

	if (info->port->update_size) {
		Evas *e;
		e = script_handler_evas(info);
		if (e) {
			info->port->update_size(info->port_data, e, NULL , w, h);
		} else {
			ErrPrint("Evas(nil) resize to %dx%d\n", w, h);
		}
	}

	if (instance_lb_script(info->inst) == info) {
		instance_set_lb_size(info->inst, w, h);
	} else if (instance_pd_script(info->inst) == info) {
		instance_set_pd_size(info->inst, w, h);
	} else {
		ErrPrint("Script is not known\n");
	}

	info->w = w;
	info->h = h;
	PERF_MARK("resize");

	return LB_STATUS_SUCCESS;
}

static int update_info(struct inst_info *inst, struct block *block, int is_pd)
{
	PERF_INIT();
	PERF_BEGIN();
	struct script_info *info;

	if (!block || !block->part || !block->data) {
		ErrPrint("block or block->part or block->data is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		return LB_STATUS_ERROR_FAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	if (!strcasecmp(block->part, INFO_SIZE)) {
		Evas_Coord w, h;

		if (sscanf(block->data, "%dx%d", &w, &h) != 2) {
			ErrPrint("Invalid format for SIZE(%s)\n", block->data);
			return LB_STATUS_ERROR_INVALID;
		}

		if (!block->id) {
			script_handler_resize(info, w, h);
		} else {
			Evas *e;
			e = script_handler_evas(info);
			if (e) {
				info->port->update_size(info->port_data, e, block->id, w, h);
			} else {
				ErrPrint("Evas(nil): id[%s] %dx%d\n", block->id, w, h);
			}
		}
	} else if (!strcasecmp(block->part, INFO_CATEGORY)) {
		Evas *e;
		e = script_handler_evas(info);
		if (e) {
			info->port->update_category(info->port_data, e, block->id, block->data);
		} else {
			ErrPrint("Evas(nil): id[%s] data[%s]\n", block->id, block->data);
		}
	}
	PERF_MARK("info");

	return LB_STATUS_SUCCESS;
}

static inline void consuming_parsed_block(struct inst_info *inst, int is_pd, struct block *block)
{
	//PERF_INIT();
	//PERF_BEGIN();
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

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL (%d, %s)\n", is_pd, instance_id(inst));
		goto free_out;
	}

	if (script_handler_is_loaded(info)) {
		if (block->type >= 0 || block->type < TYPE_MAX) {
			(void)updators[block->type](inst, block, is_pd);
		}
	} else {
		info->cached_blocks = eina_list_append(info->cached_blocks, block);
		DbgPrint("Block is cached (%p), %d, %s\n", block, eina_list_count(info->cached_blocks), instance_id(inst));
		return;
	}

free_out:
	//PERF_MARK(type_list[block->type]);
	delete_block(block);
}

HAPI int script_init(void)
{
	if (!strcasecmp(PROVIDER_METHOD, "shm")) {
		s_info.env_buf_type = BUFFER_TYPE_SHM;
	} else if (!strcasecmp(PROVIDER_METHOD, "pixmap")) {
		s_info.env_buf_type = BUFFER_TYPE_PIXMAP;
	}

	return LB_STATUS_SUCCESS;
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
		return LB_STATUS_SUCCESS;
	}

	info->x = x;
	info->y = y;

	if (down == 0) {
		info->down = 0;
	} else if (down == 1) {
		info->down = 1;
	}

	return LB_STATUS_SUCCESS;
}

HAPI int script_handler_feed_event(struct script_info *info, int event, double timestamp)
{
	Evas *e;

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return LB_STATUS_ERROR_INVALID;
	}

	e = script_handler_evas(info);
	if (!e) {
		ErrPrint("Evas is not exists\n");
		return LB_STATUS_ERROR_FAULT;
	}

	return info->port->feed_event(info->port_data, e, event, info->x, info->y, info->down, timestamp);
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

	filebuf = malloc(filesize);
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
		return LB_STATUS_ERROR_IO;
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

		return LB_STATUS_ERROR_FAULT;
	}

	if (block) {
		block->filebuf = filebuf;
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
	EINA_LIST_FREE(block_list, block) {
		consuming_parsed_block(inst, is_pd, block);
	}

	/*!
	 * Doesn't need to force to render the contents.
	struct script_info *info;
	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (info && info->ee) {
		ecore_evas_manual_render(info->ee);
	}
	*/
#endif

	return LB_STATUS_SUCCESS;
}

/* End of a file */
