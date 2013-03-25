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

#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>

#include <dlog.h>
#include <Eina.h>
#include <sqlite3.h>
#include <db-util.h>
#include <livebox-errno.h>

#include "debug.h"
#include "conf.h"
#include "parser.h"
#include "group.h"
#include "util.h"
#include "client_life.h"
#include "slave_life.h"
#include "package.h"
#include "abi.h"
#include "io.h"

int errno;

static struct {
	sqlite3 *handle;
} s_info = {
	.handle = NULL,
};

static inline int load_abi_table(void)
{
	FILE *fp;
	int ch;
	int idx;
	int tag_id;
	enum {
		INIT = 0x0,
		GROUP = 0x1,
		TAG = 0x02,
		VALUE = 0x03,
		ERROR = 0x05,
	} state;
	enum {
		PKGNAME = 0x0,
	};
	static const char *field[] = {
		"package",
		NULL,
	};
	const char *ptr;

	char group[MAX_ABI + 1];
	char pkgname[MAX_PKGNAME + 1];

	fp = fopen("/usr/share/"PACKAGE"/abi.ini", "rt");
	if (!fp)
		return LB_STATUS_ERROR_IO;

	state = INIT;
	while ((ch = getc(fp)) != EOF && state != ERROR) {
		switch (state) {
		case INIT:
			if (isspace(ch))
				continue;
			if (ch == '[') {
				state = GROUP;
				idx = 0;
			} else {
				state = ERROR;
			}
			break;
		case GROUP:
			if (ch == ']') {
				if (idx == 0) {
					state = ERROR;
				} else {
					group[idx] = '\0';
					DbgPrint("group: %s\n", group);
					state = TAG;
					idx = 0;
					ptr = NULL;
				}
			} else if (idx < MAX_ABI) {
				group[idx++] = ch;
			} else {
				ErrPrint("Overflow\n");
				state = ERROR;
			}
			break;
		case TAG:
			if (ptr == NULL) {
				if (idx == 0) {
					if (isspace(ch))
						continue;

					/* New group started */
					if (ch == '[') {
						ungetc(ch, fp);
						state = INIT;
						continue;
					}
				}

				ptr = field[idx];
			}

			if (ptr == NULL) {
				ErrPrint("unknown tag\n");
				state = ERROR;
				continue;
			}

			if (*ptr == '\0' && ch == '=') {
				/* MATCHED */
				state = VALUE;
				tag_id = idx;
				idx = 0;
				ptr = NULL;
				DbgPrint("tag: %s\n", field[tag_id]);
			} else if (*ptr == ch) {
				ptr++;
			} else {
				ungetc(ch, fp);
				ptr--;
				while (ptr >= field[idx]) {
					ungetc(*ptr, fp);
					ptr--;
				}
				ptr = NULL;
				idx++;
			}
			break;
		case VALUE:
			switch (tag_id) {
			case PKGNAME:
				if (idx == 0) { /* LTRIM */
					if (isspace(ch))
						continue;

					pkgname[idx] = ch;
					idx++;
				} else if (isspace(ch)) {
					int ret;
					pkgname[idx] = '\0';
					DbgPrint("value: %s\n", pkgname);

					DbgPrint("Add [%s] - [%s]\n", group, pkgname);
					ret = abi_add_entry(group, pkgname);
					if (ret != 0)
						ErrPrint("Failed to add %s for %s\n", pkgname, group);

					state = TAG;
					idx = 0;
				} else if (idx < MAX_PKGNAME) {
					pkgname[idx] = ch;
					idx++;
				} else {
					ErrPrint("Overflow\n");
					state = ERROR;
				}
				break;
			default:
				break;
			}
			break;
		case ERROR:
		default:
			break;
		}
	}

	if (state == VALUE) {
		switch (tag_id) {
		case PKGNAME:
			if (idx) {
				int ret;
				pkgname[idx] = '\0';
				DbgPrint("value: %s\n", pkgname);
				DbgPrint("Add [%s] - [%s]\n", group, pkgname);
				ret = abi_add_entry(group, pkgname);
				if (ret != 0)
					ErrPrint("Failed to add %s for %s\n", pkgname, group);
			}
			break;
		default:
			break;
		}
	}

	fclose(fp);
	return LB_STATUS_SUCCESS;
}

static inline int build_client_info(struct pkg_info *info)
{
	static const char *dml = "SELECT auto_launch, pd_size FROM client WHERE pkgid = ?";
	sqlite3_stmt *stmt;
	int width;
	int height;
	int ret;
	const char *tmp;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return LB_STATUS_ERROR_IO;
	}

	ret = sqlite3_bind_text(stmt, 1, package_name(info), -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to bind a pkgname %s\n", package_name(info));
		sqlite3_finalize(stmt);
		return LB_STATUS_ERROR_IO;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("%s has no records (%s)\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_finalize(stmt);
		return LB_STATUS_ERROR_IO;
	}

	package_set_auto_launch(info, (const char *)sqlite3_column_text(stmt, 0));

	tmp = (const char *)sqlite3_column_text(stmt, 1);
	if (tmp && strlen(tmp)) {
		if (sscanf(tmp, "%dx%d", &width, &height) != 2) {
			ErrPrint("Failed to get PD width and Height (%s)\n", tmp);
		} else {
			package_set_pd_width(info, width);
			package_set_pd_height(info, height);
		}
	}

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return LB_STATUS_SUCCESS;
}

static inline int build_provider_info(struct pkg_info *info)
{
	static const char *dml = "SELECT provider.network, provider.abi, provider.secured, provider.box_type, provider.box_src, provider.box_group, provider.pd_type, provider.pd_src, provider.pd_group, provider.libexec, provider.timeout, provider.period, provider.script, provider.pinup, pkgmap.appid FROM provider, pkgmap WHERE pkgmap.pkgid = ? AND provider.pkgid = ?";
	sqlite3_stmt *stmt;
	int ret;
	const char *tmp;
	const char *appid;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return LB_STATUS_ERROR_IO;
	}

	if (sqlite3_bind_text(stmt, 1, package_name(info), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
		ErrPrint("Failed to bind a pkgname(%s) - %s\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_finalize(stmt);
		return LB_STATUS_ERROR_IO;
	}

	if (sqlite3_bind_text(stmt, 2, package_name(info), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
		ErrPrint("Failed to bind a pkgname(%s) - %s\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_finalize(stmt);
		return LB_STATUS_ERROR_IO;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("%s has no record(%s)\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_finalize(stmt);
		return LB_STATUS_ERROR_IO;
	}

	appid = (const char *)sqlite3_column_text(stmt, 14);
	if (!appid || !strlen(appid)) {
		ErrPrint("Failed to execute the DML for %s\n", package_name(info));
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		sqlite3_finalize(stmt);
		return LB_STATUS_ERROR_IO;
	}

	package_set_network(info, sqlite3_column_int(stmt, 0));
	package_set_secured(info, sqlite3_column_int(stmt, 2));

	tmp = (const char *)sqlite3_column_text(stmt, 1);
	if (tmp && strlen(tmp))
		package_set_abi(info, tmp);

	package_set_lb_type(info, sqlite3_column_int(stmt, 3));
	tmp = (const char *)sqlite3_column_text(stmt, 4);
	if (tmp && strlen(tmp)) {
		package_set_lb_path(info, tmp);
		DbgPrint("LB Path: %s\n", tmp);

		tmp = (const char *)sqlite3_column_text(stmt, 5);
		if (tmp && strlen(tmp))
			package_set_lb_group(info, tmp);
	}

	package_set_pd_type(info, sqlite3_column_int(stmt, 6));
	tmp = (const char *)sqlite3_column_text(stmt, 7);
	if (tmp && strlen(tmp)) {
		package_set_pd_path(info, tmp);
		DbgPrint("PD Path: %s\n", tmp);

		tmp = (const char *)sqlite3_column_text(stmt, 8);
		if (tmp && strlen(tmp))
			package_set_pd_group(info, tmp);
	}

	tmp = (const char *)sqlite3_column_text(stmt, 9);
	if (tmp && strlen(tmp))
		package_set_libexec(info, tmp);

	package_set_timeout(info, sqlite3_column_int(stmt, 10));

	tmp = (const char *)sqlite3_column_text(stmt, 11);
	if (tmp && strlen(tmp))
		package_set_period(info, atof(tmp));

	tmp = (const char *)sqlite3_column_text(stmt, 12);
	if (tmp && strlen(tmp))
		package_set_script(info, tmp);
	package_set_pinup(info, sqlite3_column_int(stmt, 13));

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return LB_STATUS_SUCCESS;
}

static inline int build_box_size_info(struct pkg_info *info)
{
	static const char *dml = "SELECT size_type FROM box_size WHERE pkgid = ?";
	sqlite3_stmt *stmt;
	int ret;
	unsigned int size_type;
	unsigned int size_list;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return LB_STATUS_ERROR_IO;
	}

	if (sqlite3_bind_text(stmt, 1, package_name(info), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
		ErrPrint("Failed to bind a pkgname(%s) - %s\n", package_name(info), sqlite3_errmsg(s_info.handle));
		sqlite3_finalize(stmt);
		return LB_STATUS_ERROR_IO;
	}

	size_list = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		size_type = sqlite3_column_int(stmt, 0);
		size_list |= size_type;
	}

	package_set_size_list(info, size_list);

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return LB_STATUS_SUCCESS;
}

static inline int load_context_option(struct context_item *item, int id)
{
	static const char *dml = "SELECT key, value FROM option WHERE option_id = ?";
	sqlite3_stmt *stmt;
	const char *key;
	const char *value;
	int ret;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return LB_STATUS_ERROR_IO;
	}

	ret = sqlite3_bind_int(stmt, 1, id);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = LB_STATUS_ERROR_IO;
		goto out;
	}

	ret = LB_STATUS_ERROR_NOT_EXIST;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		key = (const char *)sqlite3_column_text(stmt, 0);
		if (!key || !strlen(key)) {
			ErrPrint("KEY is nil\n");
			continue;
		}

		value = (const char *)sqlite3_column_text(stmt, 1);
		if (!value || !strlen(value)) {
			ErrPrint("VALUE is nil\n");
			continue;
		}

		ret = group_add_option(item, key, value);
		if (ret < 0)
			break;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int load_context_item(struct context_info *info, int id)
{
	static const char *dml = "SELECT ctx_item, option_id FROM groupmap WHERE id = ?";
	struct context_item *item;
	sqlite3_stmt *stmt;
	const char *ctx_item;
	int option_id;
	int ret;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return LB_STATUS_ERROR_IO;
	}

	ret = sqlite3_bind_int(stmt, 1, id);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = LB_STATUS_ERROR_IO;
		goto out;
	}

	ret = LB_STATUS_ERROR_NOT_EXIST;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		ctx_item = (const char *)sqlite3_column_text(stmt, 0);
		option_id = sqlite3_column_int(stmt, 1);

		item = group_add_context_item(info, ctx_item);
		if (!item) {
			ErrPrint("Failed to add a new context item\n");
			ret = LB_STATUS_ERROR_FAULT;
			break;
		}

		ret = load_context_option(item, option_id);
		if (ret < 0)
			break;
	}

out:
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

static inline int build_group_info(struct pkg_info *info)
{
	static const char *dml = "SELECT id, cluster, category FROM groupinfo WHERE pkgid = ?";
	sqlite3_stmt *stmt;
	int ret;
	int id;
	const char *cluster_name;
	const char *category_name;
	struct cluster *cluster;
	struct category *category;
	struct context_info *ctx_info;

	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return LB_STATUS_ERROR_IO;
	}

	ret = sqlite3_bind_text(stmt, 1, package_name(info), -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to bind a package name(%s)\n", package_name(info));
		sqlite3_finalize(stmt);
		return LB_STATUS_ERROR_IO;
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		id = sqlite3_column_int(stmt, 0);
		cluster_name = (const char *)sqlite3_column_text(stmt, 1);
		if (!cluster_name || !strlen(cluster_name)) {
			DbgPrint("Cluster name is not valid\n");
			continue;
		}

		category_name = (const char *)sqlite3_column_text(stmt, 2);
		if (!category_name || !strlen(category_name)) {
			DbgPrint("Category name is not valid\n");
			continue;
		}

		cluster = group_find_cluster(cluster_name);
		if (!cluster) {
			cluster = group_create_cluster(cluster_name);
			if (!cluster) {
				ErrPrint("Failed to create a cluster(%s)\n", cluster_name);
				continue;
			}
		}

		category = group_find_category(cluster, category_name);
		if (!category) {
			category = group_create_category(cluster, category_name);
			if (!category) {
				ErrPrint("Failed to create a category(%s)\n", category_name);
				continue;
			}
		}

		/*!
		 * \TODO
		 * Step 1. Get the list of the context item from the DB using 'id'
		 *         {context_item, option_id}
		 * Step 2. Get the list of the options from the DB using option_id
		 *         key, value
		 */
		ctx_info = group_create_context_info(category, package_name(info));
		if (ctx_info) {
			ret = load_context_item(ctx_info, id);
			if (ret < 0) {
				if (ret == LB_STATUS_ERROR_NOT_EXIST) {
					DbgPrint("Has no specific context info\n");
				} else {
					DbgPrint("Context info is not valid\n");
					group_destroy_context_info(ctx_info);
					ctx_info = NULL;
				}
			}

			if (ctx_info)
				package_add_ctx_info(info, ctx_info);
		}
	}

	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	sqlite3_finalize(stmt);
	return LB_STATUS_SUCCESS;
}

HAPI int io_is_exists(const char *pkgname) /* Manifest Package Name */
{
	sqlite3_stmt *stmt;
	int ret;

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
		return LB_STATUS_ERROR_IO;
	}

	ret = sqlite3_prepare_v2(s_info.handle, "SELECT COUNT(pkgid) FROM pkgmap WHERE appid = ?", -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return LB_STATUS_ERROR_IO;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgname, -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = LB_STATUS_ERROR_IO;
		goto out;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("%s has no record (%s)\n", pkgname, sqlite3_errmsg(s_info.handle));
		ret = LB_STATUS_ERROR_IO;
		goto out;
	}

	ret = sqlite3_column_int(stmt, 0);
out:
	sqlite3_reset(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

HAPI char *io_livebox_pkgname(const char *pkgname)
{
	sqlite3_stmt *stmt;
	char *pkgid;
	char *tmp;
	int ret;

	pkgid = NULL;

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
		return NULL;
	}

	ret = sqlite3_prepare_v2(s_info.handle, "SELECT pkgid FROM pkgmap WHERE (appid = ? AND prime = 1) OR pkgid = ?", -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return NULL;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgname, -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	ret = sqlite3_bind_text(stmt, 2, pkgname, -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		goto out;
	}

	if (sqlite3_step(stmt) != SQLITE_ROW) {
		ErrPrint("%s has no record (%s)\n", pkgname, sqlite3_errmsg(s_info.handle));
		goto out;
	}

	tmp = (char *)sqlite3_column_text(stmt, 0);
	if (tmp && strlen(tmp)) {
		pkgid = strdup(tmp);
		if (!pkgid)
			ErrPrint("Heap: %s\n", strerror(errno));
	}

out:
	sqlite3_reset(stmt);
	sqlite3_finalize(stmt);
	return pkgid;
}

HAPI int io_crawling_liveboxes(int (*cb)(const char *pkgname, int prime, void *data), void *data)
{
	DIR *dir;

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
	} else {
		int ret;
		sqlite3_stmt *stmt;

		ret = sqlite3_prepare_v2(s_info.handle, "SELECT pkgid, prime FROM pkgmap", -1, &stmt, NULL);
		if (ret != SQLITE_OK) {
			ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		} else {
			const char *pkgid;
			int prime;

			while (sqlite3_step(stmt) == SQLITE_ROW) {
				pkgid = (const char *)sqlite3_column_text(stmt, 0);
				if (!pkgid || !strlen(pkgid))
					continue;

				prime = (int)sqlite3_column_int(stmt, 1);
				if (cb(pkgid, prime, data) < 0) {
					sqlite3_reset(stmt);
					sqlite3_finalize(stmt);
					return LB_STATUS_ERROR_CANCEL;
				}
			}

			sqlite3_reset(stmt);
			sqlite3_finalize(stmt);
		}
	}

	dir = opendir(ROOT_PATH);
	if (!dir) {
		ErrPrint("Error: %s\n", strerror(errno));
	} else {
		struct dirent *ent;

		while ((ent = readdir(dir))) {
			if (ent->d_name[0] == '.')
				continue;

			if (cb(ent->d_name, -1, data) < 0) {
				closedir(dir);
				return LB_STATUS_ERROR_CANCEL;
			}
		}

		closedir(dir);
	}

	return LB_STATUS_SUCCESS;
}

HAPI int io_update_livebox_package(const char *pkgname, int (*cb)(const char *lb_pkgname, int prime, void *data), void *data)
{
	sqlite3_stmt *stmt;
	char *pkgid;
	int prime;
	int ret;

	if (!cb || !pkgname)
		return LB_STATUS_ERROR_INVALID;

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
		return LB_STATUS_ERROR_INVALID;
	}

	ret = sqlite3_prepare_v2(s_info.handle, "SELECT pkgid, prime FROM pkgmap WHERE appid = ?", -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		return LB_STATUS_ERROR_FAULT;
	}

	ret = sqlite3_bind_text(stmt, 1, pkgname, -1, SQLITE_TRANSIENT);
	if (ret != SQLITE_OK) {
		ErrPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
		ret = LB_STATUS_ERROR_FAULT;
		goto out;
	}

	ret = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		pkgid = (char *)sqlite3_column_text(stmt, 0);
		if (!pkgid || !strlen(pkgid))
			continue;

		prime = sqlite3_column_int(stmt, 1);

		if (cb(pkgid, prime, data) < 0) {
			DbgPrint("Callback canceled\n");
			break;
		}

		ret++;
	}
out:
	sqlite3_reset(stmt);
	sqlite3_finalize(stmt);
	return ret;
}

HAPI int io_load_package_db(struct pkg_info *info)
{
	int ret;

	if (!s_info.handle) {
		ErrPrint("DB is not ready\n");
		return LB_STATUS_ERROR_IO;
	}

	ret = build_provider_info(info);
	if (ret < 0)
		return ret;

	ret = build_client_info(info);
	if (ret < 0)
		return ret;

	ret = build_box_size_info(info);
	if (ret < 0)
		return ret;

	ret = build_group_info(info);
	if (ret < 0)
		return ret;

	return LB_STATUS_SUCCESS;
}

static inline int db_init(void)
{
	int ret;
	struct stat stat;

	ret = db_util_open(DBFILE, &s_info.handle, DB_UTIL_REGISTER_HOOK_METHOD);
	if (ret != SQLITE_OK) {
		ErrPrint("Failed to open a DB\n");
		return LB_STATUS_ERROR_IO;
	}

	if (lstat(DBFILE, &stat) < 0) {
		db_util_close(s_info.handle);
		s_info.handle = NULL;
		ErrPrint("%s\n", strerror(errno));
		return LB_STATUS_ERROR_IO;
	}

	if (!S_ISREG(stat.st_mode)) {
		ErrPrint("Invalid file\n");
		db_util_close(s_info.handle);
		s_info.handle = NULL;
		return LB_STATUS_ERROR_INVALID;
	}

	if (stat.st_size <= 0)
		DbgPrint("Size is %d (But use this ;)\n", stat.st_size);

	return LB_STATUS_SUCCESS;
}

static inline int db_fini(void)
{
	if (!s_info.handle)
		return LB_STATUS_SUCCESS;

	db_util_close(s_info.handle);
	s_info.handle = NULL;

	return LB_STATUS_SUCCESS;
}

HAPI int io_init(void)
{
	int ret;

	ret = db_init();
	DbgPrint("DB initialized: %d\n", ret);

	ret = load_abi_table();
	DbgPrint("ABI table is loaded: %d\n", ret);

	return LB_STATUS_SUCCESS;
}

HAPI int io_fini(void)
{
	int ret;

	abi_del_all();

	ret = db_fini();
	DbgPrint("DB finalized: %d\n", ret);
	return LB_STATUS_SUCCESS;
}

/* End of a file */
