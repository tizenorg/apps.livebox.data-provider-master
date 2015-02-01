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

#include <dlog.h>
#include <packet.h>
#include <dynamicbox_errno.h>

#include "debug.h"
#include "conf.h"
#include "util.h"
#include "instance.h"
#include "package.h"
#include "client_life.h"
#include "client_rpc.h"
#include "buffer_handler.h"
#include "script_handler.h" // Reverse dependency. must has to be broken

struct buffer {
	enum {
		CREATED = 0x00beef00,
		DESTROYED = 0x00dead00
	} state;
	enum buffer_type type;
	int refcnt;
	void *info;
	char data[];
};

struct buffer_info
{
	void *buffer;
	char *id;
	char *lock;
	int lock_fd;

	enum buffer_type type;

	int w;
	int h;
	int pixel_size;
	int auto_align;
	int is_loaded;

	struct inst_info *inst;
	void *data;
};

static int destroy_lock_file(struct buffer_info *info)
{
	if (!info->inst) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (!info->lock) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (close(info->lock_fd) < 0) {
		ErrPrint("close: %s\n", strerror(errno));
	}
	info->lock_fd = -1;

	if (unlink(info->lock) < 0) {
		ErrPrint("unlink: %s\n", strerror(errno));
	}

	DbgFree(info->lock);
	info->lock = NULL;
	return DBOX_STATUS_ERROR_NONE;
}

static int create_lock_file(struct buffer_info *info)
{
	const char *id;
	int len;
	char *file;
	char target[3] = "pd";

	if (!info->inst) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	id = instance_id(info->inst);
	if (!id) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	len = strlen(id);
	file = malloc(len + 20);
	if (!file) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	if (script_handler_buffer_info(instance_gbar_script(info->inst)) != info && instance_gbar_buffer(info->inst) != info) {
		target[0] = 'l';
		target[1] = 'b';
		/* target[2] = '\0'; // We already have this ;) */
	}

	snprintf(file, len + 20, "%s.%s.lck", util_uri_to_path(id), target);
	info->lock_fd = open(file, O_WRONLY|O_CREAT, 0644);
	if (info->lock_fd < 0) {
		ErrPrint("open: %s\n", strerror(errno));
		DbgFree(file);
		return DBOX_STATUS_ERROR_IO_ERROR;
	}

	info->lock = file;
	return DBOX_STATUS_ERROR_NONE;
}

static int do_buffer_lock(struct buffer_info *buffer)
{
	struct flock flock;
	int ret;

	if (buffer->lock_fd < 0) {
		return DBOX_STATUS_ERROR_NONE;
	}

	flock.l_type = F_WRLCK;
	flock.l_whence = SEEK_SET;
	flock.l_start = 0;
	flock.l_len = 0;
	flock.l_pid = getpid();

	do {
		ret = fcntl(buffer->lock_fd, F_SETLKW, &flock);
		if (ret < 0) {
			ret = errno;
			ErrPrint("fcntl: %s\n", strerror(errno));
		}
	} while (ret == EINTR);

	return DBOX_STATUS_ERROR_NONE;
}

static int do_buffer_unlock(struct buffer_info *buffer)
{
	struct flock flock;
	int ret;

	if (buffer->lock_fd < 0) {
		return DBOX_STATUS_ERROR_NONE;
	}

	flock.l_type = F_UNLCK;
	flock.l_whence = SEEK_SET;
	flock.l_start = 0;
	flock.l_len = 0;
	flock.l_pid = getpid();

	do {
		ret = fcntl(buffer->lock_fd, F_SETLKW, &flock);
		if (ret < 0) {
			ret = errno;
			ErrPrint("fcntl: %s\n", strerror(errno));
		}
	} while (ret == EINTR);

	return DBOX_STATUS_ERROR_NONE;
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
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	timestamp = util_timestamp();
	snprintf(new_id, len, SCHEMA_FILE "%s%lf", IMAGE_PATH, timestamp);

	size = sizeof(*buffer) + info->w * info->h * info->pixel_size;
	if (!size) {
		ErrPrint("Canvas buffer size is ZERO\n");
		DbgFree(new_id);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	buffer = calloc(1, size);
	if (!buffer) {
		ErrPrint("Failed to allocate buffer\n");
		DbgFree(new_id);
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
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
	return DBOX_STATUS_ERROR_NONE;
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
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	id = shmget(IPC_PRIVATE, size + sizeof(*buffer), IPC_CREAT | 0666);
	if (id < 0) {
		ErrPrint("shmget: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_FAULT;
	}

	buffer = shmat(id, NULL, 0);
	if (buffer == (void *)-1) {
		ErrPrint("%s shmat: %s\n", info->id, strerror(errno));

		if (shmctl(id, IPC_RMID, 0) < 0) {
			ErrPrint("%s shmctl: %s\n", info->id, strerror(errno));
		}

		return DBOX_STATUS_ERROR_FAULT;
	}

	buffer->type = BUFFER_TYPE_SHM;
	buffer->refcnt = id;
	buffer->state = CREATED; /*!< Needless */
	buffer->info = (void *)size; /*!< Use this field to indicates the size of SHM */

	len = strlen(SCHEMA_SHM) + 30; /* strlen("shm://") + 30 */

	new_id = malloc(len);
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		if (shmdt(buffer) < 0) {
			ErrPrint("shmdt: %s\n", strerror(errno));
		}

		if (shmctl(id, IPC_RMID, 0) < 0) {
			ErrPrint("shmctl: %s\n", strerror(errno));
		}

		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	snprintf(new_id, len, SCHEMA_SHM "%d", id);

	DbgFree(info->id);
	info->id = new_id;
	info->buffer = buffer;
	info->is_loaded = 1;
	return DBOX_STATUS_ERROR_NONE;
}

EAPI int buffer_handler_load(struct buffer_info *info)
{
	int ret;

	if (!info) {
		ErrPrint("buffer handler is nil\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (info->is_loaded) {
		DbgPrint("Buffer is already loaded\n");
		return DBOX_STATUS_ERROR_NONE;
	}

	switch (info->type) {
	case BUFFER_TYPE_FILE:
		ret = load_file_buffer(info);
		(void)create_lock_file(info);
		break;
	case BUFFER_TYPE_SHM:
		ret = load_shm_buffer(info);
		(void)create_lock_file(info);
		break;
	case BUFFER_TYPE_PIXMAP:
	default:
		ErrPrint("Invalid buffer\n");
		ret = DBOX_STATUS_ERROR_INVALID_PARAMETER;
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
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	DbgFree(info->buffer);
	info->buffer = NULL;

	path = util_uri_to_path(info->id);
	if (path && unlink(path) < 0) {
		ErrPrint("unlink: %s\n", strerror(errno));
	}

	DbgFree(info->id);
	info->id = new_id;
	return DBOX_STATUS_ERROR_NONE;
}

static inline int unload_shm_buffer(struct buffer_info *info)
{
	int id;
	char *new_id;

	new_id = strdup(SCHEMA_SHM "-1");
	if (!new_id) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	if (sscanf(info->id, SCHEMA_SHM "%d", &id) != 1) {
		ErrPrint("%s Invalid ID\n", info->id);
		DbgFree(new_id);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (id < 0) {
		ErrPrint("(%s) Invalid id: %d\n", info->id, id);
		DbgFree(new_id);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (shmdt(info->buffer) < 0) {
		ErrPrint("Detach shm: %s\n", strerror(errno));
	}

	if (shmctl(id, IPC_RMID, 0) < 0) {
		ErrPrint("Remove shm: %s\n", strerror(errno));
	}

	info->buffer = NULL;

	DbgFree(info->id);
	info->id = new_id;
	return DBOX_STATUS_ERROR_NONE;
}

EAPI int buffer_handler_unload(struct buffer_info *info)
{
	int ret;

	if (!info) {
		ErrPrint("buffer handler is NIL\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (!info->is_loaded) {
		ErrPrint("Buffer is not loaded\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	switch (info->type) {
	case BUFFER_TYPE_FILE:
		(void)destroy_lock_file(info);
		ret = unload_file_buffer(info);
		break;
	case BUFFER_TYPE_SHM:
		(void)destroy_lock_file(info);
		ret = unload_shm_buffer(info);
		break;
	case BUFFER_TYPE_PIXMAP:
	default:
		ErrPrint("Invalid buffer\n");
		ret = DBOX_STATUS_ERROR_INVALID_PARAMETER;
		break;
	}

	if (ret == 0) {
		info->is_loaded = 0;
	}

	return ret;
}

EAPI const char *buffer_handler_id(const struct buffer_info *info)
{
	return info ? info->id : "";
}

EAPI enum buffer_type buffer_handler_type(const struct buffer_info *info)
{
	return info ? info->type : BUFFER_TYPE_ERROR;
}

EAPI void *buffer_handler_fb(struct buffer_info *info)
{
	struct buffer *buffer;

	if (!info) {
		return NULL;
	}

	buffer = info->buffer;

	if (info->type == BUFFER_TYPE_PIXMAP) {
		return NULL;
	}

	return buffer->data;
}

EAPI int buffer_handler_pixmap(const struct buffer_info *info)
{
	return 0;
}

EAPI void *buffer_handler_pixmap_acquire_buffer(struct buffer_info *info)
{
	return NULL;
}

EAPI void *buffer_handler_pixmap_buffer(struct buffer_info *info)
{
	return NULL;
}

/*!
 * \return "buffer" object (Not the buffer_info)
 */
EAPI void *buffer_handler_pixmap_ref(struct buffer_info *info)
{
	return NULL;
}

/*!
 * \return "buffer"
 */
EAPI void *buffer_handler_pixmap_find(int pixmap)
{
	return NULL;
}

EAPI int buffer_handler_pixmap_release_buffer(void *canvas)
{
	return DBOX_STATUS_ERROR_NOT_EXIST;
}

/*!
 * \note
 *
 * \return Return NULL if the buffer is in still uses.
 * 	   Return buffer_ptr if it needs to destroy
 */
EAPI int buffer_handler_pixmap_unref(void *buffer_ptr)
{
	return DBOX_STATUS_ERROR_NONE;
}

EAPI int buffer_handler_is_loaded(const struct buffer_info *info)
{
	return info ? info->is_loaded : 0;
}

EAPI void buffer_handler_update_size(struct buffer_info *info, int w, int h)
{
	if (!info) {
		return;
	}

	info->w = w;
	info->h = h;
}

EAPI int buffer_handler_resize(struct buffer_info *info, int w, int h)
{
	int ret;

	if (!info) {
		ErrPrint("Invalid handler\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (info->w == w && info->h == h) {
		DbgPrint("No changes\n");
		return DBOX_STATUS_ERROR_NONE;
	}

	buffer_handler_update_size(info, w, h);

	if (!info->is_loaded) {
		DbgPrint("Buffer size is updated[%dx%d]\n", w, h);
		return DBOX_STATUS_ERROR_NONE;
	}

	ret = buffer_handler_unload(info);
	if (ret < 0) {
		ErrPrint("Unload: %d\n", ret);
	}

	ret = buffer_handler_load(info);
	if (ret < 0) {
		ErrPrint("Load: %d\n", ret);
	}

	return DBOX_STATUS_ERROR_NONE;
}

EAPI int buffer_handler_get_size(struct buffer_info *info, int *w, int *h)
{
	if (!info) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (w) {
		*w = info->w;
	}
	if (h) {
		*h = info->h;
	}

	return DBOX_STATUS_ERROR_NONE;
}

EAPI struct inst_info *buffer_handler_instance(struct buffer_info *info)
{
	return info->inst;
}

EAPI void buffer_handler_flush(struct buffer_info *info)
{
	int fd;
	int size;
	struct buffer *buffer;

	if (!info || !info->buffer) {
		return;
	}

	buffer = info->buffer;

	if (buffer->type == BUFFER_TYPE_PIXMAP) {
		/*!
		 * \note
		 * Not supported for wayland or this should be ported correctly
		 */
	} else if (buffer->type == BUFFER_TYPE_FILE) {
		fd = open(util_uri_to_path(info->id), O_WRONLY | O_CREAT, 0644);
		if (fd < 0) {
			ErrPrint("%s open falied: %s\n", util_uri_to_path(info->id), strerror(errno));
			return;
		}

		size = info->w * info->h * info->pixel_size;
		do_buffer_lock(info);
		if (write(fd, info->buffer, size) != size) {
			ErrPrint("Write is not completed: %s\n", strerror(errno));
		}
		do_buffer_unlock(info);

		if (close(fd) < 0) {
			ErrPrint("close: %s\n", strerror(errno));
		}
	} else {
		DbgPrint("Flush nothing\n");
	}
}

HAPI int buffer_handler_init(void)
{
	/*!
	 * \TODO
	 * Implement this for wayland
	 */
	if (USE_SW_BACKEND) {
		DbgPrint("Fallback to the S/W Backend\n");
		return DBOX_STATUS_ERROR_NONE;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int buffer_handler_fini(void)
{
	/*!
	 * \TODO
	 * Implement this for wayland
	 */
	return DBOX_STATUS_ERROR_NONE;
}

static inline struct buffer *raw_open_file(const char *filename)
{
	struct buffer *buffer;
	int fd;
	off_t off;
	int ret;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		ErrPrint("open: %s\n", strerror(errno));
		return NULL;
	}

	off = lseek(fd, 0L, SEEK_END);
	if (off == (off_t)-1) {
		ErrPrint("lseek: %s\n", strerror(errno));

		if (close(fd) < 0) {
			ErrPrint("close: %s\n", strerror(errno));
		}

		return NULL;
	}

	if (lseek(fd, 0L, SEEK_SET) == (off_t)-1) {
		ErrPrint("lseek: %s\n", strerror(errno));

		if (close(fd) < 0) {
			ErrPrint("close: %s\n", strerror(errno));
		}

		return NULL;
	}

	buffer = calloc(1, sizeof(*buffer) + off);
	if (!buffer) {
		ErrPrint("Heap: %s\n", strerror(errno));

		if (close(fd) < 0) {
			ErrPrint("close: %s\n", strerror(errno));
		}

		return NULL;
	}

	buffer->state = CREATED;
	buffer->type = BUFFER_TYPE_FILE;
	buffer->refcnt = 0;
	buffer->info = (void *)off;

	ret = read(fd, buffer->data, off);
	if (ret < 0) {
		ErrPrint("read: %s\n", strerror(errno));
		DbgFree(buffer);

		if (close(fd) < 0) {
			ErrPrint("close: %s\n", strerror(errno));
		}

		return NULL;
	}

	if (close(fd) < 0) {
		ErrPrint("close: %s\n", strerror(errno));
	}

	return buffer;
}

static inline int raw_close_file(struct buffer *buffer)
{
	DbgFree(buffer);
	return 0;
}

static inline struct buffer *raw_open_shm(int shm)
{
	struct buffer *buffer;

	buffer = (struct buffer *)shmat(shm, NULL, SHM_RDONLY);
	if (buffer == (struct buffer *)-1) {
		ErrPrint("shmat: %s\n", strerror(errno));
		return NULL;
	}

	return buffer;
}

static inline int raw_close_shm(struct buffer *buffer)
{
	int ret;

	ret = shmdt(buffer);
	if (ret < 0) {
		ErrPrint("shmdt: %s\n", strerror(errno));
	}

	return ret;
}

EAPI void *buffer_handler_raw_data(struct buffer *buffer)
{
	if (!buffer || buffer->state != CREATED) {
		return NULL;
	}

	return buffer->data;
}

EAPI int buffer_handler_raw_size(struct buffer *buffer)
{
	if (!buffer || buffer->state != CREATED) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	return (int)buffer->info;
}

EAPI struct buffer *buffer_handler_raw_open(enum buffer_type buffer_type, void *resource)
{
	struct buffer *handle;

	switch (buffer_type) {
	case BUFFER_TYPE_SHM:
		handle = raw_open_shm((int)resource);
		break;
	case BUFFER_TYPE_FILE:
		handle = raw_open_file(resource);
		break;
	case BUFFER_TYPE_PIXMAP:
	default:
		handle = NULL;
		break;
	}

	return handle;
}

EAPI int buffer_handler_raw_close(struct buffer *buffer)
{
	int ret;

	switch (buffer->type) {
	case BUFFER_TYPE_SHM:
		ret = raw_close_shm(buffer);
		break;
	case BUFFER_TYPE_FILE:
		ret = raw_close_file(buffer);
		break;
	case BUFFER_TYPE_PIXMAP:
	default:
		ret = DBOX_STATUS_ERROR_INVALID_PARAMETER;
		break;
	}

	return ret;
}

EAPI int buffer_handler_lock(struct buffer_info *buffer)
{
	if (buffer->type == BUFFER_TYPE_PIXMAP) {
		return DBOX_STATUS_ERROR_NONE;
	}

	if (buffer->type == BUFFER_TYPE_FILE) {
		return DBOX_STATUS_ERROR_NONE;
	}

	return do_buffer_lock(buffer);
}

EAPI int buffer_handler_unlock(struct buffer_info *buffer)
{
	if (buffer->type == BUFFER_TYPE_PIXMAP) {
		return DBOX_STATUS_ERROR_NONE;
	}

	if (buffer->type == BUFFER_TYPE_FILE) {
		return DBOX_STATUS_ERROR_NONE;
	}

	return do_buffer_unlock(buffer);
}

/*!
 * \note
 * Only can be used by master.
 * Plugin cannot access the user data
 */

HAPI int buffer_handler_set_data(struct buffer_info *buffer, void *data)
{
	if (!buffer) {
		ErrPrint("Invalid handle\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	buffer->data = data;
	return DBOX_STATUS_ERROR_NONE;
}

HAPI void *buffer_handler_data(struct buffer_info *buffer)
{
	if (!buffer) {
		ErrPrint("Invalid handle\n");
		return NULL;
	}

	return buffer->data;
}

HAPI int buffer_handler_destroy(struct buffer_info *info)
{
	Eina_List *l;
	struct buffer *buffer;

	if (!info) {
		DbgPrint("Buffer is not created yet. info is NIL\n");
		return DBOX_STATUS_ERROR_NONE;
	}

	buffer_handler_unload(info);
	DbgFree(info->id);
	DbgFree(info);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI struct buffer_info *buffer_handler_create(struct inst_info *inst, enum buffer_type type, int w, int h, int pixel_size, int auto_align)
{
	struct buffer_info *info;

	info = malloc(sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	switch (type) {
	case BUFFER_TYPE_SHM:
		if (pixel_size != DEFAULT_PIXELS) {
			DbgPrint("SHM only supportes %d bytes pixels (requested: %d)\n", DEFAULT_PIXELS, pixel_size);
			pixel_size = DEFAULT_PIXELS;
		}

		info->id = strdup(SCHEMA_SHM "-1");
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(info);
			return NULL;
		}
		break;
	case BUFFER_TYPE_FILE:
		if (pixel_size != DEFAULT_PIXELS) {
			DbgPrint("FILE only supportes %d bytes pixels (requested: %d)\n", DEFAULT_PIXELS, pixel_size);
			pixel_size = DEFAULT_PIXELS;
		}

		info->id = strdup(SCHEMA_FILE "/tmp/.live.undefined");
		if (!info->id) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(info);
			return NULL;
		}
		break;
	case BUFFER_TYPE_PIXMAP:
	default:
		ErrPrint("Invalid type\n");
		DbgFree(info);
		return NULL;
	}

	info->lock = NULL;
	info->lock_fd = -1;
	info->w = w;
	info->h = h;
	info->pixel_size = pixel_size;
	info->type = type;
	info->is_loaded = 0;
	info->inst = inst;
	info->buffer = NULL;
	info->data = NULL;
	info->auto_align = auto_align;

	return info;
}

/* End of a file */
