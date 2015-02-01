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
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <libgen.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#include <dlog.h>
#include <Eina.h>
#if defined(HAVE_LIVEBOX)
#include <dynamicbox_errno.h>
#include <dynamicbox_conf.h>
#else
#include "lite-errno.h"
#endif

#include "conf.h"
#include "debug.h"
#include "util.h"
#include "critical_log.h"

static struct {
	FILE *fp;
	int file_id;
	int nr_of_lines;
	char *filename;
	pthread_mutex_t cri_lock;
} s_info = {
	.fp = NULL,
	.file_id = 0,
	.nr_of_lines = 0,
	.filename = NULL,
	.cri_lock = PTHREAD_MUTEX_INITIALIZER,
};



static inline void rotate_log(void)
{
	char *filename;
	int namelen;

	if (s_info.nr_of_lines < DYNAMICBOX_CONF_MAX_LOG_LINE) {
		return;
	}

	s_info.file_id = (s_info.file_id + 1) % DYNAMICBOX_CONF_MAX_LOG_FILE;

	namelen = strlen(s_info.filename) + strlen(DYNAMICBOX_CONF_LOG_PATH) + 30;
	filename = malloc(namelen);
	if (filename) {
		snprintf(filename, namelen, "%s/%d_%s.%d", DYNAMICBOX_CONF_LOG_PATH, s_info.file_id, s_info.filename, getpid());

		if (s_info.fp) {
			if (fclose(s_info.fp) != 0) {
				ErrPrint("fclose: %s\n", strerror(errno));
			}
		}

		s_info.fp = fopen(filename, "w+");
		if (!s_info.fp) {
			ErrPrint("Failed to open a file: %s\n", filename);
		}

		DbgFree(filename);
	}

	s_info.nr_of_lines = 0;
}



HAPI int critical_log(const char *func, int line, const char *fmt, ...)
{
	va_list ap;
	int ret;

	if (!s_info.fp) {
		return DBOX_STATUS_ERROR_IO_ERROR;
	}

	CRITICAL_SECTION_BEGIN(&s_info.cri_lock);

	fprintf(s_info.fp, "%lf [%s:%d] ", util_timestamp(), util_basename((char *)func), line);

	va_start(ap, fmt);
	ret = vfprintf(s_info.fp, fmt, ap);
	va_end(ap);

	fflush(s_info.fp);

	s_info.nr_of_lines++;
	rotate_log();

	CRITICAL_SECTION_END(&s_info.cri_lock);
	return ret;
}



HAPI int critical_log_init(const char *name)
{
	int namelen;
	char *filename;

	if (s_info.fp) {
		return DBOX_STATUS_ERROR_NONE;
	}

	s_info.filename = strdup(name);
	if (!s_info.filename) {
		ErrPrint("Failed to create a log file\n");
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	namelen = strlen(name) + strlen(DYNAMICBOX_CONF_LOG_PATH) + 30;

	filename = malloc(namelen);
	if (!filename) {
		ErrPrint("Failed to create a log file\n");
		DbgFree(s_info.filename);
		s_info.filename = NULL;
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	snprintf(filename, namelen, "%s/%d_%s.%d", DYNAMICBOX_CONF_LOG_PATH, s_info.file_id, name, getpid());

	s_info.fp = fopen(filename, "w+");
	if (!s_info.fp) {
		ErrPrint("Failed to open log: %s\n", strerror(errno));
		DbgFree(s_info.filename);
		s_info.filename = NULL;
		DbgFree(filename);
		return DBOX_STATUS_ERROR_IO_ERROR;
	}

	DbgFree(filename);
	return DBOX_STATUS_ERROR_NONE;
}



HAPI void critical_log_fini(void)
{
	if (s_info.filename) {
		DbgFree(s_info.filename);
		s_info.filename = NULL;
	}

	if (s_info.fp) {
		if (fclose(s_info.fp) != 0) {
			ErrPrint("fclose: %s\n", strerror(errno));
		}
		s_info.fp = NULL;
	}
}



/* End of a file */
