/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

struct liveinfo;

extern int liveinfo_init(void);
extern void liveinfo_fini(void);
extern struct liveinfo *liveinfo_create(pid_t pid, int handle);
extern void liveinfo_destroy(struct liveinfo *info);

extern struct liveinfo *liveinfo_find_by_pid(pid_t pid);
extern struct liveinfo *liveinfo_find_by_handle(int handle);

extern const char *liveinfo_filename(struct liveinfo *info);
extern pid_t liveinfo_pid(struct liveinfo *info);
extern FILE *liveinfo_fifo(struct liveinfo *info);
extern int liveinfo_open_fifo(struct liveinfo *info);
extern void liveinfo_close_fifo(struct liveinfo *info);

/* End of a file */
