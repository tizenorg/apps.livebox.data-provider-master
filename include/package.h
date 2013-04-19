/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.0 (the "License");
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

enum lb_type {
	LB_TYPE_NONE = 0x0,
	LB_TYPE_SCRIPT,
	LB_TYPE_FILE,
	LB_TYPE_TEXT,
	LB_TYPE_BUFFER,
};

enum pd_type {
	PD_TYPE_NONE = 0x0,
	PD_TYPE_SCRIPT,
	PD_TYPE_TEXT,
	PD_TYPE_BUFFER,
};

enum alter_type {
	ALTER_CREATE,
	ALTER_DESTROY,
};

struct pkg_info;
struct inst_info;
struct context_info;
struct slave_node;

/*!
 * \brief
 * Construction & Destruction
 */
extern struct pkg_info *package_create(const char *pkgname);
extern int package_destroy(struct pkg_info *info);
extern char *package_lb_pkgname(const char *pkgname);
extern int package_is_lb_pkgname(const char *pkgname);
extern struct pkg_info *package_find(const char *pkgname);
extern const char *package_find_by_secured_slave(struct slave_node *slave);
extern struct inst_info *package_find_instance_by_id(const char *pkgname, const char *id);
extern struct inst_info *package_find_instance_by_timestamp(const char *pkgname, double timestamp);
extern int package_dump_fault_info(struct pkg_info *info);
extern int package_set_fault_info(struct pkg_info *info, double timestamp, const char *filename, const char *function);
extern int package_get_fault_info(struct pkg_info *info, double *timestmap, const char **filename, const char **function);

/*!
 * \brief
 * Readonly functions
 */
extern const int const package_is_fault(const struct pkg_info *info);
extern struct slave_node * const package_slave(const struct pkg_info *info);
extern const int const package_timeout(const struct pkg_info *info);
extern const double const package_period(const struct pkg_info *info);
extern const int const package_secured(const struct pkg_info *info);
extern const char * const package_script(const struct pkg_info *info);
extern const char * const package_abi(const struct pkg_info *info);
extern const char * const package_lb_path(const struct pkg_info *info);
extern const char * const package_lb_group(const struct pkg_info *info);
extern const char * const package_pd_path(const struct pkg_info *info);
extern const char * const package_pd_group(const struct pkg_info *info);
extern const int const package_pinup(const struct pkg_info *info);
extern const char * const package_auto_launch(const struct pkg_info *info);
extern const unsigned int const package_size_list(const struct pkg_info *info);
extern const int const package_pd_width(const struct pkg_info *info);
extern const int const package_pd_height(const struct pkg_info *info);
extern const char * const package_name(const struct pkg_info *info);
extern const char * const package_libexec(struct pkg_info *info);
extern int package_network(struct pkg_info *info);
extern Eina_List *package_ctx_info(struct pkg_info *pkginfo);

extern int package_set_libexec(struct pkg_info *info, const char *libexec);
extern void package_set_pinup(struct pkg_info *info, int pinup);
extern void package_set_auto_launch(struct pkg_info *info, const char *auto_launch);
extern void package_set_size_list(struct pkg_info *info, unsigned int size_list);
extern void package_set_lb_type(struct pkg_info *info, enum lb_type type);
extern void package_set_pd_type(struct pkg_info *info, enum pd_type type);
extern int package_set_lb_group(struct pkg_info *info, const char *group);
extern int package_set_lb_path(struct pkg_info *info, const char *path);
extern int package_set_pd_group(struct pkg_info *info, const char *group);
extern int package_set_pd_path(struct pkg_info *info, const char *path);
extern int package_set_script(struct pkg_info *info, const char *script);
extern void package_set_secured(struct pkg_info *info, int secured);
extern void package_set_period(struct pkg_info *info, double period);
extern void package_set_timeout(struct pkg_info *info, int timeout);
extern void package_set_network(struct pkg_info *info, int network);
extern void package_set_pd_height(struct pkg_info *info, int height);
extern void package_set_pd_width(struct pkg_info *info, int width);
extern int package_set_abi(struct pkg_info *info, const char *abi);
extern void package_add_ctx_info(struct pkg_info *pkginfo, struct context_info *info);

/*!
 * \brief
 * Reference counter
 */
extern struct pkg_info * const package_ref(struct pkg_info *info);
extern struct pkg_info * const package_unref(struct pkg_info *info);
extern const int const package_refcnt(const struct pkg_info *info);

extern const enum pd_type const package_pd_type(const struct pkg_info *info);
extern const enum lb_type const package_lb_type(const struct pkg_info *info);

extern int package_add_instance(struct pkg_info *info, struct inst_info *inst);
extern int package_del_instance(struct pkg_info *info, struct inst_info *inst);
extern Eina_List *package_instance_list(struct pkg_info *info);

extern int package_clear_fault(struct pkg_info *info);
extern int package_alter_instances_to_client(struct client_node *client, enum alter_type alter);

extern const Eina_List *package_list(void);
extern int const package_fault_count(struct pkg_info *info);

extern int package_init(void);
extern int package_fini(void);

/* End of a file */
