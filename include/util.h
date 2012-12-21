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

extern unsigned long util_string_hash(const char *str);
extern double util_timestamp(void);
extern int util_check_ext(const char *filename, const char *check_ptr);
extern int util_validate_livebox_package(const char *pkgname);
extern int util_unlink(const char *filename);
extern char *util_slavename(void);
extern const char *util_basename(const char *name);
extern unsigned long util_free_space(const char *path);
extern char *util_replace_string(const char *src, const char *pattern, const char *replace);
extern const char *util_uri_to_path(const char *uri);
extern void *util_timer_add(double interval, Eina_Bool (*cb)(void *data), void *data);
extern void util_timer_interval_set(void *timer, double interval);
extern char *util_get_file_kept_in_safe(const char *id);

#define SCHEMA_FILE	"file://"
#define SCHEMA_PIXMAP	"pixmap://"
#define SCHEMA_SHM	"shm://"

/* End of a file */
