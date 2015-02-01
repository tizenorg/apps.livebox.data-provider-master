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

#define _GNU_SOURCE

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
#include <sys/statvfs.h>
#include <sys/mount.h>

#include <sys/smack.h>
#include <dlog.h>
#include <Eina.h>
#include <Ecore.h>
#if defined(HAVE_LIVEBOX)
#include <dynamicbox_errno.h>
#include <dynamicbox_conf.h>
#else
#include "lite-errno.h"
#define DYNAMICBOX_CONF_IMAGE_PATH "/tmp/"
#endif

#include "util.h"
#include "debug.h"
#include "conf.h"

static struct info {
	int emergency_mounted;
} s_info = {
	.emergency_mounted = 0,
};

int errno;

HAPI unsigned long util_string_hash(const char *str)
{
	unsigned long ret = 0;

	while (*str) {
		ret += (unsigned long)(*str++);
	}

	ret %= 371773;
	return ret;
}

HAPI double util_timestamp(void)
{
#if defined(_USE_ECORE_TIME_GET)
	return ecore_time_get();
#else
	struct timeval tv;
	if (gettimeofday(&tv, NULL) < 0) {
		static unsigned long internal_count = 0;
		ErrPrint("failed to get time of day: %s\n", strerror(errno));
		tv.tv_sec = internal_count++;
		tv.tv_usec = 0;
	}

	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0f;
#endif
}

HAPI int util_check_ext(const char *filename, const char *check_ptr)
{
	int name_len;

	name_len = strlen(filename);
	while (--name_len >= 0 && *check_ptr) {
		if (filename[name_len] != *check_ptr) {
			return DBOX_STATUS_ERROR_INVALID_PARAMETER;
		}

		check_ptr ++;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int util_unlink(const char *filename)
{
	char *descfile;
	int desclen;
	int ret;

	if (!filename) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	desclen = strlen(filename) + 6; /* .desc */
	descfile = malloc(desclen);
	if (!descfile) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	ret = snprintf(descfile, desclen, "%s.desc", filename);
	if (ret < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		DbgFree(descfile);
		return DBOX_STATUS_ERROR_FAULT;
	}

	(void)unlink(descfile);
	DbgFree(descfile);
	(void)unlink(filename);

	return DBOX_STATUS_ERROR_NONE;
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
	if (!length) {
		return ".";
	}

	while (--length > 0 && name[length] != '/');

	return length <= 0 ? name : (name + length + (name[length] == '/'));
}

/*!
 * Return size of stroage in Bytes unit.
 */
HAPI unsigned long long util_free_space(const char *path)
{
	struct statvfs st;
	unsigned long long space;

	if (statvfs(path, &st) < 0) {
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
	int idx = 0;
	int out_idx;
	int out_sz;
	enum {
		STATE_START,
		STATE_FIND,
		STATE_CHECK,
		STATE_END
	} state;

	if (!src || !pattern) {
		return NULL;
	}

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
						DbgFree(ret);
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
						DbgFree(ret);
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
						DbgFree(ret);
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
	if (strncasecmp(uri, SCHEMA_FILE, len)) {
		return NULL;
	}

	return uri + len;
}

HAPI double util_time_delay_for_compensation(double period)
{
	unsigned long long curtime;
	unsigned long long _period;
	unsigned long long remain;
	struct timeval tv;
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
	return period - ret;
}

HAPI void *util_timer_add(double interval, Eina_Bool (*cb)(void *data), void *data)
{
	Ecore_Timer *timer;
	double delay;

	timer = ecore_timer_add(interval, cb, data);
	if (!timer) {
		return NULL;
	}

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

HAPI int util_unlink_files(const char *folder)
{
	struct stat info;
	DIR *handle;
	struct dirent *entry;
	char *abspath;
	int len;

	if (lstat(folder, &info) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_IO_ERROR;
	}

	if (!S_ISDIR(info.st_mode)) {
		ErrPrint("Error: %s is not a folder", folder);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	handle = opendir(folder);
	if (!handle) {
		ErrPrint("Error: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_IO_ERROR;
	}

	while ((entry = readdir(handle))) {
		if (!strcmp(entry->d_name, ".")) {
			continue;
		}

		if (!strcmp(entry->d_name, "..")) {
			continue;
		}

		len = strlen(folder) + strlen(entry->d_name) + 3;
		abspath = calloc(1, len);
		if (!abspath) {
			ErrPrint("Heap: %s\n", strerror(errno));
			continue;
		}
		snprintf(abspath, len - 1, "%s/%s", folder, entry->d_name);

		if (unlink(abspath) < 0) {
			DbgPrint("unlink: %s\n", strerror(errno));
		}

		DbgFree(abspath);
	}

	if (closedir(handle) < 0) {
		ErrPrint("closedir: %s\n", strerror(errno));
	}
	return DBOX_STATUS_ERROR_NONE;
}

HAPI void util_remove_emergency_disk(void)
{
	int ret;
	ret = umount(DYNAMICBOX_CONF_IMAGE_PATH);
	if (ret < 0) {
		ErrPrint("umount: %s\n", strerror(errno));
	}

	DbgPrint("Try to unmount[%s] %d\n", DYNAMICBOX_CONF_IMAGE_PATH, ret);
	s_info.emergency_mounted = 0;
}

HAPI void util_prepare_emergency_disk(void)
{
	char *buf;
	char *source = NULL;
	char *type = NULL;
	char *option = NULL;
	char *ptr;
	char *rollback_ptr;
	int tag_idx;
	int idx;
	int len;
	int ret;
	static const char *tag[] = {
		"source",
		"type",
		"option",
		NULL,
	};
	enum tag_type {
		TAG_SOURCE,
		TAG_TYPE,
		TAG_OPTION,
		TAG_ERROR
	};

	buf = strdup(DYNAMICBOX_CONF_EMERGENCY_DISK);
	if (!buf) {
		ErrPrint("Failed to prepare emergency disk info\n");
		return;
	}

	rollback_ptr = ptr = buf;
	idx = 0;
	tag_idx = 0;
	len = strlen(ptr);

	while (tag[tag_idx] != NULL && ptr != (buf + len)) {
		if (tag[tag_idx][idx] == '\0') {
			if (*ptr == '=' || isblank(*ptr)) {
				switch (tag_idx) {
				case TAG_SOURCE:
					if (source) {
						ErrPrint("source[%s] is overrided\n", source);
					}

					while ((*ptr != '\0' && *ptr != ';') && (*ptr == '=' || isblank(*ptr))) {
						ptr++;
					}

					source = ptr;
					while (*ptr != '\0' && *ptr != ';') {
						ptr++;
					}

					if (*source == '\0') {
						type = NULL;
					}

					*ptr = '\0';
					rollback_ptr = ptr + 1;
					idx = 0;
					break;
				case TAG_TYPE:
					if (type) {
						ErrPrint("type[%s] is overrided\n", type);
					}

					while ((*ptr != '\0' && *ptr != ';') && (*ptr == '=' || isblank(*ptr))) {
						ptr++;
					}

					type = ptr;
					while (*ptr != '\0' && *ptr != ';') {
						ptr++;
					}

					if (*type == '\0') {
						type = NULL;
					}

					*ptr = '\0';
					rollback_ptr = ptr + 1;
					idx = 0;
					break;
				case TAG_OPTION:
					if (option) {
						ErrPrint("option[%s] is overrided\n", option);
					}

					while ((*ptr != '\0' && *ptr != ';') && (*ptr == '=' || isblank(*ptr))) {
						ptr++;
					}

					option = ptr;
					while (*ptr != '\0' && *ptr != ';') {
						ptr++;
					}

					if (*option == '\0') {
						option = NULL;
					}

					*ptr = '\0';
					rollback_ptr = ptr + 1;
					idx = 0;
					break;
				default:
					break;
				}
			} else {
				ptr = rollback_ptr;
				tag_idx++;
				idx = 0;
			}
		} else if (tag[tag_idx][idx] != *ptr) {
			ptr = rollback_ptr;
			tag_idx++;
			idx = 0;
		} else {
			ptr++;
			idx++;
		} // tag
	}

	DbgPrint("source[%s] type[%s] option[%s]\n", source, type, option);

	ret = mount(source, DYNAMICBOX_CONF_IMAGE_PATH, type, MS_NOSUID | MS_NOEXEC, option);
	DbgFree(buf);
	if (ret < 0) {
		ErrPrint("Failed to mount: %s\n", strerror(errno));
		return;
	}

	ErrPrint("Disk space is not enough, use the tmpfs. Currently required minimum space is %lu bytes\n", DYNAMICBOX_CONF_MINIMUM_SPACE);
	if (chmod(DYNAMICBOX_CONF_IMAGE_PATH, 0750) < 0) {
		ErrPrint("chmod: %s\n", strerror(errno));
	}

	if (chown(DYNAMICBOX_CONF_IMAGE_PATH, 5000, 5000) < 0) {
		ErrPrint("chown: %s\n", strerror(errno));
	}

	ret = smack_setlabel(DYNAMICBOX_CONF_IMAGE_PATH, DATA_SHARE_LABEL, SMACK_LABEL_ACCESS);
	if (ret != 0) {
		ErrPrint("Failed to set SMACK for %s (%d)\n", DYNAMICBOX_CONF_IMAGE_PATH, ret);
	} else {
		ret = smack_setlabel(DYNAMICBOX_CONF_IMAGE_PATH, "1", SMACK_LABEL_TRANSMUTE);
		DbgPrint("[%s] is successfully created (t: %d)\n", DYNAMICBOX_CONF_IMAGE_PATH, ret);
	}

	if (mkdir(DYNAMICBOX_CONF_ALWAYS_PATH, 0755) < 0) {
		ErrPrint("mkdir: (%s) %s\n", DYNAMICBOX_CONF_ALWAYS_PATH, strerror(errno));
	} else {
		if (chmod(DYNAMICBOX_CONF_ALWAYS_PATH, 0750) < 0) {
			ErrPrint("chmod: %s\n", strerror(errno));
		}

		if (chown(DYNAMICBOX_CONF_ALWAYS_PATH, 5000, 5000) < 0) {
			ErrPrint("chown: %s\n", strerror(errno));
		}

		ret = smack_setlabel(DYNAMICBOX_CONF_ALWAYS_PATH, DATA_SHARE_LABEL, SMACK_LABEL_ACCESS);
		if (ret != 0) {
			ErrPrint("Failed to set SMACK for %s (%d)\n", DYNAMICBOX_CONF_ALWAYS_PATH, ret);
		} else {
			ret = smack_setlabel(DYNAMICBOX_CONF_ALWAYS_PATH, "1", SMACK_LABEL_TRANSMUTE);
			DbgPrint("[%s] is successfully created (t: %d)\n", DYNAMICBOX_CONF_ALWAYS_PATH, ret);
		}
	}

	if (mkdir(DYNAMICBOX_CONF_READER_PATH, 0755) < 0) {
		ErrPrint("mkdir: (%s) %s\n", DYNAMICBOX_CONF_READER_PATH, strerror(errno));
	} else {
		if (chmod(DYNAMICBOX_CONF_READER_PATH, 0750) < 0) {
			ErrPrint("chmod: %s\n", strerror(errno));
		}

		if (chown(DYNAMICBOX_CONF_READER_PATH, 5000, 5000) < 0) {
			ErrPrint("chown: %s\n", strerror(errno));
		}

		ret = smack_setlabel(DYNAMICBOX_CONF_READER_PATH, DATA_SHARE_LABEL, SMACK_LABEL_ACCESS);
		if (ret != 0) {
			ErrPrint("Failed to set SMACK for %s (%d)\n", DYNAMICBOX_CONF_READER_PATH, ret);
		} else {
			ret = smack_setlabel(DYNAMICBOX_CONF_READER_PATH, "1", SMACK_LABEL_TRANSMUTE);
			DbgPrint("[%s] is successfully created (t: %d)\n", DYNAMICBOX_CONF_READER_PATH, ret);
		}
	}

	s_info.emergency_mounted = 1;
}

HAPI int util_emergency_disk_is_mounted(void)
{
	return s_info.emergency_mounted;
}

HAPI void util_setup_log_disk(void)
{
	int ret;

	if (access(DYNAMICBOX_CONF_LOG_PATH, R_OK | W_OK | X_OK) == 0) {
		DbgPrint("[%s] is already accessible\n", DYNAMICBOX_CONF_LOG_PATH);
		return;
	}

	DbgPrint("Initiate the critical log folder [%s]\n", DYNAMICBOX_CONF_LOG_PATH);
	if (mkdir(DYNAMICBOX_CONF_LOG_PATH, 0755) < 0) {
		ErrPrint("mkdir: %s\n", strerror(errno));
	} else {
		if (chmod(DYNAMICBOX_CONF_LOG_PATH, 0750) < 0) {
			ErrPrint("chmod: %s\n", strerror(errno));
		}

		if (chown(DYNAMICBOX_CONF_LOG_PATH, 5000, 5000) < 0) {
			ErrPrint("chown: %s\n", strerror(errno));
		}

		ret = smack_setlabel(DYNAMICBOX_CONF_LOG_PATH, DATA_SHARE_LABEL, SMACK_LABEL_ACCESS);
		if (ret != 0) {
			ErrPrint("Failed to set SMACK for %s (%d)\n", DYNAMICBOX_CONF_LOG_PATH, ret);
		} else {
			ret = smack_setlabel(DYNAMICBOX_CONF_LOG_PATH, "1", SMACK_LABEL_TRANSMUTE);
			DbgPrint("[%s] is successfully created (t: %d)\n", DYNAMICBOX_CONF_LOG_PATH, ret);
		}
	}
}

HAPI int util_service_is_enabled(const char *tag)
{
	return !!strcasestr(DYNAMICBOX_CONF_SERVICES, tag);
}

/* End of a file */
