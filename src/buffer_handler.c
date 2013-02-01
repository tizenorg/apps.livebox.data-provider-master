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
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <string.h>
#include <stdlib.h>

#include <Ecore.h>
#include <Ecore_X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XShm.h>

#include <dri2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_slp_bufmgr.h>

#include <dlog.h>
#include <packet.h>

#include "debug.h"
#include "conf.h"
#include "util.h"
#include "instance.h"
#include "package.h"
#include "client_life.h"
#include "client_rpc.h"
#include "buffer_handler.h"

struct buffer {
	enum {
		CREATED = 0x00beef00,
		DESTROYED = 0x00dead00,
	} state;
	enum buffer_type type;
	int refcnt;
	void *info;
	char data[];
};

/*!
 * \brief Allocate this in the buffer->data.
 */
struct gem_data {
	DRI2Buffer *dri2_buffer;
	unsigned int attachments[1];
	drm_slp_bo pixmap_bo;
	int count;
	int buf_count;
	int w;
	int h;
	int depth;
	Pixmap pixmap;
	void *data; /* Gem layer */
	int refcnt;

	void *compensate_data; /* Check the pitch value, copy this to data */
};

struct buffer_info
{
	void *buffer;
	char *id;

	enum buffer_type type;

	int w;
	int h;
	int pixel_size;
	int is_loaded;

	struct inst_info *inst;
};

static struct {
	drm_slp_bufmgr slp_bufmgr;
	int evt_base;
	int err_base;
	int fd;
	Eina_List *pixmap_list;
} s_info = {
	.slp_bufmgr = NULL,
	.evt_base = 0,
	.err_base = 0,
	.fd = -1,
	.pixmap_list = NULL,
};

HAPI struct buffer_info *buffer_handler_create(struct inst_info *inst, enum buffer_type type, int w, int h, int pixel_size)
{
	struct buffer_info *info;

	info = malloc(sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	switch (type) {
	case BUFFER_TYPE_SHM:
		info->id = strdup(SCHEMA_SHM "-1");
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(info);
			return NULL;
		}
		break;
	case BUFFER_TYPE_FILE:
		info->id = strdup(SCHEMA_FILE "/tmp/.live.undefined");
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(info);
			return NULL;
		}
		break;
	case BUFFER_TYPE_PIXMAP:
		info->id = strdup(SCHEMA_PIXMAP "0");
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(info);
			return NULL;
		}
		break;
	default:
		ErrPrint("Invalid type\n");
		DbgFree(info);
		return NULL;
	}

	info->w = w;
	info->h = h;
	info->pixel_size = pixel_size;
	info->type = type;
	info->is_loaded = 0;
	info->inst = inst;
	info->buffer = NULL;

	DbgPrint("%dx%d size buffer is created\n", w, h);
	return info;
}

static inline struct buffer *create_gem(Display *disp, Window parent, struct buffer_info *info)
{
	struct gem_data *gem;
	struct buffer *buffer;


	buffer = calloc(1, sizeof(*buffer) + sizeof(*gem));
	if (!buffer) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	gem = (struct gem_data *)buffer->data;

	buffer->type = BUFFER_TYPE_PIXMAP;
	buffer->refcnt = 1;
	buffer->state = CREATED;

	DbgPrint("Canvas %dx%d - %d is created\n", info->w, info->h, info->pixel_size);

	gem->attachments[0] = DRI2BufferFrontLeft;
	gem->count = 1;
	gem->w = info->w; /*!< This can be changed by DRI2GetBuffers */
	gem->h = info->h; /*!< This can be changed by DRI2GetBuffers */
	gem->depth = info->pixel_size;
	/*!
	 * \NOTE
	 * Use the 24 Bits
	 * 32 Bits is not supported for video playing.
	 */
	gem->pixmap = XCreatePixmap(disp, parent, info->w, info->h, 24 /* (info->pixel_size << 3) */);
	if (gem->pixmap == (Pixmap)0) {
		ErrPrint("Failed to create a pixmap\n");
		DbgFree(buffer);
		return NULL;
	}

	XSync(disp, False);

	DbgPrint("Pixmap:0x%X is created\n", gem->pixmap);

	if (info->inst) {
		struct pkg_info *pkg;

		if (instance_lb_buffer(info->inst) == info) {
			pkg = instance_package(info->inst);
			if (package_lb_type(pkg) == LB_TYPE_BUFFER) {
				DbgPrint("Doesn't need to create gem for LB\n");
				return buffer;
			}
		} else if (instance_pd_buffer(info->inst) == info) {
			pkg = instance_package(info->inst);
			if (package_pd_type(pkg) == PD_TYPE_BUFFER) {
				DbgPrint("Doesn't need to create gem for PD\n");
				return buffer;
			}
		}
	}

	if (s_info.fd < 0) {
		gem->data = calloc(1, gem->w * gem->h * gem->depth);
		if (!gem->data) {
			ErrPrint("Heap: %s\n", strerror(errno));
			XFreePixmap(disp, gem->pixmap);
			buffer->state = DESTROYED;
			DbgFree(buffer);
			return NULL;
		}

		DbgPrint("DRI2(gem) is not supported - Fallback to the S/W Backend\n");
		return buffer;
	}

	DRI2CreateDrawable(disp, gem->pixmap);
	DbgPrint("DRI2CreateDrawable is done\n");

	gem->dri2_buffer = DRI2GetBuffers(disp, gem->pixmap,
					&gem->w, &gem->h, gem->attachments, gem->count, &gem->buf_count);
	if (!gem->dri2_buffer || !gem->dri2_buffer->name) {
		ErrPrint("Failed to get GemBuffer\n");
		DRI2DestroyDrawable(disp, gem->pixmap);
		XFreePixmap(disp, gem->pixmap);
		buffer->state = DESTROYED;
		DbgFree(buffer);
		return NULL;
	}
	DbgPrint("dri2_buffer: %p, name: %p, %dx%d (%dx%d)\n",
				gem->dri2_buffer, gem->dri2_buffer->name, gem->w, gem->h, info->w, info->h);
	DbgPrint("dri2_buffer->pitch : %d, buf_count: %d\n",
				gem->dri2_buffer->pitch, gem->buf_count);

	/*!
	 * \How can I destroy this?
	 */
	gem->pixmap_bo = drm_slp_bo_import(s_info.slp_bufmgr, gem->dri2_buffer->name);
	if (!gem->pixmap_bo) {
		DRI2DestroyDrawable(disp, gem->pixmap);
		XFreePixmap(disp, gem->pixmap);
		ErrPrint("Failed to import BO\n");
		buffer->state = DESTROYED;
		DbgFree(buffer);
		return NULL;
	}

	if (gem->dri2_buffer->pitch != gem->w * gem->depth) {
		gem->compensate_data = calloc(1, gem->w * gem->h * gem->depth);
		if (!gem->compensate_data) {
			ErrPrint("Failed to allocate heap\n");
		} else {
			DbgPrint("Allocate compensate buffer %p(%dx%d %d)\n",
								gem->compensate_data,
								gem->w, gem->h, gem->depth);
		}
	}

	DbgPrint("Return buffer: %p\n", buffer);
	return buffer;
}

static inline void *acquire_gem(struct buffer *buffer)
{
	struct gem_data *gem;

	if (!buffer)
		return NULL;

	gem = (struct gem_data *)buffer->data;
	if (s_info.fd < 0) {
		DbgPrint("GEM is not supported - Use the fake gem buffer\n");
	} else {
		if (!gem->pixmap_bo) {
			DbgPrint("GEM is not created\n");
			return NULL;
		}

		if (!gem->data) {
			if (gem->refcnt) {
				ErrPrint("Already acquired. but the buffer is not valid\n");
				return NULL;
			}

			gem->data = (void *)drm_slp_bo_map(gem->pixmap_bo, DRM_SLP_DEVICE_CPU, DRM_SLP_OPTION_READ|DRM_SLP_OPTION_WRITE);
		}
	}

	gem->refcnt++;

	/*!
	 * \note
	 * If there is a compensate canvas buffer,
	 * use it
	 */
	return gem->compensate_data ? gem->compensate_data : gem->data;
}

static inline void release_gem(struct buffer *buffer)
{
	struct gem_data *gem;

	gem = (struct gem_data *)buffer->data;
	if (s_info.fd >= 0 && !gem->pixmap_bo) {
		DbgPrint("GEM is not created\n");
		return;
	}

	if (!gem->data) {
		if (gem->refcnt > 0) {
			ErrPrint("Reference count is not valid %d\n", gem->refcnt);
			gem->refcnt = 0;
		}
		return;
	}

	gem->refcnt--;
	if (gem->refcnt == 0) {
		if (s_info.fd < 0) {
			DbgPrint("S/W Gem buffer has no reference\n");
		} else {
			/*!
			 * \note
			 * Update the gem buffer using compensate data buffer if it is exists
			 */
			if (gem->compensate_data) {
				register int x;
				register int y;
				int *gem_pixel;
				int *pixel;
				int gap;

				pixel = gem->compensate_data;
				gem_pixel = gem->data;
				gap = gem->dri2_buffer->pitch - (gem->w * gem->depth);

				for (y = 0; y < gem->h; y++) {
					for (x = 0; x < gem->w; x++)
						*gem_pixel++ = *pixel++;

					gem_pixel = (int *)(((char *)gem_pixel) + gap);
				}
			}

			if (gem->pixmap_bo)
				drm_slp_bo_unmap(gem->pixmap_bo, DRM_SLP_DEVICE_CPU);

			gem->data = NULL;
		}
	} else if (gem->refcnt < 0) {
		DbgPrint("Invalid refcnt: %d (reset)\n", gem->refcnt);
		gem->refcnt = 0;
	}
}

static inline int destroy_gem(struct buffer *buffer)
{
	struct gem_data *gem;

	if (!buffer)
		return -EINVAL;

	/*!
	 * Forcely release the acquire_buffer.
	 */
	gem = (struct gem_data *)buffer->data;
	if (!gem)
		return -EFAULT;

	if (s_info.fd >= 0) {
		if (gem->compensate_data) {
			DbgPrint("Release compensate buffer %p\n", gem->compensate_data);
			free(gem->compensate_data);
			gem->compensate_data = NULL;
		}

		if (gem->pixmap_bo) {
			DbgPrint("unref pixmap bo\n");
			drm_slp_bo_unref(gem->pixmap_bo);
			gem->pixmap_bo = NULL;

			DbgPrint("DRI2DestroyDrawable\n");
			DRI2DestroyDrawable(ecore_x_display_get(), gem->pixmap);
		}
	} else if (gem->data) {
		DbgPrint("Release fake gem buffer\n");
		DbgFree(gem->data);
		gem->data = NULL;
	}

	DbgPrint("Free pixmap 0x%X\n", gem->pixmap);
	XFreePixmap(ecore_x_display_get(), gem->pixmap);

	buffer->state = DESTROYED;
	DbgFree(buffer);
	return 0;
}

static inline int load_file_buffer(struct buffer_info *info)
{
	struct buffer *buffer;
	double timestamp;
	int size;
	char *new_id;
	int len;

	len = strlen(IMAGE_PATH) + 40;
	new_id = malloc(len);
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	timestamp = util_timestamp();
	snprintf(new_id, len, SCHEMA_FILE "%s%lf", IMAGE_PATH, timestamp);

	size = sizeof(*buffer) + info->w * info->h * info->pixel_size;
	if (!size) {
		ErrPrint("Canvas buffer size is ZERO\n");
		DbgFree(new_id);
		return -EINVAL;
	}

	buffer = calloc(1, size);
	if (!buffer) {
		ErrPrint("Failed to allocate buffer\n");
		DbgFree(new_id);
		return -ENOMEM;
	}

	buffer->type = BUFFER_TYPE_FILE;
	buffer->refcnt = 0;
	buffer->state = CREATED;
	buffer->info = info;

	DbgFree(info->id);
	info->id = new_id;
	info->buffer = buffer;
	info->is_loaded = 1;

	DbgPrint("FILE type %d created\n", size);
	return 0;
}

static inline int load_shm_buffer(struct buffer_info *info)
{
	int id;
	int size;
	struct buffer *buffer; /* Just for getting a size */
	char *new_id;
	int len;

	size = info->w * info->h * info->pixel_size;
	if (!size) {
		ErrPrint("Invalid buffer size\n");
		return -EINVAL;
	}

	id = shmget(IPC_PRIVATE, size + sizeof(*buffer), IPC_CREAT | 0666);
	if (id < 0) {
		ErrPrint("shmget: %s\n", strerror(errno));
		return -EFAULT;
	}

	buffer = shmat(id, NULL, 0);
	if (buffer == (void *)-1) {
		ErrPrint("%s shmat: %s\n", info->id, strerror(errno));

		if (shmctl(id, IPC_RMID, 0) < 0)
			ErrPrint("%s shmctl: %s\n", info->id, strerror(errno));

		return -EFAULT;
	}

	buffer->type = BUFFER_TYPE_SHM;
	buffer->refcnt = id;
	buffer->state = CREATED; /*!< Needless */
	buffer->info = NULL; /*!< This has not to be used, every process will see this. So, don't try to save anything on here */

	len = strlen(SCHEMA_SHM) + 30; /* strlen("shm://") + 30 */

	new_id = malloc(len);
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		if (shmdt(buffer) < 0)
			ErrPrint("shmdt: %s\n", strerror(errno));

		if (shmctl(id, IPC_RMID, 0) < 0)
			ErrPrint("shmctl: %s\n", strerror(errno));

		return -ENOMEM;
	}

	snprintf(new_id, len, SCHEMA_SHM "%d", id);

	DbgFree(info->id);
	info->id = new_id;
	info->buffer = buffer;
	info->is_loaded = 1;
	return 0;
}

static inline int load_pixmap_buffer(struct buffer_info *info)
{
	struct buffer *buffer;
	struct gem_data *gem;
	char *new_id;
	int len;

	/*!
	 * \NOTE
	 * Before call the buffer_handler_pixmap_ref function,
	 * You should make sure that the is_loaded value is toggled (1)
	 * Or the buffer_handler_pixmap_ref function will return NULL
	 */
	info->is_loaded = 1;

	if (info->buffer)
		DbgPrint("Buffer is already exists, but override it with new one\n");

	buffer = buffer_handler_pixmap_ref(info);
	if (!buffer) {
		DbgPrint("Failed to make a reference of a pixmap\n");
		info->is_loaded = 0;
		return -EFAULT;
	}

	len = strlen(SCHEMA_PIXMAP) + 30; /* strlen("pixmap://") + 30 */
	new_id = malloc(len);
	if (!new_id) {
		info->is_loaded = 0;
		ErrPrint("Heap: %s\n", strerror(errno));
		buffer_handler_pixmap_unref(buffer);
		return -ENOMEM;
	}

	DbgPrint("Releaseo old id (%s)\n", info->id);
	DbgFree(info->id);
	info->id = new_id;

	gem = (struct gem_data *)buffer->data;
	DbgPrint("gem pointer: %p\n", gem);

	snprintf(info->id, len, SCHEMA_PIXMAP "%d", (int)gem->pixmap);
	DbgPrint("info->id: %s\n", info->id);

	return 0;
}

HAPI int buffer_handler_load(struct buffer_info *info)
{
	int ret;

	if (!info) {
		DbgPrint("buffer handler is nil\n");
		return -EINVAL;
	}

	if (info->is_loaded) {
		DbgPrint("Buffer is already loaded\n");
		return 0;
	}

	switch (info->type) {
	case BUFFER_TYPE_FILE:
		ret = load_file_buffer(info);
		break;
	case BUFFER_TYPE_SHM:
		ret = load_shm_buffer(info);
		break;
	case BUFFER_TYPE_PIXMAP:
		ret = load_pixmap_buffer(info);
		break;
	default:
		ErrPrint("Invalid buffer\n");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static inline int unload_file_buffer(struct buffer_info *info)
{
	const char *path;
	char *new_id;

	new_id = strdup(SCHEMA_FILE "/tmp/.live.undefined");
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	DbgFree(info->buffer);
	info->buffer = NULL;

	path = util_uri_to_path(info->id);
	if (path && unlink(path) < 0)
		ErrPrint("unlink: %s\n", strerror(errno));

	DbgFree(info->id);
	info->id = new_id;
	return 0;
}

static inline int unload_shm_buffer(struct buffer_info *info)
{
	int id;
	char *new_id;

	new_id = strdup(SCHEMA_SHM "-1");
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	if (sscanf(info->id, SCHEMA_SHM "%d", &id) != 1) {
		ErrPrint("%s Invalid ID\n", info->id);
		DbgFree(new_id);
		return -EINVAL;
	}

	if (id < 0) {
		ErrPrint("(%s) Invalid id: %d\n", info->id, id);
		DbgFree(new_id);
		return -EINVAL;
	}

	if (shmdt(info->buffer) < 0)
		ErrPrint("Detach shm: %s\n", strerror(errno));

	if (shmctl(id, IPC_RMID, 0) < 0)
		ErrPrint("Remove shm: %s\n", strerror(errno));

	info->buffer = NULL;

	DbgFree(info->id);
	info->id = new_id;
	return 0;
}

static inline int unload_pixmap_buffer(struct buffer_info *info)
{
	int id;
	char *new_id;

	new_id = strdup(SCHEMA_PIXMAP "0");
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	if (sscanf(info->id, SCHEMA_PIXMAP "%d", &id) != 1) {
		ErrPrint("Invalid ID (%s)\n", info->id);
		DbgFree(new_id);
		return -EINVAL;
	}

	if (id == 0) {
		ErrPrint("(%s) Invalid id: %d\n", info->id, id);
		DbgFree(new_id);
		return -EINVAL;
	}

	/*!
	 * Decrease the reference counter.
	 */
	buffer_handler_pixmap_unref(info->buffer);

	/*!
	 * \note
	 * Just clear the info->buffer.
	 * It will be reallocated again.
	 */
	info->buffer = NULL;

	DbgFree(info->id);
	info->id = new_id;
	return 0;
}

HAPI int buffer_handler_unload(struct buffer_info *info)
{
	int ret;

	if (!info) {
		DbgPrint("buffer handler is nil\n");
		return -EINVAL;
	}

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return -EINVAL;
	}

	switch (info->type) {
	case BUFFER_TYPE_FILE:
		ret = unload_file_buffer(info);
		break;
	case BUFFER_TYPE_SHM:
		ret = unload_shm_buffer(info);
		break;
	case BUFFER_TYPE_PIXMAP:
		ret = unload_pixmap_buffer(info);
		break;
	default:
		ErrPrint("Invalid buffer\n");
		ret = -EINVAL;
		break;
	}

	if (ret == 0)
		info->is_loaded = 0;

	return ret;
}

HAPI int buffer_handler_destroy(struct buffer_info *info)
{
	Eina_List *l;
	struct buffer *buffer;

	if (!info) {
		DbgPrint("Buffer is not created yet. info is nil\n");
		return 0;
	}

	EINA_LIST_FOREACH(s_info.pixmap_list, l, buffer) {
		if (buffer->info == info)
			buffer->info = NULL;
	}

	buffer_handler_unload(info);
	DbgFree(info->id);
	DbgFree(info);
	return 0;
}

HAPI const char *buffer_handler_id(const struct buffer_info *info)
{
	return info ? info->id : "";
}

HAPI enum buffer_type buffer_handler_type(const struct buffer_info *info)
{
	return info ? info->type : BUFFER_TYPE_ERROR;
}

HAPI void *buffer_handler_fb(struct buffer_info *info)
{
	struct buffer *buffer;

	if (!info)
		return NULL;

	buffer = info->buffer;

	if (info->type == BUFFER_TYPE_PIXMAP) {
		void *canvas;
		int ret;

		/*!
		 */
		canvas = buffer_handler_pixmap_acquire_buffer(info);
		ret = buffer_handler_pixmap_release_buffer(canvas);
		DbgPrint("Canvas %p(%d) (released but still in use)\n", canvas, ret);
		return canvas;
	}

	return buffer->data;
}

HAPI int buffer_handler_pixmap(const struct buffer_info *info)
{
	struct buffer *buf;
	struct gem_data *gem;

	if (!info) {
		ErrPrint("Inavlid buffer handler\n");
		return 0;
	}

	if (info->type != BUFFER_TYPE_PIXMAP) {
		ErrPrint("Invalid buffer type\n");
		return 0;
	}

	buf = (struct buffer *)info->buffer;
	if (!buf) {
		ErrPrint("Invalid buffer data\n");
		return 0;
	}

	gem = (struct gem_data *)buf->data;
	return gem->pixmap;
}

HAPI void *buffer_handler_pixmap_acquire_buffer(struct buffer_info *info)
{
	struct buffer *buffer;

	if (!info || !info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return NULL;
	}

	buffer = buffer_handler_pixmap_ref(info);
	if (!buffer)
		return NULL;

	return acquire_gem(buffer);
}

HAPI void *buffer_handler_pixmap_buffer(struct buffer_info *info)
{
	struct buffer *buffer;
	struct gem_data *gem;

	if (!info)
		return NULL;

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return NULL;
	}

	buffer = info->buffer;
	if (!buffer)
		return NULL;

	gem = (struct gem_data *)buffer->data;
	return gem->compensate_data ? gem->compensate_data : gem->data;
}

/*!
 * \return "buffer" object (Not the buffer_info)
 */
HAPI void *buffer_handler_pixmap_ref(struct buffer_info *info)
{
	struct buffer *buffer;

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return NULL;
	}

	if (info->type != BUFFER_TYPE_PIXMAP) {
		ErrPrint("Buffer type is not matched\n");
		return NULL;
	}

	buffer = info->buffer;
	if (!buffer) {
		Display *disp;
		Window root;

		disp = ecore_x_display_get();
		root = DefaultRootWindow(disp);

		buffer = create_gem(disp, root, info);
		if (!buffer) {
			DbgPrint("No GEM initialization\n");
			return NULL;
		}

		info->buffer = buffer;
	} else if (buffer->state != CREATED || buffer->type != BUFFER_TYPE_PIXMAP) {
		ErrPrint("Invalid buffer\n");
		return NULL;
	} else if (buffer->refcnt > 0) {
		buffer->refcnt++;
		return buffer;
	}

	s_info.pixmap_list = eina_list_append(s_info.pixmap_list, buffer);
	return buffer;
}

/*!
 * \return "buffer"
 */
HAPI void *buffer_handler_pixmap_find(int pixmap)
{
	struct buffer *buffer;
	struct gem_data *gem;
	Eina_List *l;
	Eina_List *n;

	if (pixmap == 0)
		return NULL;

	EINA_LIST_FOREACH_SAFE(s_info.pixmap_list, l, n, buffer) {
		if (!buffer || buffer->state != CREATED || buffer->type != BUFFER_TYPE_PIXMAP) {
			s_info.pixmap_list = eina_list_remove(s_info.pixmap_list, buffer);
			DbgPrint("Invalid buffer (List Removed: %p)\n", buffer);
			continue;
		}

		gem = (struct gem_data *)buffer->data;
		if (gem->pixmap == pixmap)
			return buffer;
	}

	return NULL;
}

HAPI int buffer_handler_pixmap_release_buffer(void *canvas)
{
	struct buffer *buffer;
	struct gem_data *gem;
	Eina_List *l;
	Eina_List *n;
	void *_ptr;

	if (!canvas)
		return -EINVAL;

	EINA_LIST_FOREACH_SAFE(s_info.pixmap_list, l, n, buffer) {
		if (!buffer || buffer->state != CREATED || buffer->type != BUFFER_TYPE_PIXMAP) {
			s_info.pixmap_list = eina_list_remove(s_info.pixmap_list, buffer);
			continue;
		}

		gem = (struct gem_data *)buffer->data;
		_ptr = gem->compensate_data ? gem->compensate_data : gem->data;

		if (!_ptr)
			continue;

		if (_ptr == canvas) {
			release_gem(buffer);
			buffer_handler_pixmap_unref(buffer);
			return 0;
		}
	}

	return -ENOENT;
}

/*!
 * \note
 *
 * \return Return NULL if the buffer is in still uses.
 * 	   Return buffer_ptr if it needs to destroy
 */
HAPI int buffer_handler_pixmap_unref(void *buffer_ptr)
{
	struct buffer *buffer = buffer_ptr;
	struct buffer_info *info;

	buffer->refcnt--;
	if (buffer->refcnt > 0)
		return 0; /* Return NULL means, gem buffer still in use */

	s_info.pixmap_list = eina_list_remove(s_info.pixmap_list, buffer);

	destroy_gem(buffer);

	info = buffer->info;
	if (info && info->buffer == buffer)
		info->buffer = NULL;

	return 0;
}

HAPI int buffer_handler_is_loaded(const struct buffer_info *info)
{
	return info ? info->is_loaded : 0;
}

HAPI void buffer_handler_update_size(struct buffer_info *info, int w, int h)
{
	if (!info)
		return;

	info->w = w;
	info->h = h;
}

HAPI int buffer_handler_resize(struct buffer_info *info, int w, int h)
{
	int ret;

	if (!info) {
		ErrPrint("Invalid handler\n");
		return -EINVAL;
	}

	if (info->w == w && info->h == h) {
		DbgPrint("No changes\n");
		return 0;
	}

	buffer_handler_update_size(info, w, h);

	if (!info->is_loaded) {
		DbgPrint("Not yet loaded, just update the size [%dx%d]\n", w, h);
		return 0;
	}

	ret = buffer_handler_unload(info);
	if (ret < 0)
		ErrPrint("Unload: %d\n", ret);

	ret = buffer_handler_load(info);
	if (ret < 0)
		ErrPrint("Load: %d\n", ret);

	return 0;
}

HAPI int buffer_handler_get_size(struct buffer_info *info, int *w, int *h)
{
	if (!info)
		return -EINVAL;

	if (w)
		*w = info->w;
	if (h)
		*h = info->h;

	return 0;
}

HAPI struct inst_info *buffer_handler_instance(struct buffer_info *info)
{
	return info->inst;
}

/*!
 * \note
 * Only for used S/W Backend
 */
static inline int sync_for_pixmap(struct buffer *buffer)
{
	XShmSegmentInfo si;
	XImage *xim;
	GC gc;
	Display *disp;
	struct gem_data *gem;
	Screen *screen;
	Visual *visual;

	if (buffer->state != CREATED) {
		ErrPrint("Invalid state of a FB\n");
		return -EINVAL;
	}

	if (buffer->type != BUFFER_TYPE_PIXMAP) {
		DbgPrint("Invalid buffer\n");
		return 0;
	}

	disp = ecore_x_display_get();
	if (!disp) {
		ErrPrint("Failed to get a display\n");
		return -EFAULT;
	}

	gem = (struct gem_data *)buffer->data;
	if (gem->w == 0 || gem->h == 0) {
		DbgPrint("Nothing can be sync\n");
		return 0;
	}

	si.shmid = shmget(IPC_PRIVATE, gem->w * gem->h * gem->depth, IPC_CREAT | 0666);
	if (si.shmid < 0) {
		ErrPrint("shmget: %s\n", strerror(errno));
		return -EFAULT;
	}

	si.readOnly = False;
	si.shmaddr = shmat(si.shmid, NULL, 0);
	if (si.shmaddr == (void *)-1) {
		if (shmctl(si.shmid, IPC_RMID, 0) < 0)
			ErrPrint("shmctl: %s\n", strerror(errno));
		return -EFAULT;
	}

	screen = DefaultScreenOfDisplay(disp);
	visual = DefaultVisualOfScreen(screen);
	/*!
	 * \NOTE
	 * XCreatePixmap can only uses 24 bits depth only.
	 */
	xim = XShmCreateImage(disp, visual, 24 /* (gem->depth << 3) */, ZPixmap, NULL, &si, gem->w, gem->h);
	if (xim == NULL) {
		if (shmdt(si.shmaddr) < 0)
			ErrPrint("shmdt: %s\n", strerror(errno));

		if (shmctl(si.shmid, IPC_RMID, 0) < 0)
			ErrPrint("shmctl: %s\n", strerror(errno));
		return -EFAULT;
	}

	xim->data = si.shmaddr;
	XShmAttach(disp, &si);
	XSync(disp, False);

	gc = XCreateGC(disp, gem->pixmap, 0, NULL);
	if (!gc) {
		XShmDetach(disp, &si);
		XDestroyImage(xim);

		if (shmdt(si.shmaddr) < 0)
			ErrPrint("shmdt: %s\n", strerror(errno));

		if (shmctl(si.shmid, IPC_RMID, 0) < 0)
			ErrPrint("shmctl: %s\n", strerror(errno));

		return -EFAULT;
	}

	memcpy(xim->data, gem->data, gem->w * gem->h * gem->depth);

	/*!
	 * \note Do not send the event.
	 *       Instead of X event, master will send the updated event to the viewer
	 */
	XShmPutImage(disp, gem->pixmap, gc, xim, 0, 0, 0, 0, gem->w, gem->h, False);
	XSync(disp, False);

	XFreeGC(disp, gc);
	XShmDetach(disp, &si);
	XDestroyImage(xim);

	if (shmdt(si.shmaddr) < 0)
		ErrPrint("shmdt: %s\n", strerror(errno));

	if (shmctl(si.shmid, IPC_RMID, 0) < 0)
		ErrPrint("shmctl: %s\n", strerror(errno));

	return 0;
}

HAPI void buffer_handler_flush(struct buffer_info *info)
{
	int fd;
	int size;
	struct buffer *buffer;

	if (!info || !info->buffer)
		return;

	buffer = info->buffer;

	if (buffer->type == BUFFER_TYPE_PIXMAP) {
		if (s_info.fd > 0) {
			XRectangle rect;
			XserverRegion region;

			rect.x = 0;
			rect.y = 0;
			rect.width = info->w;
			rect.height = info->h;

			region = XFixesCreateRegion(ecore_x_display_get(), &rect, 1);
			XDamageAdd(ecore_x_display_get(), buffer_handler_pixmap(info), region);
			XFixesDestroyRegion(ecore_x_display_get(), region);
			XFlush(ecore_x_display_get());
		} else {
			if (sync_for_pixmap(buffer) < 0)
				ErrPrint("Failed to sync via S/W Backend\n");
		}
	} else if (buffer->type == BUFFER_TYPE_FILE) {
		fd = open(util_uri_to_path(info->id), O_WRONLY | O_CREAT, 0644);
		if (fd < 0) {
			ErrPrint("%s open falied: %s\n", util_uri_to_path(info->id), strerror(errno));
			return;
		}

		size = info->w * info->h * info->pixel_size;
		if (write(fd, info->buffer, size) != size)
			ErrPrint("Write is not completed: %s\n", strerror(errno));

		close(fd);
	} else {
		DbgPrint("Flush nothing\n");
	}
}

HAPI int buffer_handler_init(void)
{
	int dri2Major, dri2Minor;
	char *driverName, *deviceName;
	drm_magic_t magic;

	if (!DRI2QueryExtension(ecore_x_display_get(), &s_info.evt_base, &s_info.err_base)) {
		DbgPrint("DRI2 is not supported\n");
		return 0;
	}

	if (!DRI2QueryVersion(ecore_x_display_get(), &dri2Major, &dri2Minor)) {
		DbgPrint("DRI2 is not supported\n");
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	if (!DRI2Connect(ecore_x_display_get(), DefaultRootWindow(ecore_x_display_get()), &driverName, &deviceName)) {
		DbgPrint("DRI2 is not supported\n");
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	DbgPrint("Open: %s (driver: %s)", deviceName, driverName);

	if (USE_SW_BACKEND) {
		DbgPrint("Fallback to the S/W Backend\n");
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	s_info.fd = open(deviceName, O_RDWR);
	DbgFree(deviceName);
	DbgFree(driverName);
	if (s_info.fd < 0) {
		DbgPrint("Failed to open a drm device: (%s)\n", strerror(errno));
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	drmGetMagic(s_info.fd, &magic);
	DbgPrint("DRM Magic: 0x%X\n", magic);
	if (!DRI2Authenticate(ecore_x_display_get(), DefaultRootWindow(ecore_x_display_get()), (unsigned int)magic)) {
		DbgPrint("Failed to do authenticate for DRI2\n");
		close(s_info.fd);
		s_info.fd = -1;
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	s_info.slp_bufmgr = drm_slp_bufmgr_init(s_info.fd, NULL);
	if (!s_info.slp_bufmgr) {
		DbgPrint("Failed to init bufmgr\n");
		close(s_info.fd);
		s_info.fd = -1;
		s_info.evt_base = 0;
		s_info.err_base = 0;
		return 0;
	}

	return 0;
}

HAPI int buffer_handler_fini(void)
{
	if (s_info.fd >= 0) {
		close(s_info.fd);
		s_info.fd = -1;
	}

	if (s_info.slp_bufmgr) {
		drm_slp_bufmgr_destroy(s_info.slp_bufmgr);
		s_info.slp_bufmgr = NULL;
	}

	return 0;
}

/* End of a file */
