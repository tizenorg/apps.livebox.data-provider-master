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
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/vfs.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>

#include <dlog.h>
#include <Eina.h>
#include <Ecore.h>
#include <livebox-errno.h>

#include "util.h"
#include "debug.h"
#include "conf.h"

int errno;

HAPI unsigned long util_string_hash(const char *str)
{
	unsigned long ret = 0;

	while (*str)
		ret += (unsigned long)(*str++);

	ret %= 371773;
	return ret;
}

HAPI double util_timestamp(void)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) < 0) {
		static unsigned long internal_count = 0;
		ErrPrint("failed to get time of day: %s\n", strerror(errno));
		tv.tv_sec = internal_count++;
		tv.tv_usec = 0;
	}

	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
}

HAPI int util_check_ext(const char *filename, const char *check_ptr)
{
	int name_len;

	name_len = strlen(filename);
	while (--name_len >= 0 && *check_ptr) {
		if (filename[name_len] != *check_ptr)
			return LB_STATUS_ERROR_INVALID;

		check_ptr ++;
	}

	return LB_STATUS_SUCCESS;
}

static inline int check_native_livebox(const char *pkgname)
{
	int len;
	char *path;

	len = strlen(pkgname) * 2;
	len += strlen(ROOT_PATH);
	len += strlen("%s/libexec/liblive-%s.so");

	path = malloc(len + 1);
	if (!path) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	snprintf(path, len, "%s%s/libexec/liblive-%s.so", ROOT_PATH, pkgname, pkgname);
	if (access(path, F_OK | R_OK) != 0) {
		ErrPrint("%s is not a valid package\n", pkgname);
		DbgFree(path);
		return LB_STATUS_ERROR_INVALID;
	}

	DbgFree(path);
	return LB_STATUS_SUCCESS;
}

static inline int check_web_livebox(const char *pkgname)
{
	int len;
	char *path;

	len = strlen(pkgname) * 2;
	len += strlen("/opt/usr/apps/%s/res/wgt/livebox/index.html");

	path = malloc(len + 1);
	if (!path) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	snprintf(path, len, "/opt/usr/apps/%s/res/wgt/livebox/index.html", pkgname);
	if (access(path, F_OK | R_OK) != 0) {
		ErrPrint("%s is not a valid package\n", pkgname);
		DbgFree(path);
		return LB_STATUS_ERROR_INVALID;
	}

	DbgFree(path);
	return LB_STATUS_SUCCESS;
}

HAPI int util_validate_livebox_package(const char *pkgname)
{
	if (!pkgname) {
		ErrPrint("Invalid argument\n");
		return LB_STATUS_ERROR_INVALID;
	}

	if (!check_native_livebox(pkgname) || !check_web_livebox(pkgname))
		return LB_STATUS_SUCCESS;

	return LB_STATUS_ERROR_INVALID;
}

HAPI int util_unlink(const char *filename)
{
	char *descfile;
	int desclen;
	int ret;

	if (!filename)
		return LB_STATUS_ERROR_INVALID;

	desclen = strlen(filename) + 6; /* .desc */
	descfile = malloc(desclen);
	if (!descfile) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	ret = snprintf(descfile, desclen, "%s.desc", filename);
	if (ret < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		DbgFree(descfile);
		return LB_STATUS_ERROR_FAULT;
	}

	(void)unlink(descfile);
	DbgFree(descfile);
	(void)unlink(filename);

	return LB_STATUS_SUCCESS;
}

HAPI char *util_slavename(void)
{
	char slavename[BUFSIZ];
	static unsigned long idx = 0;

	snprintf(slavename, sizeof(slavename), "%lu_%lf", idx++, util_timestamp());
	return strdup(slavename);
}

HAPI const char *util_basename(const char *name)
{
	int length;
	length = name ? strlen(name) : 0;
	if (!length)
		return ".";

	while (--length > 0 && name[length] != '/');

	return length <= 0 ? name : (name + length + (name[length] == '/'));
}

/*!
 * Return size of stroage in MegaBytes unit.
 */
HAPI unsigned long long util_free_space(const char *path)
{
	struct statfs st;
	unsigned long long space;

	if (statfs(path, &st) < 0) {
		ErrPrint("statvfs: %s\n", strerror(errno));
		return 0lu;
	}

	space = (unsigned long long)st.f_bsize * (unsigned long long)st.f_bavail;
	DbgPrint("Available size: %llu, f_bsize: %lu, f_bavail: %lu\n", space, st.f_bsize, st.f_bavail);
	/*!
	 * \note
	 * Must have to check the overflow
	 */
	return space;
}

static inline char *extend_heap(char *buffer, int *sz, int incsz)
{
	char *tmp;

	*sz += incsz;
	tmp = realloc(buffer, *sz);
	if (!tmp) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	return tmp;
}


HAPI char *util_replace_string(const char *src, const char *pattern, const char *replace)
{
	const char *ptr;
	char *tmp;
	char *ret = NULL;
	int idx;
	int out_idx;
	int out_sz;
	enum {
		STATE_START,
		STATE_FIND,
		STATE_CHECK,
		STATE_END,
	} state;

	if (!src || !pattern)
		return NULL;

	out_sz = strlen(src);
	ret = strdup(src);
	if (!ret) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	out_idx = 0;
	for (state = STATE_START, ptr = src; state != STATE_END; ptr++) {
		switch (state) {
		case STATE_START:
			if (*ptr == '\0') {
				state = STATE_END;
			} else if (!isblank(*ptr)) {
				state = STATE_FIND;
				ptr--;
			}
			break;
		case STATE_FIND:
			if (*ptr == '\0') {
				state = STATE_END;
			} else if (*ptr == *pattern) {
				state = STATE_CHECK;
				ptr--;
				idx = 0;
			} else {
				ret[out_idx] = *ptr;
				out_idx++;
				if (out_idx == out_sz) {
					tmp = extend_heap(ret, &out_sz, strlen(replace) + 1);
					if (!tmp) {
						free(ret);
						return NULL;
					}
					ret = tmp;
				}
			}
			break;
		case STATE_CHECK:
			if (!pattern[idx]) {
				/*!
				 * If there is no space for copying the replacement,
				 * Extend size of the return buffer.
				 */
				if (out_sz - out_idx < strlen(replace) + 1) {
					tmp = extend_heap(ret, &out_sz, strlen(replace) + 1);
					if (!tmp) {
						free(ret);
						return NULL;
					}
					ret = tmp;
				}

				strcpy(ret + out_idx, replace);
				out_idx += strlen(replace);

				state = STATE_FIND;
				ptr--;
			} else if (*ptr != pattern[idx]) {
				ptr -= idx;

				/* Copy the first matched character */
				ret[out_idx] = *ptr;
				out_idx++;
				if (out_idx == out_sz) {
					tmp = extend_heap(ret, &out_sz, strlen(replace) + 1);
					if (!tmp) {
						free(ret);
						return NULL;
					}

					ret = tmp;
				}

				state = STATE_FIND;
			} else {
				idx++;
			}
			break;
		default:
			break;
		}
	}

	return ret;
}


HAPI const char *util_uri_to_path(const char *uri)
{
	int len;

	len = strlen(SCHEMA_FILE);
	if (strncasecmp(uri, SCHEMA_FILE, len))
		return NULL;

	return uri + len;
}

HAPI double util_time_delay_for_compensation(double period)
{
	struct timeval tv;
	unsigned long long curtime;
	unsigned long long _period;
	unsigned long long remain;
	double ret;

	if (period == 0.0f) {
		DbgPrint("Period is ZERO\n");
		return 0.0f;
	}

	if (gettimeofday(&tv, NULL) < 0){
		ErrPrint("gettimeofday: %s\n", strerror(errno));
		return period;
	}

	curtime = (unsigned long long)tv.tv_sec * 1000000llu + (unsigned long long)tv.tv_usec;
	_period = (unsigned long long)(period * (double)1000000);
	if (_period == 0llu) {
		ErrPrint("%lf <> %llu\n", period, _period);
		return period;
	}

	remain = curtime % _period;

	ret = (double)remain / (double)1000000;
	DbgPrint("curtime: %llu, _period: %llu, remain: %llu, ret: %lf, result: %lf\n", curtime, _period, remain, ret, period - ret);
	return period - ret;
}

HAPI void *util_timer_add(double interval, Eina_Bool (*cb)(void *data), void *data)
{
	Ecore_Timer *timer;
	double delay;

	timer = ecore_timer_add(interval, cb, data);
	if (!timer)
		return NULL;

	delay = util_time_delay_for_compensation(interval) - interval;
	ecore_timer_delay(timer, delay);
	DbgPrint("Compensate timer: %lf\n", delay);

	return timer;
}

HAPI void util_timer_interval_set(void *timer, double interval)
{
	double delay;
	ecore_timer_interval_set(timer, interval);

	delay = util_time_delay_for_compensation(interval) - interval;
	ecore_timer_delay(timer, delay);
}

HAPI char *util_get_file_kept_in_safe(const char *id)
{
	const char *path;
	char *new_path;
	int len;
	int base_idx;

	path = util_uri_to_path(id);
	if (!path) {
		ErrPrint("Invalid URI(%s)\n", id);
		return NULL;
	}

	/*!
	 * TODO: Remove me
	 */
	if (OVERWRITE_CONTENT)
		return strdup(path);

	len = strlen(path);
	base_idx = len - 1;

	while (base_idx > 0 && path[base_idx] != '/') base_idx--;
	base_idx += (path[base_idx] == '/');

	new_path = malloc(len + 10);
	if (!new_path) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	strncpy(new_path, path, base_idx);
	snprintf(new_path + base_idx, len + 10 - base_idx, "reader/%s", path + base_idx);
	return new_path;
}

HAPI int util_unlink_files(const char *folder)
{
	struct stat info;
	DIR *handle;
	struct dirent *entry;
	char *abspath;
	int len;

	if (lstat(folder, &info) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		return LB_STATUS_ERROR_IO;
	}

	if (!S_ISDIR(info.st_mode)) {
		ErrPrint("Error: %s is not a folder", folder);
		return LB_STATUS_ERROR_INVALID;
	}

	handle = opendir(folder);
	if (!handle) {
		ErrPrint("Error: %s\n", strerror(errno));
		return LB_STATUS_ERROR_IO;
	}

	while ((entry = readdir(handle))) {
		if (!strcmp(entry->d_name, "."))
			continue;

		if (!strcmp(entry->d_name, ".."))
			continue;

		len = strlen(folder) + strlen(entry->d_name) + 3;
		abspath = calloc(1, len);
		if (!abspath) {
			ErrPrint("Heap: %s\n", strerror(errno));
			continue;
		}
		snprintf(abspath, len - 1, "%s/%s", folder, entry->d_name);

		if (unlink(abspath) < 0)
			DbgPrint("unlink: %s - %s\n", abspath, strerror(errno));

		free(abspath);
	}

	closedir(handle);
	return LB_STATUS_SUCCESS;
}

/* End of a file */
