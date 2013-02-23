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
#include <errno.h>
#include <stdlib.h> /* free */
#include <ctype.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>

#include <Ecore_Evas.h>
#include <Ecore.h>
#include <Evas.h>

#include <dlog.h>
#include <packet.h>

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

#define TYPE_COLOR "color"
#define TYPE_TEXT "text"
#define TYPE_IMAGE "image"
#define TYPE_EDJE "script"
#define TYPE_SIGNAL "signal"
#define TYPE_INFO "info"
#define TYPE_DRAG "drag"
#define INFO_SIZE "size"
#define INFO_CATEGORY "category"
#define ADDEND 256

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
	int (*update_script)(void *handle, Evas *e, const char *src_id, const char *target_id, const char *part, const char *path, const char *option);
	int (*update_signal)(void *handle, Evas *e, const char *id, const char *part, const char *signal);
	int (*update_drag)(void *handle, Evas *e, const char *id, const char *part, double x, double y);
	int (*update_size)(void *handle, Evas *e, const char *id, int w, int h);
	int (*update_category)(void *handle, Evas *e, const char *id, const char *category);

	void *(*create)(const char *file, const char *option);
	int (*destroy)(void *handle);

	int (*load)(void *handle, Evas *e, int w, int h);
	int (*unload)(void *handle, Evas *e);

	int (*init)(void);
	int (*fini)(void);
};

struct block {
	char *type;
	int type_len;

	char *part;
	int part_len;

	char *data;
	int data_len;

	char *file;
	int file_len;

	char *option;
	int option_len;

	char *id;
	int id_len;

	char *target_id;
	int target_len;
};

struct script_info {
	Ecore_Evas *ee;
	struct fb_info *fb;
	struct inst_info *inst;
	int loaded;

	int w;
	int h;

	double x;
	double y;
	int down;

	struct script_port *port;
	void *port_data;
};

static inline struct script_port *find_port(const char *magic_id)
{
	Eina_List *l;
	struct script_port *item;

	EINA_LIST_FOREACH(s_info.script_port_list, l, item) {
		if (!strcmp(item->magic_id(), magic_id))
			return item;
	}

	return NULL;
}

static void render_pre_cb(void *data, Evas *e, void *event_info)
{
	struct inst_info *inst = data;
	struct script_info *info;

	if (instance_state(inst) != INST_ACTIVATED) {
		DbgPrint("Render pre invoked but instance is not activated\n");
		return;
	}

	info = instance_lb_script(inst);
	if (info && script_handler_evas(info) == e) {
		return;
	}

	info = instance_pd_script(inst);
	if (info && script_handler_evas(info) == e) {
		return;
	}

	ErrPrint("Failed to do sync\n");
	return;
}

static void render_post_cb(void *data, Evas *e, void *event_info)
{
	struct inst_info *inst;
	struct script_info *info;

	inst = data;

	if (instance_state(inst) != INST_ACTIVATED) {
		DbgPrint("Render post invoked but instance is not activated\n");
		return;
	}

	info = instance_lb_script(inst);
	if (info && script_handler_evas(info) == e) {
		fb_sync(script_handler_fb(info));
		instance_lb_updated_by_instance(inst);
		return;
	}

	info = instance_pd_script(inst);
	if (info && script_handler_evas(info) == e) {
		fb_sync(script_handler_fb(info));
		instance_pd_updated_by_instance(inst, NULL);
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
	int ret;

	ee = ecore_evas_ecore_evas_get(e);
	if (!ee) {
		ErrPrint("Evas has no Ecore_Evas\n");
		return -EINVAL;
	}

	info = ecore_evas_data_get(ee, "script,info");
	if (!info) {
		ErrPrint("ecore_evas doesn't carry info data\n");
		return -EINVAL;
	}

	if (!signal || strlen(signal) == 0)
		signal = "";

	if (!part || strlen(part) == 0)
		part = "";

	ret = instance_signal_emit(info->inst, signal, part, sx, sy, ex, ey, info->x, info->y, info->down);
	return ret;
}

HAPI int script_handler_load(struct script_info *info, int is_pd)
{
	int ret;
	Evas *e;

	if (!info || !info->port) {
		ErrPrint("Script handler is not created\n");
		return -EINVAL;
	}

	if (info->loaded > 0) {
		info->loaded++;
		return 0;
	}

	ret = fb_create_buffer(info->fb);
	if (ret < 0)
		return ret;

	info->ee = fb_canvas(info->fb);
	if (!info->ee) {
		ErrPrint("Failed to get canvas\n");
		fb_destroy_buffer(info->fb);
		return -EFAULT;
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
			return -EFAULT;
		}
		info->loaded = 1;
	} else {
		ErrPrint("Evas: (nil) %dx%d\n", info->w, info->h);
	}

	ecore_evas_manual_render_set(info->ee, EINA_FALSE);
	ecore_evas_resize(info->ee, info->w, info->h);
	ecore_evas_show(info->ee);
	ecore_evas_activate(info->ee);
	fb_sync(info->fb);

	if (e)
		script_signal_emit(e, util_uri_to_path(instance_id(info->inst)),
					is_pd ? "pd,show" : "lb,show", 0.0f, 0.0f, 0.0f, 0.0f);
	return 0;
}

HAPI int script_handler_unload(struct script_info *info, int is_pd)
{
	Ecore_Evas *ee;
	Evas *e;

	if (!info || !info->port)
		return -EINVAL;

	info->loaded--;
	if (info->loaded > 0)
		return 0;

	if (info->loaded < 0) {
		info->loaded = 0;
		return 0;
	}

	e = script_handler_evas(info);
	if (e) {
		script_signal_emit(e, util_uri_to_path(instance_id(info->inst)), is_pd ? "pd,hide" : "lb,hide", 0.0f, 0.0f, 0.0f, 0.0f);
		if (info->port->unload(info->port_data, e) < 0)
			ErrPrint("Failed to unload script object. but go ahead\n");
		evas_event_callback_del(e, EVAS_CALLBACK_RENDER_POST, render_post_cb);
		evas_event_callback_del(e, EVAS_CALLBACK_RENDER_PRE, render_pre_cb);
	} else {
		ErrPrint("Evas(nil): Unload script\n");
	}

	ee = fb_canvas(info->fb);
	if (ee)
		ecore_evas_data_set(ee, "script,info", NULL);

	fb_destroy_buffer(info->fb);
	return 0;
}

HAPI struct script_info *script_handler_create(struct inst_info *inst, const char *file, const char *option, int w, int h)
{
	struct script_info *info;

	DbgPrint("Create script: %s (%s)\n", file, option);

	if (!file)
		return NULL;

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

	DbgPrint("Update info [%dx%d]\n", w, h);
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
	if (!info || !info->port) {
		ErrPrint("port is not valid\n");
		return -EINVAL;
	}

	if (info->loaded != 0) {
		ErrPrint("Script handler is not unloaded\n");
		return -EINVAL;
	}

	if (info->port->destroy(info->port_data) < 0)
		ErrPrint("Failed to destroy port, but go ahead\n");

	fb_destroy(info->fb);
	DbgFree(info);
	return 0;
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
	if (!info)
		return NULL;

	if (!info->ee)
		return NULL;

	return ecore_evas_get(info->ee);
}

static int update_script_color(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;
	Evas *e;

	if (!block || !block->part || !block->data) {
		ErrPrint("Block or part or data is not valid\n");
		return -EINVAL;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL\n");
		return -EFAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return -EINVAL;
	}

	e = script_handler_evas(info);
	if (e)
		info->port->update_color(info->port_data, e, block->id, block->part, block->data);
	else
		ErrPrint("Evas(nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);

	return 0;
}

static int update_script_text(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;
	Evas *e;

	if (!block || !block->part || !block->data) {
		ErrPrint("Block or part or data is not valid\n");
		return -EINVAL;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL\n");
		return -EFAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return -EINVAL;
	}

	e = script_handler_evas(info);
	if (e)
		info->port->update_text(info->port_data, e, block->id, block->part, block->data);
	else
		ErrPrint("Evas(nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);
	return 0;
}

static int update_script_image(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;
	Evas *e;

	if (!block || !block->part) {
		ErrPrint("Block or part is not valid\n");
		return -EINVAL;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL\n");
		return -EFAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return -EINVAL;
	}

	e = script_handler_evas(info);
	if (e)
		info->port->update_image(info->port_data, e, block->id, block->part, block->data, block->option);
	else
		ErrPrint("Evas: (nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);
	return 0;
}

static int update_script_script(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;
	Evas *e;

	if (!block || !block->part) {
		ErrPrint("Block or part is NIL\n");
		return -EINVAL;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL\n");
		return -EFAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return -EINVAL;
	}

	e = script_handler_evas(info);
	if (e)
		info->port->update_script(info->port_data, e, block->id, block->target_id, block->part, block->data, block->option);
	else
		ErrPrint("Evas: (nil) id[%s] part[%s] data[%s] option[%s]\n",
						block->id, block->part, block->data, block->option);
	return 0;
}

static int update_script_signal(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;
	Evas *e;

	if (!block) {
		ErrPrint("block is NIL\n");
		return -EINVAL;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL\n");
		return -EFAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return -EINVAL;
	}

	e = script_handler_evas(info);
	if (e)
		info->port->update_signal(info->port_data, e, block->id, block->part, block->data);
	else
		ErrPrint("Evas(nil) id[%s] part[%s] data[%s]\n", block->id, block->part, block->data);
	return 0;
}

static int update_script_drag(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;
	double dx, dy;
	Evas *e;

	if (!block || !block->data || !block->part) {
		ErrPrint("block or block->data or block->part is NIL\n");
		return -EINVAL;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL\n");
		return -EFAULT;
	}

	if (sscanf(block->data, "%lfx%lf", &dx, &dy) != 2) {
		ErrPrint("Invalid format of data (DRAG data [%s])\n", block->data);
		return -EINVAL;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return -EINVAL;
	}

	e = script_handler_evas(info);
	if (e)
		info->port->update_drag(info->port_data, e, block->id, block->part, dx, dy);
	else
		ErrPrint("Evas(nil) id[%s] part[%s] %lfx%lf\n", block->id, block->part, dx, dy);
	return 0;
}

HAPI int script_handler_resize(struct script_info *info, int w, int h)
{
	if (!info) {
	//|| (info->w == w && info->h == h)) {
		ErrPrint("info[%p] resize is not changed\n", info);
		return 0;
	}

	fb_resize(script_handler_fb(info), w, h);

	if (info->port->update_size) {
		Evas *e;
		e = script_handler_evas(info);
		if (e)
			info->port->update_size(info->port_data, e, NULL , w, h);
		else
			ErrPrint("Evas(nil) resize to %dx%d\n", w, h);
	}

	info->w = w;
	info->h = h;

	return 0;
}

static int update_info(struct inst_info *inst, struct block *block, int is_pd)
{
	struct script_info *info;

	if (!block || !block->part || !block->data) {
		ErrPrint("block or block->part or block->data is NIL\n");
		return -EINVAL;
	}

	info = is_pd ? instance_pd_script(inst) : instance_lb_script(inst);
	if (!info) {
		ErrPrint("info is NIL\n");
		return -EFAULT;
	}

	if (!info->port) {
		ErrPrint("info->port is NIL\n");
		return -EINVAL;
	}

	if (!strcasecmp(block->part, INFO_SIZE)) {
		Evas_Coord w, h;

		if (sscanf(block->data, "%dx%d", &w, &h) != 2) {
			ErrPrint("Invalid format for SIZE(%s)\n", block->data);
			return -EINVAL;
		}

		if (!block->id) {
			int resized;

			if (is_pd) {
				resized = (instance_pd_width(inst) != w) || (instance_pd_height(inst) != h);
				instance_set_pd_info(inst, w, h);
			} else {
				/*!
				 * \note
				 * LB Size is already scaled by livebox-service.
				 * Each livebox uses the LB_SIZE_TYPE_XXX for its size.
				 */
				resized = (instance_lb_width(inst) != w) || (instance_lb_height(inst) != h);
				instance_set_lb_info(inst, w, h, NO_CHANGE, CONTENT_NO_CHANGE, TITLE_NO_CHANGE);
			}

			if (resized)
				instance_send_resized_event(inst, is_pd, w, h, 0);

			script_handler_resize(info, w, h);
		} else {
			Evas *e;
			e = script_handler_evas(info);
			if (e)
				info->port->update_size(info->port_data, e, block->id, w, h);
			else
				ErrPrint("Evas(nil): id[%s] %dx%d\n", block->id, w, h);
		}
	} else if (!strcasecmp(block->part, INFO_CATEGORY)) {
		Evas *e;
		e = script_handler_evas(info);
		if (e)
			info->port->update_category(info->port_data, e, block->id, block->data);
		else
			ErrPrint("Evas(nil): id[%s] data[%s]\n", block->id, block->data);
	}

	return 0;
}

HAPI int script_handler_parse_desc(const char *pkgname, const char *id, const char *descfile, int is_pd)
{
	struct inst_info *inst;
	FILE *fp;
	int ch;
	int lineno;
	enum state {
		UNKNOWN = 0x10,
		BLOCK_OPEN = 0x11,
		FIELD = 0x12,
		VALUE = 0x13,
		BLOCK_CLOSE = 0x14,

		VALUE_TYPE = 0x00,
		VALUE_PART = 0x01,
		VALUE_DATA = 0x02,
		VALUE_FILE = 0x03,
		VALUE_OPTION = 0x04,
		VALUE_ID = 0x05,
		VALUE_TARGET = 0x06,
	};
	const char *field_name[] = {
		"type",
		"part",
		"data",
		"file",
		"option",
		"id",
		"target",
		NULL
	};
	enum state state;
	register int field_idx;
	register int idx = 0;
	register int i;
	struct block *block;
	struct {
		const char *type;
		int (*handler)(struct inst_info *inst, struct block *block, int is_pd);
	} handlers[] = {
		{
			.type = TYPE_COLOR,
			.handler = update_script_color,
		},
		{
			.type = TYPE_TEXT,
			.handler = update_script_text,
		},
		{
			.type = TYPE_IMAGE,
			.handler = update_script_image,
		},
		{
			.type = TYPE_EDJE,
			.handler = update_script_script,
		},
		{
			.type = TYPE_SIGNAL,
			.handler = update_script_signal,
		},
		{
			.type = TYPE_DRAG,
			.handler = update_script_drag,
		},
		{
			.type = TYPE_INFO,
			.handler = update_info,
		},
		{
			.type = NULL,
			.handler = NULL,
		},
	};

	block = NULL;
	inst = package_find_instance_by_id(pkgname, id);
	if (!inst) {
		ErrPrint("Instance is not exists\n");
		return -ENOENT;
	}

	fp = fopen(descfile, "rt");
	if (!fp) {
		ErrPrint("Error: %s [%s]\n", descfile, strerror(errno));
		return -EIO;
	}

	state = UNKNOWN;
	field_idx = 0;
	lineno = 1;

	block = NULL;
	while (!feof(fp)) {
		ch = getc(fp);
		if (ch == '\n')
			lineno++;

		switch (state) {
		case UNKNOWN:
			if (ch == '{') {
				state = BLOCK_OPEN;
				break;
			}

			if (!isspace(ch) && ch != EOF) {
				ErrPrint("%d: Syntax error: Desc is not started with '{' or space - (%c = 0x%x)\n", lineno, ch, ch);
				fclose(fp);
				return -EINVAL;
			}
			break;

		case BLOCK_OPEN:
			if (isblank(ch))
				break;

			if (ch != '\n') {
				ErrPrint("%d: Syntax error: New line must has to be started right after '{'\n", lineno);
				goto errout;
			}

			block = calloc(1, sizeof(*block));
			if (!block) {
				ErrPrint("Heap: %s\n", strerror(errno));
				fclose(fp);
				return -ENOMEM;
			}

			state = FIELD;
			idx = 0;
			field_idx = 0;
			break;

		case FIELD:
			if (isspace(ch))
				break;

			if (ch == '}') {
				state = BLOCK_CLOSE;
				break;
			}

			if (ch == '=') {
				if (field_name[field_idx][idx] != '\0') {
					ErrPrint("%d: Syntax error: Unrecognized field\n", lineno);
					goto errout;
				}

				switch (field_idx) {
				case 0:
					state = VALUE_TYPE;
					if (block->type) {
						DbgFree(block->type);
						block->type = NULL;
						block->type_len = 0;
					}
					idx = 0;
					break;
				case 1:
					state = VALUE_PART;
					if (block->part) {
						DbgFree(block->part);
						block->part = NULL;
						block->part_len = 0;
					}
					idx = 0;
					break;
				case 2:
					state = VALUE_DATA;
					if (block->data) {
						DbgFree(block->data);
						block->data = NULL;
						block->data_len = 0;
					}
					idx = 0;
					break;
				case 3:
					state = VALUE_FILE;
					if (block->file) {
						DbgFree(block->file);
						block->file = NULL;
						block->file_len = 0;
					}
					idx = 0;
					break;
				case 4:
					state = VALUE_OPTION;
					if (block->option) {
						DbgFree(block->option);
						block->option = NULL;
						block->option_len = 0;
					}
					idx = 0;
					break;
				case 5:
					state = VALUE_ID;
					if (block->id) {
						DbgFree(block->id);
						block->id = NULL;
						block->id_len = 0;
					}
					idx = 0;
					break;
				case 6:
					state = VALUE_TARGET;
					if (block->target_id) {
						DbgFree(block->target_id);
						block->target_id = NULL;
						block->target_len = 0;
					}
					idx = 0;
					break;
				default:
					ErrPrint("%d: Syntax error: Unrecognized field\n", lineno);
					goto errout;
				}

				break;
			}

			if (ch == '\n')
				goto errout;

			if (field_name[field_idx][idx] != ch) {
				ungetc(ch, fp);
				if (ch == '\n')
					lineno--;

				while (--idx >= 0)
					ungetc(field_name[field_idx][idx], fp);

				field_idx++;
				if (field_name[field_idx] == NULL) {
					ErrPrint("%d: Syntax error: Unrecognized field\n", lineno);
					goto errout;
				}

				idx = 0;
				break;
			}

			idx++;
			break;

		case VALUE_TYPE:
			if (idx == block->type_len) {
				char *tmp;
				block->type_len += ADDEND;
				tmp = realloc(block->type, block->type_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->type = tmp;
			}

			if (ch == '\n') {
				block->type[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->type[idx] = ch;
			idx++;
			break;

		case VALUE_PART:
			if (idx == block->part_len) {
				char *tmp;
				block->part_len += ADDEND;
				tmp = realloc(block->part, block->part_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->part = tmp;
			}

			if (ch == '\n') {
				block->part[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->part[idx] = ch;
			idx++;
			break;

		case VALUE_DATA:
			if (idx == block->data_len) {
				char *tmp;
				block->data_len += ADDEND;
				tmp = realloc(block->data, block->data_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->data = tmp;
			}

			if (ch == '\n') {
				block->data[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->data[idx] = ch;
			idx++;
			break;

		case VALUE_FILE:
			if (idx == block->file_len) {
				char *tmp;
				block->file_len += ADDEND;
				tmp = realloc(block->file, block->file_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->file = tmp;
			}

			if (ch == '\n') {
				block->file[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->file[idx] = ch;
			idx++;
			break;

		case VALUE_OPTION:
			if (idx == block->option_len) {
				char *tmp;
				block->option_len += ADDEND;
				tmp = realloc(block->option, block->option_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->option = tmp;
			}

			if (ch == '\n') {
				block->option[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->option[idx] = ch;
			idx++;
			break;
		case VALUE_ID:
			if (idx == block->id_len) {
				char *tmp;
				block->id_len += ADDEND;
				tmp = realloc(block->id, block->id_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->id = tmp;
			}

			if (ch == '\n') {
				block->id[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->id[idx] = ch;
			idx++;
			break;
		case VALUE_TARGET:
			if (idx == block->target_len) {
				char *tmp;
				block->target_len += ADDEND;
				tmp = realloc(block->target_id, block->target_len);
				if (!tmp) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
				block->target_id = tmp;
			}

			if (ch == '\n') {
				block->target_id[idx] = '\0';
				state = FIELD;
				idx = 0;
				field_idx = 0;
				break;
			}

			block->target_id[idx] = ch;
			idx++;
			break;
		case BLOCK_CLOSE:
			if (!block->file) {
				block->file = strdup(util_uri_to_path(id));
				if (!block->file) {
					ErrPrint("Heap: %s\n", strerror(errno));
					goto errout;
				}
			}

			i = 0;
			while (handlers[i].type) {
				if (!strcasecmp(handlers[i].type, block->type)) {
					handlers[i].handler(inst, block, is_pd);
					break;
				}
				i++;
			}

			if (!handlers[i].type)
				ErrPrint("%d: Unknown block type: %s\n", lineno, block->type);

			DbgFree(block->file);
			DbgFree(block->type);
			DbgFree(block->part);
			DbgFree(block->data);
			DbgFree(block->option);
			DbgFree(block->id);
			DbgFree(block->target_id);
			DbgFree(block);
			block = NULL;

			state = UNKNOWN;
			break;

		default:
			break;
		} /* switch */
	} /* while */

	if (state != UNKNOWN) {
		ErrPrint("%d: Unknown state\n", lineno);
		goto errout;
	}

	fclose(fp);
	return 0;

errout:
	ErrPrint("Parse error at %d file %s\n", lineno, util_basename(descfile));
	if (block) {
		DbgFree(block->file);
		DbgFree(block->type);
		DbgFree(block->part);
		DbgFree(block->data);
		DbgFree(block->option);
		DbgFree(block->id);
		DbgFree(block->target_id);
		DbgFree(block);
	}
	fclose(fp);
	return -EINVAL;
}

HAPI int script_init(void)
{
	struct script_port *item;
	struct dirent *ent;
	DIR *dir;
	char *path;
	int pathlen;

	if (!strcasecmp(PROVIDER_METHOD, "shm"))
		s_info.env_buf_type = BUFFER_TYPE_SHM;
	else if (!strcasecmp(PROVIDER_METHOD, "pixmap"))
		s_info.env_buf_type = BUFFER_TYPE_PIXMAP;

	dir = opendir(SCRIPT_PORT_PATH);
	if (!dir) {
		ErrPrint("Error: %s\n", strerror(errno));
		return -EIO;
	}

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.')
			continue;

		pathlen = strlen(ent->d_name) + strlen(SCRIPT_PORT_PATH) + 1;
		path = malloc(pathlen);
		if (!path) {
			ErrPrint("Heap: %s %d\n", strerror(errno), pathlen);
			closedir(dir);
			return -ENOMEM;
		}

		snprintf(path, pathlen, "%s%s", SCRIPT_PORT_PATH, ent->d_name);

		item = malloc(sizeof(*item));
		if (!item) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(path);
			closedir(dir);
			return -ENOMEM;
		}

		DbgPrint("Open SCRIPT PORT: %s\n", path);
		item->handle = dlopen(path, RTLD_GLOBAL | RTLD_NOW | RTLD_DEEPBIND);
		DbgFree(path);
		if (!item->handle) {
			ErrPrint("Error: %s\n", dlerror());
			DbgFree(item);
			closedir(dir);
			return -EFAULT;
		}

		item->magic_id = dlsym(item->handle, "script_magic_id");
		if (!item->magic_id)
			goto errout;

		DbgPrint("SCRIPT PORT magic id: %s\n", item->magic_id());

		item->update_color = dlsym(item->handle, "script_update_color");
		if (!item->update_color)
			goto errout;

		item->update_text = dlsym(item->handle, "script_update_text");
		if (!item->update_text)
			goto errout;

		item->update_image = dlsym(item->handle, "script_update_image");
		if (!item->update_image)
			goto errout;

		item->update_script = dlsym(item->handle, "script_update_script");
		if (!item->update_script)
			goto errout;

		item->update_signal = dlsym(item->handle, "script_update_signal");
		if (!item->update_signal)
			goto errout;

		item->update_drag = dlsym(item->handle, "script_update_drag");
		if (!item->update_drag)
			goto errout;

		item->update_size = dlsym(item->handle, "script_update_size");
		if (!item->update_size)
			goto errout;

		item->update_category = dlsym(item->handle, "script_update_category");
		if (!item->update_category)
			goto errout;

		item->create = dlsym(item->handle, "script_create");
		if (!item->create)
			goto errout;

		item->destroy = dlsym(item->handle, "script_destroy");
		if (!item->destroy)
			goto errout;

		item->load = dlsym(item->handle, "script_load");
		if (!item->load)
			goto errout;

		item->unload = dlsym(item->handle, "script_unload");
		if (!item->unload)
			goto errout;

		item->init = dlsym(item->handle, "script_init");
		if (!item->init)
			goto errout;

		item->fini = dlsym(item->handle, "script_fini");
		if (!item->fini)
			goto errout;

		if (item->init() < 0) {
			ErrPrint("Failed to initialize script engine\n");
			goto errout;
		}

		s_info.script_port_list = eina_list_append(s_info.script_port_list, item);
	}

	closedir(dir);
	return 0;

errout:
	ErrPrint("Error: %s\n", dlerror());
	dlclose(item->handle);
	DbgFree(item);
	closedir(dir);
	return -EFAULT;
}

HAPI int script_fini(void)
{
	struct script_port *item;
	/*!
	 * \TODO: Release all handles
	 */
	EINA_LIST_FREE(s_info.script_port_list, item) {
		item->fini();
		dlclose(item->handle);
		DbgFree(item);
	}

	return 0;
}

HAPI int script_handler_update_pointer(struct script_info *info, double x, double y, int down)
{
	if (!info)
		return 0;

	info->x = x;
	info->y = y;

	if (down == 0)
		info->down = 0;
	else if (down == 1)
		info->down = 1;

	return 0;
}

/* End of a file */
