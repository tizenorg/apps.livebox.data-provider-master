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

struct cluster;
struct category;
struct pkg_info;
struct context_info;
struct context_item;
struct context_option;

extern struct cluster *group_create_cluster(const char *name);
extern struct cluster *group_find_cluster(const char *name);
extern int group_destroy_cluster(struct cluster *cluster);

extern struct category *group_create_category(struct cluster *cluster, const char *name);
extern struct category *group_find_category(struct cluster *cluster, const char *name);
extern int group_destroy_category(struct category *category);

extern const char * const group_category_name(struct category *category);
extern const char * const group_cluster_name(struct cluster *cluster);
extern const char *group_cluster_name_by_category(struct category *category);

extern int group_add_package(struct category *category, const char *pkgname);
extern int group_del_package(struct category *category, const char *pkgname);

extern int group_add_dynamicbox(const char *group, const char *pkgname);
extern int group_del_dynamicbox(const char *pkgname);

extern int group_init(void);
extern int group_fini(void);

extern struct context_info *group_create_context_info(struct category *category, const char *pkgname);
extern struct context_item *group_add_context_item(struct context_info *info, const char *ctx_item);
extern int group_add_option(struct context_item *item, const char *key, const char *value);
extern int group_destroy_context_info(struct context_info *info);

extern Eina_List * const group_context_info_list(struct category *category);
extern Eina_List * const group_context_item_list(struct context_info *info);
extern Eina_List * const group_context_option_list(struct context_item *item);
extern Eina_List * const group_cluster_list(void);
extern Eina_List * const group_category_list(struct cluster *cluster);
extern struct context_info * const group_context_info_from_item(struct context_item *item);
extern struct category * const group_category_from_context_info(struct context_info *info);
extern const char * const group_option_item_key(struct context_option *option);
extern const char * const group_option_item_value(struct context_option *option);
extern const char * const group_context_item(struct context_item *item);
extern const char * const group_pkgname_from_context_info(struct context_info *info);

extern void *group_context_item_del_data(struct context_item *item, const char *tag);
extern void *group_context_item_data(struct context_item *item, const char *tag);
extern int group_context_item_add_data(struct context_item *item, const char *tag, void *data);
/* End of a file */
