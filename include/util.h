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

extern unsigned long util_string_hash(const char *str);
extern double util_timestamp(void);
extern int util_check_ext(const char *filename, const char *check_ptr);
extern int util_validate_livebox_package(const char *pkgname);
extern int util_unlink(const char *filename);
extern int util_unlink_files(const char *folder);
extern char *util_slavename(void);
extern const char *util_basename(const char *name);
extern unsigned long long util_free_space(const char *path);
extern char *util_replace_string(const char *src, const char *pattern, const char *replace);
extern const char *util_uri_to_path(const char *uri);
extern void *util_timer_add(double interval, Eina_Bool (*cb)(void *data), void *data);
extern void util_timer_interval_set(void *timer, double interval);
extern double util_time_delay_for_compensation(double period);

#define SCHEMA_FILE	"file://"
#define SCHEMA_PIXMAP	"pixmap://"
#define SCHEMA_SHM	"shm://"

#define CRITICAL_SECTION_BEGIN(handle) \
do { \
	int ret; \
	ret = pthread_mutex_lock(handle); \
	if (ret != 0) \
		ErrPrint("Failed to lock: %s\n", strerror(ret)); \
} while (0)

#define CRITICAL_SECTION_END(handle) \
do { \
	int ret; \
	ret = pthread_mutex_unlock(handle); \
	if (ret != 0) \
		ErrPrint("Failed to unlock: %s\n", strerror(ret)); \
} while (0)

#define CANCEL_SECTION_BEGIN() do { \
	int ret; \
	ret = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); \
	if (ret != 0) \
		ErrPrint("Unable to set cancelate state: %s\n", strerror(ret)); \
} while (0)

#define CANCEL_SECTION_END() do { \
	int ret; \
	ret = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL); \
	if (ret != 0) \
		ErrPrint("Unable to set cancelate state: %s\n", strerror(ret)); \
} while (0)

#define CLOSE_PIPE(p)	do { \
	int status; \
	status = close(p[PIPE_READ]); \
	if (status < 0) \
		ErrPrint("close: %s\n", strerror(errno)); \
	status = close(p[PIPE_WRITE]); \
	if (status < 0) \
		ErrPrint("close: %s\n", strerror(errno)); \
} while (0)

#define PIPE_READ 0
#define PIPE_WRITE 1
#define PIPE_MAX 2

/* End of a file */
