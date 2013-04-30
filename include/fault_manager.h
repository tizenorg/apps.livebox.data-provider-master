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

extern int fault_check_pkgs(struct slave_node *node);
extern int fault_func_call(struct slave_node *node, const char *pkgname, const char *filename, const char *func);
extern int fault_func_ret(struct slave_node *node, const char *pkgname, const char *filename, const char *func);
extern int const fault_is_occured(void);
extern void fault_unicast_info(struct client_node *client, const char *pkgname, const char *filename, const char *func);
extern void fault_broadcast_info(const char *pkgname, const char *filename, const char *func);
extern int fault_info_set(struct slave_node *slave, const char *pkgname, const char *id, const char *func);

/* End of a file */
