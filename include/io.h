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

extern int io_init(void);
extern int io_fini(void);
extern int io_load_package_db(struct pkg_info *info);
extern char *io_livebox_pkgname(const char *pkgname);
extern int io_update_livebox_package(const char *pkgname, int (*cb)(const char *pkgid, const char *lbid, int prime, void *data), void *data);
extern int io_crawling_liveboxes(int (*cb)(const char *pkgid, const char *lbid, int prime, void *data), void *data);
extern int io_is_exists(const char *lbid);

/* End of a file */
