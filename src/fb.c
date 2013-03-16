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
#include <unistd.h> /* access */
#include <errno.h>

#include <dlog.h>
#include <Ecore_Evas.h>
#include <Evas.h>

#include "util.h"
#include "conf.h"
#include "debug.h"
#include "buffer_handler.h"
#include "fb.h"

int errno;

struct fb_info {
	Ecore_Evas *ee;

	struct buffer_info *buffer;
};

HAPI int fb_init(void)
{
	return 0;
}

HAPI int fb_fini(void)
{
	return 0;
}

static void *alloc_fb(void *data, int size)
{
	struct fb_info *info = data;

	DbgPrint("FB size: %d\n", size);

	if (buffer_handler_load(info->buffer) < 0) {
		ErrPrint("Failed to load buffer handler\n");
		return NULL;
	}

	return buffer_handler_fb(info->buffer);
}

static void free_fb(void *data, void *ptr)
{
	struct fb_info *info = data;

	if (!info->buffer) {
		ErrPrint("Buffer is not valid (maybe already released)\n");
		return;
	}

	if (buffer_handler_fb(info->buffer) != ptr)
		ErrPrint("Buffer pointer is not matched\n");

	(void)buffer_handler_unload(info->buffer);
}

HAPI struct fb_info *fb_create(struct inst_info *inst, int w, int h, enum buffer_type type)
{
	struct fb_info *info;

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	info->buffer = buffer_handler_create(inst, type, w, h, sizeof(int));
	if (!info->buffer) {
		ErrPrint("Failed to create a buffer\n");
		DbgFree(info);
		return NULL;
	}

	info->ee = NULL;
	return info;
}

static void sw_render_pre_cb(void *data, Evas *e, void *event_info)
{
	struct fb_info *info = data;
	int w;
	int h;

	buffer_handler_get_size(info->buffer, &w, &h);
	evas_damage_rectangle_add(e, 0, 0, w, h);
}

static void sw_render_post_cb(void *data, Evas *e, void *event_info)
{
        void *canvas;
        Ecore_Evas *internal_ee;
	int x, y, w, h;

        internal_ee = ecore_evas_ecore_evas_get(e);
        if (!internal_ee) {
		LOGD("Failed to get ecore evas\n");
                return;
        }

        // Get a pointer of a buffer of the virtual canvas
        canvas = (void*)ecore_evas_buffer_pixels_get(internal_ee);
        if (!canvas) {
		LOGD("Failed to get pixel canvas\n");
                return;
        }

	ecore_evas_geometry_get(internal_ee, &x, &y, &w, &h);
	evas_data_argb_unpremul(canvas, w * h);
}

static void render_pre_cb(void *data, Evas *e, void *event_info)
{
	fb_pixmap_render_pre(data);
	sw_render_pre_cb(data, e, event_info);
}

static void render_post_cb(void *data, Evas *e, void *event_info)
{
	sw_render_post_cb(data, e, event_info);
	fb_pixmap_render_post(data);
}

HAPI int fb_create_buffer(struct fb_info *info)
{
	int ow;
	int oh;
	Evas *e;

	buffer_handler_get_size(info->buffer, &ow, &oh);
	DbgPrint("Buffer handler size: %dx%d\n", ow, oh);
	if (ow == 0 && oh == 0) {
		DbgPrint("ZERO Size FB accessed\n");
		return 0;
	}

	if (info->ee) {
		int w = 0;
		int h = 0;

		ecore_evas_geometry_get(info->ee, NULL, NULL, &w, &h);
		if (w != ow || h != oh) {
			ErrPrint("EE exists, size mismatched requested (%dx%d) but (%dx%d)\n", ow, oh, w, h);
			ecore_evas_resize(info->ee, ow, oh);
		}

		return 0;
	}

	info->ee = ecore_evas_buffer_allocfunc_new(ow, oh, alloc_fb, free_fb, info);
	if (!info->ee) {
		ErrPrint("Failed to create a buffer\n");
		return -EFAULT;
	}

	e = ecore_evas_get(info->ee);
	if (buffer_handler_type(info->buffer) == BUFFER_TYPE_PIXMAP) {
		evas_event_callback_add(e, EVAS_CALLBACK_RENDER_PRE, render_pre_cb, info);
		evas_event_callback_add(e, EVAS_CALLBACK_RENDER_POST, render_post_cb, info);

		/*!
		 * \note
		 * ecore_evas_alpha_set tries to access the canvas buffer.
		 * Without any render_pre/render_post callback.
		 */
		fb_pixmap_render_pre(info);
		ecore_evas_alpha_set(info->ee, EINA_TRUE);
		fb_pixmap_render_post(info);
	} else {
		evas_event_callback_add(e, EVAS_CALLBACK_RENDER_PRE, sw_render_pre_cb, info);
		evas_event_callback_add(e, EVAS_CALLBACK_RENDER_POST, sw_render_post_cb, info);
		ecore_evas_alpha_set(info->ee, EINA_TRUE);
	}

	return 0;
}

HAPI int fb_destroy_buffer(struct fb_info *info)
{
	if (!info->ee) {
		ErrPrint("EE is not exists (Maybe ZERO byte ee?)\n");
		return -EINVAL;
	}

	if (buffer_handler_type(info->buffer) == BUFFER_TYPE_PIXMAP) {
		Evas *e;
		e = ecore_evas_get(info->ee);
		if (e) {
			evas_event_callback_del(e, EVAS_CALLBACK_RENDER_POST, render_post_cb);
			evas_event_callback_del(e, EVAS_CALLBACK_RENDER_PRE, render_pre_cb);
		}
	}

	ecore_evas_free(info->ee);
	info->ee = NULL;
	return 0;
}

HAPI int fb_destroy(struct fb_info *info)
{
	fb_destroy_buffer(info);
	DbgFree(info);
	return 0;
}

HAPI Ecore_Evas * const fb_canvas(struct fb_info *info)
{
	return info->ee;
}

HAPI const char *fb_id(struct fb_info *fb)
{
	return fb ? buffer_handler_id(fb->buffer) : "";
}

HAPI int fb_resize(struct fb_info *info, int w, int h)
{
	buffer_handler_update_size(info->buffer, w, h);

	if (info->ee) {
		ecore_evas_resize(info->ee, w, h);
	} else if (!info->ee && !info->buffer) {
		/*!
		 * This object has no size at the initial time.
		 * Create a new buffer and use it
		 */
	}

	return 0;
}

HAPI int fb_get_size(struct fb_info *info, int *w, int *h)
{
	return buffer_handler_get_size(info->buffer, w, h);
}

HAPI void fb_sync(struct fb_info *info)
{
	buffer_handler_flush(info->buffer);
}

HAPI void *fb_pixmap_render_pre(struct fb_info *info)
{
	void *canvas;
	canvas = buffer_handler_pixmap_acquire_buffer(info->buffer);
	return canvas;
}

HAPI int fb_pixmap_render_post(struct fb_info *info)
{
	void *canvas;

	/*!
	 * \note
	 * info->buffer == struct buffer_info
	 */
	canvas = buffer_handler_pixmap_buffer(info->buffer);
	return buffer_handler_pixmap_release_buffer(canvas);
}

/* End of a file */
