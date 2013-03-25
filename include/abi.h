/*
 * Copyright 2013  Samsung Electronics Co., Ltd
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

extern int abi_add_entry(const char *abi, const char *pkgname);
extern int abi_update_entry(const char *abi, const char *pkgname);
extern int abi_del_entry(const char *abi);
extern const char *abi_find_slave(const char *abi);
extern void abi_del_all(void);
extern const char *abi_find_by_pkgname(const char *pkgname);

/* End of a file */
