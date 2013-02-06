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
#include <sys/time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <dirent.h>

#include <dlog.h>
#include <Eina.h>
#include <Ecore.h>

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
			return -EINVAL;

		check_ptr ++;
	}

	return 0;
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
		return -ENOMEM;
	}

	snprintf(path, len, "%s%s/libexec/liblive-%s.so", ROOT_PATH, pkgname, pkgname);
	if (access(path, F_OK | R_OK) != 0) {
		ErrPrint("%s is not a valid package\n", pkgname);
		DbgFree(path);
		return -EINVAL;
	}

	DbgFree(path);
	return 0;
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
		return -ENOMEM;
	}

	snprintf(path, len, "/opt/usr/apps/%s/res/wgt/livebox/index.html", pkgname);
	if (access(path, F_OK | R_OK) != 0) {
		ErrPrint("%s is not a valid package\n", pkgname);
		DbgFree(path);
		return -EINVAL;
	}

	DbgFree(path);
	return 0;
}

HAPI int util_validate_livebox_package(const char *pkgname)
{
	if (!pkgname) {
		ErrPrint("Invalid argument\n");
		return -EINVAL;
	}

	if (!check_native_livebox(pkgname) || !check_web_livebox(pkgname))
		return 0;

	return -EINVAL;
}

HAPI int util_unlink(const char *filename)
{
	char *descfile;
	int desclen;
	int ret;

	desclen = strlen(filename) + 6; /* .desc */
	descfile = malloc(desclen);
	if (!descfile) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	ret = snprintf(descfile, desclen, "%s.desc", filename);
	if (ret < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		DbgFree(descfile);
		return -EFAULT;
	}

	(void)unlink(descfile);
	DbgFree(descfile);
	(void)unlink(filename);

	return 0;
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

HAPI unsigned long util_free_space(const char *path)
{
	struct statvfs st;
	unsigned long space;

	if (statvfs(path, &st) < 0) {
		ErrPrint("statvfs: %s\n", strerror(errno));
		return 0lu;
	}

	space = st.f_bsize * st.f_bfree;
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
	char *ret;
	int src_idx;
	int pattern_idx;
	int src_rollback_idx;
	int ret_rollback_idx;
	int ret_idx;
	int target_idx;
	int bufsz;
	int incsz;
	int matched;

	bufsz = strlen(src);
	incsz = bufsz + 1; /* Including the NULL byte */
	ret = malloc(bufsz + 1);
	if (!ret) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	pattern_idx = 0;
	ret_idx = 0;
	matched = 0;
	for (src_idx = 0; src[src_idx]; src_idx++) {
		if (!pattern[pattern_idx]) {
			while (replace[target_idx]) {
				ret[ret_idx] = replace[target_idx];
				ret_idx++;
				target_idx++;

				if (ret_idx >= bufsz) {
					char *tmp;

					tmp = extend_heap(ret, &bufsz, incsz);
					if (!tmp) {
						ErrPrint("Heap: %s\n", strerror(errno));
						DbgFree(ret);
						return NULL;
					}
					ret = tmp;
				}
			}

			pattern_idx = 0;
			src--;
			matched++;
			continue;
		} else if (src[src_idx] == pattern[pattern_idx]) {
			if (pattern_idx == 0) {
				src_rollback_idx = src_idx;
				ret_rollback_idx = ret_idx;
				target_idx = 0;
			}

			if (replace[target_idx]) {
				ret[ret_idx] = replace[target_idx];
				ret_idx++;
				target_idx++;
				if (ret_idx >= bufsz) {
					char *tmp;

					tmp = extend_heap(ret, &bufsz, incsz);
					if (!tmp) {
						ErrPrint("Heap: %s\n", strerror(errno));
						DbgFree(ret);
						return NULL;
					}

					ret = tmp;
				}
			}

			pattern_idx++;
			continue;
		} else if (pattern_idx > 0) {
			src_idx = src_rollback_idx;
			ret_idx = ret_rollback_idx;
			pattern_idx = 0;
		}

		ret[ret_idx] = src[src_idx];
		ret_idx++;
		if (ret_idx >= bufsz) {
			char *tmp;

			tmp = extend_heap(ret, &bufsz, incsz);
			if (!tmp) {
				ErrPrint("Heap: %s\n", strerror(errno));
				DbgFree(ret);
				return NULL;
			}
			ret = tmp;
		}
	}

	if (matched || !pattern[pattern_idx]) {
		if (target_idx > 0) {
			while (replace[target_idx]) {
				if (ret_idx >= bufsz) {
					char *tmp;
					tmp = extend_heap(ret, &bufsz, incsz);
					if (!tmp) {
						ErrPrint("Heap: %s\n", strerror(errno));
						DbgFree(ret);
						return NULL;
					}

					ret = tmp;
				}

				ret[ret_idx] = replace[target_idx];
				ret_idx++;
				target_idx++;
			}
		}
		ret[ret_idx] = '\0';
	} else {
		DbgFree(ret);
		ret = NULL;
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

static inline void compensate_timer(Ecore_Timer *timer)
{
	struct timeval tv;
	struct timeval compensator;
	double delay;
	double pending;

	if (ecore_timer_interval_get(timer) <= 1.0f) {
		DbgPrint("Doesn't need to sync the timer to start from ZERO sec\n");
		return;
	}

	if (gettimeofday(&tv, NULL) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		return;
	}

	compensator.tv_sec = tv.tv_sec % 60;
	if (compensator.tv_sec == 0)
		compensator.tv_sec = 59;

	delay = 60.0f - ((double)compensator.tv_sec + ((double)tv.tv_usec / 1000000.0f));
	pending = ecore_timer_pending_get(timer);
	ecore_timer_delay(timer, delay - pending);
	DbgPrint("COMPENSATED: %lf\n", delay);
}

HAPI void *util_timer_add(double interval, Eina_Bool (*cb)(void *data), void *data)
{
	Ecore_Timer *timer;

	timer = ecore_timer_add(interval, cb, data);
	if (!timer)
		return NULL;

	compensate_timer(timer);
	return timer;
}

HAPI void util_timer_interval_set(void *timer, double interval)
{
	ecore_timer_interval_set(timer, interval);
	compensate_timer(timer);
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
		return -EIO;
	}

	if (!S_ISDIR(info.st_mode)) {
		ErrPrint("Error: %s is not a folder", folder);
		return -EINVAL;
	}

	handle = opendir(folder);
	if (!handle) {
		ErrPrint("Error: %s\n", strerror(errno));
		return -EIO;
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
	return 0;
}

/* End of a file */
