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
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <Eina.h>

#include <dlog.h>
#include <dynamicbox_errno.h>

#include "util.h"
#include "debug.h"
#include "conf.h"

int errno;

static struct info {
	Eina_List *info_list;
} s_info = {
	.info_list = NULL,
};

struct liveinfo {
	FILE *fp;
	char fifo_name[60];
	pid_t pid;
	int handle;
	void *data;
};

HAPI int liveinfo_init(void)
{
	return 0;
}

HAPI void liveinfo_fini(void)
{
	struct liveinfo *info;

	EINA_LIST_FREE(s_info.info_list, info) {
		if (fclose(info->fp) != 0) {
			ErrPrint("fclose: %s\n", strerror(errno));
		}
		if (unlink(info->fifo_name) < 0) {
			ErrPrint("unlink: %s\n", strerror(errno));
		}
		DbgFree(info);
	}
}

static inline int valid_requestor(pid_t pid)
{
	char cmdline[60]; /* strlen("/proc/%d/cmdline") + 30 */
	struct stat target;
	struct stat src;

	snprintf(cmdline, sizeof(cmdline), "/proc/%d/exe", pid);

	DbgPrint("Open cmdline: %s (%d)\n", cmdline, pid);

	if (stat(cmdline, &target) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		return 0;
	}

	if (stat("/opt/usr/devel/usr/bin/liveinfo", &src) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		return 0;
	}

	if (target.st_ino == src.st_ino) {
		return 1;
	}

	if (stat("/opt/usr/devel/usr/bin/dbox-mgr", &src) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		return 0;
	}

	return target.st_ino == src.st_ino;
}

HAPI void liveinfo_set_data(struct liveinfo *info, void *data)
{
	info->data = data;
}

HAPI void *liveinfo_data(struct liveinfo *info)
{
	return info->data;
}

HAPI struct liveinfo *liveinfo_create(pid_t pid, int handle)
{
	struct liveinfo *info;

	if (!valid_requestor(pid)) {
		ErrPrint("Invalid requestor\n");
		return NULL;
	}

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	snprintf(info->fifo_name, sizeof(info->fifo_name), "/tmp/.live_info.%lf", util_timestamp());
	if (mkfifo(info->fifo_name, 0644) < 0) {
		ErrPrint("mkfifo: %s\n", strerror(errno));
		if (unlink(info->fifo_name) < 0) {
			ErrPrint("unlink: %s\n", strerror(errno));
		}
		DbgFree(info);
		return NULL;
	}

	info->fp = NULL;
	info->pid = pid;
	info->handle = handle;

	DbgPrint("Live info is successfully created\n");
	s_info.info_list = eina_list_append(s_info.info_list, info);
	return info;
}

HAPI int liveinfo_open_fifo(struct liveinfo *info)
{
	DbgPrint("FIFO is created (%s)\n", info->fifo_name);
	info->fp = fopen(info->fifo_name, "w");
	if (!info->fp) {
		ErrPrint("open: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_IO_ERROR;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI void liveinfo_close_fifo(struct liveinfo *info)
{
	if (info->fp) {
		if (fclose(info->fp) != 0) {
			ErrPrint("fclose: %s\n", strerror(errno));
		}
		info->fp = NULL;
	}
}

HAPI void liveinfo_destroy(struct liveinfo *info)
{
	s_info.info_list = eina_list_remove(s_info.info_list, info);
	liveinfo_close_fifo(info);
	if (unlink(info->fifo_name) < 0) {
		ErrPrint("unlink: %s\n", strerror(errno));
	}
	DbgFree(info);
}

HAPI pid_t liveinfo_pid(struct liveinfo *info)
{
	return info ? info->pid : (pid_t)-1;
}

HAPI const char *liveinfo_filename(struct liveinfo *info)
{
	return info ? info->fifo_name : NULL;
}

HAPI FILE *liveinfo_fifo(struct liveinfo *info)
{
	return info ? info->fp : NULL;
}

HAPI struct liveinfo *liveinfo_find_by_pid(pid_t pid)
{
	Eina_List *l;
	struct liveinfo *info;

	EINA_LIST_FOREACH(s_info.info_list, l, info) {
		if (info->pid == pid) {
			return info;
		}
	}

	return NULL;
}

HAPI struct liveinfo *liveinfo_find_by_handle(int handle)
{
	Eina_List *l;
	struct liveinfo *info;

	EINA_LIST_FOREACH(s_info.info_list, l, info) {
		if (info->handle == handle) {
			return info;
		}
	}

	return NULL;
}

/* End of a file */
