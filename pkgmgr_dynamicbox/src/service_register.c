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
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <string.h>

#include <sqlite3.h>
#include <db-util.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <dlog.h>

#include <dynamicbox_service.h>

#include "dlist.h"

#if !defined(FLOG)
#define DbgPrint(format, arg...)	SECURE_LOGD("[[32m%s/%s[0m:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg)
#define ErrPrint(format, arg...)	SECURE_LOGE("[[32m%s/%s[0m:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg)
#define ErrPrintWithConsole(format, arg...)	do { fprintf(stderr, "[%s/%s:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg); SECURE_LOGE("[[32m%s/%s[0m:%d] " format, basename(__FILE__), __func__, __LINE__, ##arg); } while (0)
#endif

#define CUR_VER 5
#define DEFAULT_CATEGORY	"http://tizen.org/category/default"

/*!
 * \note
 * DB Table schema
 *
 * version
 * +---------+
 * | version |
 * +---------+
 * |   -     |
 * +---------+
 * CREATE TABLE version ( version INTEGER )
 * 
 *
 * pkgmap
 * +-------+-------+-------+-------+-------------------+
 * | appid | pkgid | uiapp | prime | categ(from ver 2) |
 * +-------+-------+-------+-------+-------------------+
 * |   -   |   -   |   -   |   -   |         -         |
 * +-------+-------+-------+-------+-------------------+
 * CREATE TABLE pkgmap ( pkgid TEXT PRIMARY KEY NOT NULL, appid TEXT, uiapp TEXT, prime INTEGER, category TEXT )
 *
 *
 * provider
 * +-------+---------+-----+---------+----------+---------+-----------+---------+--------+----------+---------+---------+--------+--------+-------+-----------------------+
 * | pkgid | network | abi | secured | box_type | box_src | box_group | gbar_type | gbar_src | gbar_group | libexec | timeout | period | script | pinup | count(from ver 4) | direct_input | hw_acceleration |
 * +-------+---------+-----+---------+----------+---------+-----------+---------+--------+----------+---------+---------+--------+--------+-------+-----------------------+-------|---------------|
 * |   -   |    -    |  -  |    -    |     -    |    -    |     -     |    -    |    -   |     -    |     -   |    -    |    -   |    -   |   -   |           -           |   -   |       -       |
 * +-------+---------+-----+---------+----------+---------+-----------+---------+--------+----------+---------+---------+--------+--------+-------+-----------------------+-------|---------------|
 * CREATE TABLE provider ( pkgid TEXT PRIMARY KEY NOT NULL, network INTEGER, abi TEXT, secured INTEGER, box_type INTEGER, box_src TEXT, box_group TEXT, gbar_type TEXT, gbar_src TEXT, gbar_group TEXT, libexec TEXT, timeout INTEGER, period TEXT, script TEXT, pinup INTEGER, count INTEGER, direct_input INTEGER DEFAULT 0, hw_acceleration TEXT DEFAULT 'none', FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid))
 *
 * = box_type = { text | buffer | script | image }
 * = gbar_type = { text | buffer | script }
 * = network = { 1 | 0 }
 * = secured = { 1 | 0 }
 *
 *
 * client
 * +-------+------+---------+-------------+-----------+---------+-----------+-------+
 * | pkgid | Icon |  Name   | auto_launch | gbar_size | content | nodisplay | setup |
 * +-------+------+---------+-------------+-----------+---------+-----------+-------+
 * |   -   |   -  |    -    |      -      |    -      |    -    |     -     |   -   |
 * +-------+------+---------+-------------+-----------+---------+-----------+-------+
 * CREATE TABLE client ( pkgid TEXT PRIMARY KEY NOT NULL, icon TEXT, name TEXT, auto_launch TEXT, gbar_size TEXT, content TEXT, nodisplay INTEGER, setup TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )
 *
 * = auto_launch = UI-APPID
 * = gbar_size = WIDTHxHEIGHT
 *
 *
 * i18n
 * +-------+------+------+------+
 * |   fk  | lang | name | icon |
 * +-------+------+------+------+
 * | pkgid |   -  |   -  |   -  |
 * +-------+------+------+------+
 * CREATE TABLE i18n ( pkgid TEXT NOT NULL, lang TEXT COLLATE NOCASE, name TEXT, icon TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )
 *
 *
 * box_size
 * +-------+-----------+---------+--------------+------------+-------------------------+
 * | pkgid | size_type | preview | touch_effect | need_frame | mouse_event(from ver 3) |
 * +-------+-----------+---------+--------------+------------+-------------------------+
 * |   -   |     -     |    -    |       -      |     -      |            -            |
 * +-------+-----------+---------+--------------+------------+-------------------------+
 * CREATE TABLE box_size ( pkgid TEXT NOT NULL, size_type INTEGER, preview TEXT, INTEGER, touch_effect INTEGER, need_frame INTEGER, mouse_event INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )
 *
 * = box_size_list = { WIDTHxHEIGHT; WIDTHxHEIGHT; ... }
 *
 * groupinfo
 * +----+---------+----------+-------+
 * | id | cluster | category | pkgid |
 * +----+---------+----------+-------+
 * |  - |    -    |    -     |   -   |
 * +----+---------+----------+-------|
 * CREATE TABLE groupinfo ( id INTEGER PRIMARY KEY AUTOINCREMENT, cluster TEXT NOT NULL, category TEXT NOT NULL, appid TEXT NOT NULL, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ))
 *
 * groupmap
 * +-------+----+----------+-----------+
 * | pkgid | id | ctx_item | option_id |
 * +-------+----+----------+-----------+
 * CREATE TABLE groupmap ( option_id INTEGER PRIMARY KEY AUTOINCREMENT, id INTEGER, pkgid TEXT NOT NULL, ctx_item TEXT NOT NULL, FOREIGN KEY(id) REFERENCES groupinfo(id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) )
 *
 *
 * option
 * +-------+-----------+-----+-------+
 * | pkgid | option_id | key | value |
 * +-------+-----------+-----+-------+
 * CREATE TABLE option ( pkgid TEXT NOT NULL, option_id INTEGER, key TEXT NOT NULL, value TEXT NOT NULL, FOREIGN KEY(option_id) REFERENCES groupmap(option_id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid)  )
 */

#if !defined(LIBXML_TREE_ENABLED)
	#error "LIBXML is not supporting the tree"
#endif

#if defined(LOG_TAG)
#undef LOG_TAG
#endif

#define LOG_TAG "PKGMGR_DYNAMICBOX"

 int errno;

 struct i18n {
     xmlChar *lang;
     xmlChar *name;
     xmlChar *icon;
 };

struct dynamicbox {
    xmlChar *pkgid;
    int secured;
    int network;
    xmlChar *auto_launch;
    xmlChar *abi;
    xmlChar *name; /* Default name */
    xmlChar *icon; /* Default icon */
    xmlChar *libexec; /* Path of the SO file */
    xmlChar *timeout; /* INTEGER, timeout */
    xmlChar *period; /* DOUBLE, update period */
    xmlChar *script; /* Script engine */
    xmlChar *content; /* Content information */
    xmlChar *setup;
    xmlChar *uiapp;	/* UI App Id */
    xmlChar *category; /* Category of this box */

    int pinup; /* Is this support the pinup feature? */
    int primary; /* Is this primary dynamicbox? */
    int nodisplay;
    int count; /* Max count of instances */
    int direct_input; /* Use the input node to get the event directly */

    int default_mouse_event; /* Mouse event processing option for dynamicbox */
    int default_touch_effect;
    int default_need_frame;

    enum dynamicbox_dbox_type dbox_type;
    xmlChar *dbox_src;
    xmlChar *dbox_group;
    int size_list; /* 1x1, 2x1, 2x2, 4x1, 4x2, 4x3, 4x4 */

    xmlChar *preview[DBOX_NR_OF_SIZE_LIST];
    int touch_effect[DBOX_NR_OF_SIZE_LIST]; /* Touch effect of a dynamicbox */
    int need_frame[DBOX_NR_OF_SIZE_LIST]; /* Box needs frame which should be cared by viewer */
    int mouse_event[DBOX_NR_OF_SIZE_LIST];

    enum dynamicbox_gbar_type gbar_type;
    xmlChar *gbar_src;
    xmlChar *gbar_group;
    xmlChar *gbar_size; /* Default PD size */
    xmlChar *hw_acceleration;

    struct dlist *i18n_list;
    struct dlist *group_list;
};

struct group {
    xmlChar *cluster;
    xmlChar *category;
    xmlChar *ctx_item;
    struct dlist *option_list;
};

struct option {
    xmlChar *key;
    xmlChar *value;
};

static struct {
    const char *dbfile;
    sqlite3 *handle;
} s_info = {
    .dbfile = "/opt/dbspace/.dynamicbox.db",
    .handle = NULL,
};

static inline int begin_transaction(void)
{
    sqlite3_stmt *stmt;
    int ret;

    ret = sqlite3_prepare_v2(s_info.handle, "BEGIN TRANSACTION", -1, &stmt, NULL);

    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return EXIT_FAILURE;
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Failed to do update (%s)\n",
		sqlite3_errmsg(s_info.handle));
	sqlite3_finalize(stmt);
	return EXIT_FAILURE;
    }

    sqlite3_finalize(stmt);
    return EXIT_SUCCESS;
}

static inline int rollback_transaction(void)
{
    int ret;
    sqlite3_stmt *stmt;

    ret = sqlite3_prepare_v2(s_info.handle, "ROLLBACK TRANSACTION", -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return EXIT_FAILURE;
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Failed to do update (%s)\n",
		sqlite3_errmsg(s_info.handle));
	sqlite3_finalize(stmt);
	return EXIT_FAILURE;
    }

    sqlite3_finalize(stmt);
    return EXIT_SUCCESS;
}

static inline int commit_transaction(void)
{
    sqlite3_stmt *stmt;
    int ret;

    ret = sqlite3_prepare_v2(s_info.handle, "COMMIT TRANSACTION", -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return EXIT_FAILURE;
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Failed to do update (%s)\n",
		sqlite3_errmsg(s_info.handle));
	sqlite3_finalize(stmt);
	return EXIT_FAILURE;
    }

    sqlite3_finalize(stmt);
    return EXIT_SUCCESS;
}

static void db_create_version(void)
{
    static const char *ddl = "CREATE TABLE version (version INTEGER)";
    char *err;

    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }
}

static int set_version(int version)
{
    static const char *dml = "INSERT INTO version (version) VALUES (?)";
    sqlite3_stmt *stmt;
    int ret;

    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrint("Failed to prepare the initial DML(%s)\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    if (sqlite3_bind_int(stmt, 1, version) != SQLITE_OK) {
	ErrPrint("Failed to bind a id(%s)\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
	ErrPrint("Failed to execute the DML for version: %d\n", ret);
	ret = -EIO;
    } else {
	ret = 0;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static int update_version(int version)
{
    static const char *dml = "UPDATE version SET version = ?";
    sqlite3_stmt *stmt;
    int ret;

    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrint("Failed to prepare the initial DML(%s)\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    if (sqlite3_bind_int(stmt, 1, version) != SQLITE_OK) {
	ErrPrint("Failed to bind a version: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
	ErrPrint("Failed to execute DML: %d\n", ret);
	ret = -EIO;
    } else {
	ret = 0;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static int get_version(void)
{
    static const char *dml = "SELECT version FROM version";
    sqlite3_stmt *stmt;
    int ret;

    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	return -ENOSYS;
    }

    if (sqlite3_step(stmt) != SQLITE_ROW) {
	ret = -ENOENT;
    } else {
	ret = sqlite3_column_int(stmt, 0);
    }

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

/*!
 * \note
 * From version 1 to 2
 */
static void upgrade_pkgmap_for_category(void)
{
    char *err;
    static const char *ddl;

    ddl = "ALTER TABLE pkgmap ADD COLUMN category TEXT DEFAULT \"" DEFAULT_CATEGORY "\"";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return;
}

/**
 * From version 4 to 5
 * "provider" table should have "direct_input" column.
 * "direct_input" will be used for selecting input event path.
 * if it is "true", the provider must has to get all events from device node directly.
 * The file descriptor will be given by data-provider-master
 */
static void upgrade_to_version_5(void)
{
    char *err;
    static const char *ddl;

    /*
     * Step 1
     * Create a new column "direct_input" for provider table
     */
    ddl = "ALTER TABLE provider ADD COLUMN direct_input INTEGER DEFAULT 0";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
        return;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    /*
     * Step 2
     * Create a new column "hw_acceleration" for provider table
     */
    ddl = "ALTER TABLE provider ADD COLUMN hw_acceleration TEXT DEFAULT 'none'";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
        return;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }
}

/*!
 * \note
 * From version 3 to 4
 * "provider" table should have "count" column.
 * "count" will be used for limiting creatable count of instances for each dynamicbox.
 * Every dynamicbox developer should describe their max count of creatable instances.
 */
static void upgrade_to_version_4(void)
{
    char *err;
    static const char *ddl;

    /*
     * Step 1
     * Create a new column for count to provider table.
     */
    ddl = "ALTER TABLE provider ADD COLUMN count INTEGER DEFAULT 0";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }
}

/*!
 * \note
 * From version 2 to 3
 * mouse_event is deleted from client table
 * mouse_event is added to box_size table
 *
 * Every size has their own configuration for mouse_event flag.
 */
static void upgrade_to_version_3(void)
{
    char *err;
    static const char *ddl;
    static const char *dml;
    sqlite3_stmt *select_stmt;
    int ret;

    /*
     * Step 1
     * Create a new column for mouse_event to box_size table.
     */
    ddl = "ALTER TABLE box_size ADD COLUMN mouse_event INTEGER DEFAULT 0";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    /*
     * Step 2
     * Copy mouse_event values from the client to the box_size table.
     */
    dml = "SELECT pkgid, mouse_event FROM client";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &select_stmt, NULL);
    if (ret == SQLITE_OK) {
	sqlite3_stmt *update_stmt;

	dml = "UPDATE box_size SET mouse_event = ? WHERE pkgid = ?";
	ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &update_stmt, NULL);
	if (ret == SQLITE_OK) {
	    int mouse_event;
	    const char *pkgid;

	    while (sqlite3_step(select_stmt) == SQLITE_ROW) {
		pkgid = (const char *)sqlite3_column_text(select_stmt, 0);
		if (!pkgid) {
		    ErrPrint("Package Id is not valid\n");
		    continue;
		}

		mouse_event = sqlite3_column_int(select_stmt, 1);

		ret = sqlite3_bind_int(update_stmt, 1, mouse_event);
		if (ret != SQLITE_OK) {
		    ErrPrint("Failed to bind mouse_event [%s], [%d]\n", pkgid, mouse_event);
		    sqlite3_reset(update_stmt);
		    sqlite3_clear_bindings(update_stmt);
		    continue;
		}

		ret = sqlite3_bind_text(update_stmt, 2, pkgid, -1, SQLITE_TRANSIENT);
		if (ret != SQLITE_OK) {
		    ErrPrint("Failed to bind pkgid [%s], [%d]\n", pkgid, mouse_event);
		    sqlite3_reset(update_stmt);
		    sqlite3_clear_bindings(update_stmt);
		    continue;
		}

		ret = sqlite3_step(update_stmt);
		if (ret != SQLITE_DONE) {
		    ErrPrint("Failed to execute DML: %d\n", ret);
		    sqlite3_reset(update_stmt);
		    sqlite3_clear_bindings(update_stmt);
		    continue;
		}

		sqlite3_reset(update_stmt);
		sqlite3_clear_bindings(update_stmt);
	    }

	    sqlite3_finalize(update_stmt);
	} else {
	    ErrPrint("Failed to execute DML\n");
	}

	sqlite3_reset(select_stmt);
	sqlite3_clear_bindings(select_stmt);
	sqlite3_finalize(select_stmt);
    } else {
	ErrPrint("Failed to prepare the initial DML(%s)\n", sqlite3_errmsg(s_info.handle));
    }

    /*
     * Step 3
     * Drop a column from the client table
     */
    ddl = "ALTER TABLE client DROP COLUMN mouse_event";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return;
}

static void do_upgrade_db_schema(void)
{
    int version;

    version = get_version();

    switch (version) {
	case -ENOSYS:
	    db_create_version();
	    /* Need to create version table */
	case -ENOENT:
	    if (set_version(CUR_VER) < 0) {
		ErrPrint("Failed to set version\n");
	    }
	    /* Need to set version */
	case 1:
	    upgrade_pkgmap_for_category();
	case 2:
	    upgrade_to_version_3();
	case 3:
	    upgrade_to_version_4();
	case 4:
	    upgrade_to_version_5();
	default:
	    /* Need to update version */
	    DbgPrint("Old version: %d\n", version);
	    if (update_version(CUR_VER) < 0) {
		ErrPrint("Failed to update version\n");
	    }
	case CUR_VER:
	    break;
    }
}

static inline int db_create_pkgmap(void)
{
    char *err;
    static const char *ddl;

    ddl = "CREATE TABLE pkgmap ( pkgid TEXT PRIMARY KEY NOT NULL, appid TEXT, uiapp TEXT, prime INTEGER, category TEXT )";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return 0;
}

static inline int db_insert_pkgmap(const char *appid, const char *pkgid, const char *uiappid, int primary, const char *category)
{
    int ret;
    static const char *dml;
    sqlite3_stmt *stmt;

    dml = "INSERT INTO pkgmap ( appid, pkgid, uiapp, prime, category ) VALUES (? ,?, ?, ?, ?)";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, appid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 2, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 3, uiappid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 4, primary);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 5, category, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_remove_pkgmap(const char *pkgid)
{
    int ret;
    static const char *dml;
    sqlite3_stmt *stmt;

    dml = "DELETE FROM pkgmap WHERE pkgid = ?";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_create_provider(void)
{
    char *err;
    static const char *ddl;

    ddl = "CREATE TABLE provider (" \
	   "pkgid TEXT PRIMARY KEY NOT NULL, network INTEGER, " \
	   "abi TEXT, secured INTEGER, box_type INTEGER, " \
	   "box_src TEXT, box_group TEXT, gbar_type INTEGER, " \
	   "gbar_src TEXT, gbar_group TEXT, libexec TEXT, timeout INTEGER, period TEXT, script TEXT, pinup INTEGER, "\
	   "count INTEGER, direct_input INTEGER DEFAULT 0, hw_acceleration TEXT DEFAULT 'none', "\
	   "FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE)";

    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return 0;
}

static inline int db_remove_provider(const char *pkgid)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "DELETE FROM provider WHERE pkgid = ?";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}
static int db_insert_provider(struct dynamicbox *dynamicbox)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;
    char *abi = (char *)dynamicbox->abi;
    char *box_src = (char *)dynamicbox->dbox_src;
    char *box_group = (char *)dynamicbox->dbox_group;
    char *gbar_src = (char *)dynamicbox->gbar_src;
    char *gbar_group = (char *)dynamicbox->gbar_group;
    char *libexec = (char *)dynamicbox->libexec;
    char *timeout = (char *)dynamicbox->timeout;
    char *period = (char *)dynamicbox->period;
    char *script = (char *)dynamicbox->script;
    char *hw_acceleration = (char *)dynamicbox->hw_acceleration;

    if (!abi) {
	abi = "c";
    }

    if (!timeout) {
	timeout = "10";
    }

    if (!period) {
	period = "0.0";
    }

    if (!script) {
	script = "edje";
    }

    if (!hw_acceleration) {
	hw_acceleration = "none";
    }

    dml = "INSERT INTO provider ( pkgid, network, abi, secured, box_type, box_src, box_group, gbar_type, gbar_src, gbar_group, libexec, timeout, period, script, pinup, count, direct_input, hw_acceleration) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, (char *)dynamicbox->pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 2, dynamicbox->network);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 3, abi, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }
    ret = sqlite3_bind_int(stmt, 4, dynamicbox->secured);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 5, dynamicbox->dbox_type);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 6, box_src, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 7, box_group, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 8, dynamicbox->gbar_type);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 9, gbar_src, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 10, gbar_group, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 11, libexec, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 12, atoi(timeout));
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 13, period, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 14, script, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 15, dynamicbox->pinup);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 16, dynamicbox->count);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 17, dynamicbox->direct_input);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 18, hw_acceleration, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_create_client(void)
{
    char *err;
    static const char *ddl;

    ddl = "CREATE TABLE client (" \
	   "pkgid TEXT PRIMARY KEY NOT NULL, icon TEXT, name TEXT, " \
	   "auto_launch TEXT, gbar_size TEXT, content TEXT, nodisplay INTEGER, setup TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE)";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return 0;
}

static inline int db_insert_client(struct dynamicbox *dynamicbox)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "INSERT INTO client ( pkgid, icon, name, auto_launch, gbar_size, content, nodisplay, setup ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, (char *)dynamicbox->pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 2, (char *)dynamicbox->icon, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 3, (char *)dynamicbox->name, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 4, (char *)dynamicbox->auto_launch, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 5, (char *)dynamicbox->gbar_size, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 6, (char *)dynamicbox->content, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 7, dynamicbox->nodisplay);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 8, (char *)dynamicbox->setup, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_remove_client(const char *pkgid)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "DELETE FROM client WHERE pkgid = ?";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_create_i18n(void)
{
    char *err;
    static const char *ddl;

    ddl = "CREATE TABLE i18n ( pkgid TEXT NOT NULL, lang TEXT COLLATE NOCASE, name TEXT, " \
	   "icon TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE)";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return 0;
}

static inline int db_insert_i18n(const char *pkgid, const char *lang, const char *name, const char *icon)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    DbgPrint("%s - lang[%s] name[%s] icon[%s]\n", pkgid, lang, name, icon);
    dml = "INSERT INTO i18n ( pkgid, lang, name, icon ) VALUES (?, ?, ?, ?)";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 2, lang, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 3, name, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 4, icon, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_remove_i18n(const char *pkgid)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "DELETE FROM i18n WHERE pkgid = ?";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	DbgPrint("No changes\n");
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_create_group(void)
{
    char *err;
    static const char *ddl;

    ddl = "CREATE TABLE groupinfo ( id INTEGER PRIMARY KEY AUTOINCREMENT, cluster TEXT NOT NULL, category TEXT NOT NULL, pkgid TEXT NOT NULL, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE)";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return 0;
}

static inline int db_insert_group(const char *pkgid, const char *cluster, const char *category)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "INSERT INTO groupinfo ( cluster, category, pkgid ) VALUES (?, ?, ?)";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, cluster, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 2, category, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 3, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static int db_get_group_id(const char *cluster, const char *category)
{
    static const char *dml = "SELECT id FROM groupinfo WHERE cluster = ? AND category = ?";
    sqlite3_stmt *stmt;
    int ret;

    if (!cluster || !category) {
	ErrPrint("Invalid argument\n");
	return -EINVAL;
    }

    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrint("Failed to prepare the initial DML(%s)\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = -EIO;
    if (sqlite3_bind_text(stmt, 1, cluster, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
	ErrPrint("Failed to bind a cluster(%s) - %s\n", cluster, sqlite3_errmsg(s_info.handle));
	goto out;
    }

    if (sqlite3_bind_text(stmt, 2, category, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
	ErrPrint("Failed to bind a category(%s) - %s\n", category, sqlite3_errmsg(s_info.handle));
	goto out;
    }

    if (sqlite3_step(stmt) != SQLITE_ROW) {
	ErrPrint("Failed to execute the DML for %s - %s\n", cluster, category);
	goto out;
    }

    ret = sqlite3_column_int(stmt, 0);

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_remove_group(const char *pkgid)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "DELETE FROM groupinfo WHERE pkgid = ?";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	DbgPrint("No changes\n");
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_create_groupmap(void)
{
    char *err;
    static const char *ddl;

    ddl = "CREATE TABLE groupmap (option_id INTEGER PRIMARY KEY AUTOINCREMENT, id INTEGER, pkgid TEXT NOT NULL, ctx_item TEXT NOT NULL, FOREIGN KEY(id) REFERENCES groupinfo(id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE)";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return 0;
}

static inline int db_get_option_id(int id, const char *pkgid, const char *ctx_item)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "SELECT option_id FROM groupmap WHERE id = ? AND pkgid = ? AND ctx_item = ?";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_int(stmt, 1, id);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 2, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 3, ctx_item, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_ROW) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_column_int(stmt, 0);

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_insert_groupmap(int id, const char *pkgid, const char *ctx_item)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    DbgPrint("%d (%s) add to groupmap\n", id, pkgid);

    dml = "INSERT INTO groupmap ( id, pkgid, ctx_item ) VALUES (?, ?, ?)";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_int(stmt, 1, id);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 2, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 3, ctx_item, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_remove_groupmap(const char *pkgid)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "DELETE FROM groupmap WHERE pkgid = ?";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	DbgPrint("No changes\n");
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_create_option(void)
{
    char *err;
    static const char *ddl;

    ddl = "CREATE TABLE option ( pkgid TEXT NOT NULL, option_id INTEGER, key TEXT NOT NULL, value TEXT NOT NULL, " \
	   "FOREIGN KEY(option_id) REFERENCES groupmap(option_id), " \
	   "FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE)";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return 0;
}

static inline int db_insert_option(const char *pkgid, int option_id, const char *key, const char *value)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "INSERT INTO option (pkgid, option_id, key, value) VALUES (?, ?, ?, ?)";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 2, option_id);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 3, key, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 4, value, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }
out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_remove_option(const char *pkgid)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "DELETE FROM option WHERE pkgid = ?";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	DbgPrint("No changes\n");
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_create_box_size(void)
{
    char *err;
    static const char *ddl;

    ddl = "CREATE TABLE box_size ( pkgid TEXT NOT NULL, size_type INTEGER, preview TEXT, touch_effect INTEGER, need_frame INTEGER, mouse_event INTEGER " \
	   "FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE)";
    if (sqlite3_exec(s_info.handle, ddl, NULL, NULL, &err) != SQLITE_OK) {
	ErrPrint("Failed to execute the DDL (%s)\n", err);
	return -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	ErrPrint("No changes to DB\n");
    }

    return 0;
}

static int db_insert_box_size(const char *pkgid, int size_type, const char *preview, int touch_effect, int need_frame, int mouse_event)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    DbgPrint("box size: %s - %d (%s) is added\n", pkgid, size_type, preview);
    dml = "INSERT INTO box_size ( pkgid, size_type, preview, touch_effect, need_frame, mouse_event ) VALUES (?, ?, ?, ?, ?, ?)";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 2, size_type);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_text(stmt, 3, preview, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 4, touch_effect);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 5, need_frame);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = sqlite3_bind_int(stmt, 6, mouse_event);
    if (ret != SQLITE_OK) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	ErrPrintWithConsole("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline int db_remove_box_size(const char *pkgid)
{
    static const char *dml;
    int ret;
    sqlite3_stmt *stmt;

    dml = "DELETE FROM box_size WHERE pkgid = ?";
    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = sqlite3_bind_text(stmt, 1, pkgid, -1, SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
	goto out;
    }

    ret = 0;
    if (sqlite3_step(stmt) != SQLITE_DONE) {
	DbgPrint("Error: %s\n", sqlite3_errmsg(s_info.handle));
	ret = -EIO;
    }

    if (sqlite3_changes(s_info.handle) == 0) {
	DbgPrint("No changes\n");
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return ret;
}

static inline void db_create_table(void)
{
    int ret;
    begin_transaction();

    ret = db_create_pkgmap();
    if (ret < 0) {
	rollback_transaction();
	return;
    }

    ret = db_create_provider();
    if (ret < 0) {
	rollback_transaction();
	return;
    }

    ret = db_create_client();
    if (ret < 0) {
	rollback_transaction();
	return;
    }

    ret = db_create_i18n();
    if (ret < 0) {
	rollback_transaction();
	return;
    }

    ret = db_create_box_size();
    if (ret < 0) {
	rollback_transaction();
	return;
    }

    ret = db_create_group();
    if (ret < 0) {
	rollback_transaction();
	return;
    }

    ret = db_create_option();
    if (ret < 0) {
	rollback_transaction();
	return;
    }

    ret = db_create_groupmap();
    if (ret < 0) {
	rollback_transaction();
	return;
    }

    commit_transaction();
}

static int db_init(void)
{
    int ret;
    struct stat stat;

    ret = db_util_open(s_info.dbfile, &s_info.handle, DB_UTIL_REGISTER_HOOK_METHOD);
    if (ret != SQLITE_OK) {
	ErrPrint("Failed to open a DB\n");
	return -EIO;
    }

    if (lstat(s_info.dbfile, &stat) < 0) {
	ErrPrint("%s\n", strerror(errno));
	db_util_close(s_info.handle);
	s_info.handle = NULL;
	return -EIO;
    }

    if (!S_ISREG(stat.st_mode)) {
	ErrPrint("Invalid file\n");
	db_util_close(s_info.handle);
	s_info.handle = NULL;
	return -EINVAL;
    }

    if (!stat.st_size) {
	db_create_table();
    }

    return 0;
}

static inline int db_fini(void)
{
    if (!s_info.handle) {
	return 0;
    }

    db_util_close(s_info.handle);
    s_info.handle = NULL;

    return 0;
}

static inline int validate_pkgid(const char *appid, const char *pkgid)
{
    /* Just return 1 Always */
    return 1 || !strncmp(appid, pkgid, strlen(appid));
}

static int dynamicbox_destroy(struct dynamicbox *dynamicbox)
{
    struct dlist *l;
    struct dlist *n;
    struct i18n *i18n;
    struct group *group;
    struct option *option;
    struct dlist *il;
    struct dlist *in;

    xmlFree(dynamicbox->auto_launch);
    xmlFree(dynamicbox->pkgid);
    xmlFree(dynamicbox->abi);
    xmlFree(dynamicbox->name);
    xmlFree(dynamicbox->icon);
    xmlFree(dynamicbox->dbox_src);
    xmlFree(dynamicbox->dbox_group);
    xmlFree(dynamicbox->gbar_src);
    xmlFree(dynamicbox->gbar_group);
    xmlFree(dynamicbox->gbar_size);
    xmlFree(dynamicbox->libexec);
    xmlFree(dynamicbox->script);
    xmlFree(dynamicbox->period);
    xmlFree(dynamicbox->content);
    xmlFree(dynamicbox->setup);
    xmlFree(dynamicbox->category);
    xmlFree(dynamicbox->preview[0]); /* 1x1 */
    xmlFree(dynamicbox->preview[1]); /* 2x1 */
    xmlFree(dynamicbox->preview[2]); /* 2x2 */
    xmlFree(dynamicbox->preview[3]); /* 4x1 */
    xmlFree(dynamicbox->preview[4]); /* 4x2 */
    xmlFree(dynamicbox->preview[5]); /* 4x3 */
    xmlFree(dynamicbox->preview[6]); /* 4x4 */
    xmlFree(dynamicbox->preview[7]); /* 4x5 */
    xmlFree(dynamicbox->preview[8]); /* 4x6 */
    xmlFree(dynamicbox->preview[9]); /* easy 1x1 */
    xmlFree(dynamicbox->preview[10]); /* easy 3x1 */
    xmlFree(dynamicbox->preview[11]); /* easy 3x3 */
    xmlFree(dynamicbox->preview[12]); /* full */
    xmlFree(dynamicbox->hw_acceleration);

    dlist_foreach_safe(dynamicbox->i18n_list, l, n, i18n) {
	dynamicbox->i18n_list = dlist_remove(dynamicbox->i18n_list, l);
	xmlFree(i18n->name);
	xmlFree(i18n->icon);
	xmlFree(i18n->lang);
	free(i18n);
    }

    dlist_foreach_safe(dynamicbox->group_list, l, n, group) {
	dynamicbox->group_list = dlist_remove(dynamicbox->group_list, l);
	DbgPrint("Release %s/%s\n", group->cluster, group->category);

	if (group->ctx_item) {
	    dlist_foreach_safe(group->option_list, il, in, option) {
		group->option_list = dlist_remove(group->option_list, il);
		DbgPrint("Release option %s(%s)\n", option->key, option->value);
		xmlFree(option->key);
		xmlFree(option->value);
		free(option);
	    }
	    xmlFree(group->ctx_item);
	}

	xmlFree(group->cluster);
	xmlFree(group->category);
	free(group);
    }

    free(dynamicbox);
    return 0;
}

static inline void update_i18n_name(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    struct i18n *i18n;
    struct dlist *l;
    xmlChar *lang;
    xmlChar *name;

    name = xmlNodeGetContent(node);
    if (!name) {
	ErrPrint("Invalid tag\n");
	return;
    }

    lang = xmlNodeGetLang(node);
    if (!lang) {
	if (dynamicbox->name) {
	    DbgPrint("Override default name: %s\n", dynamicbox->name);
	    xmlFree(dynamicbox->name);
	}

	dynamicbox->name = name;
	return;
    }

    dlist_foreach(dynamicbox->i18n_list, l, i18n) {
	if (!xmlStrcasecmp(i18n->lang, lang)) {
	    if (i18n->name) {
		DbgPrint("Override name: %s\n", i18n->name);
		xmlFree(i18n->name);
	    }

	    i18n->name = name;
	    return;
	}
    }

    i18n = calloc(1, sizeof(*i18n));
    if (!i18n) {
	ErrPrint("Heap: %s\n", strerror(errno));
	xmlFree(name);
	xmlFree(lang);
	return;
    }

    i18n->name = name;
    i18n->lang = lang;
    DbgPrint("Label[%s] - [%s] added\n", i18n->lang, i18n->name);
    dynamicbox->i18n_list = dlist_append(dynamicbox->i18n_list, i18n);
}

static inline void update_i18n_icon(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    struct i18n *i18n;
    struct dlist *l;
    xmlChar *lang;
    xmlChar *icon;

    icon = xmlNodeGetContent(node);
    if (!icon) {
	ErrPrint("Invalid tag\n");
	return;
    }

    lang = xmlNodeGetLang(node);
    if (!lang) {
	if (dynamicbox->icon) {
	    DbgPrint("Override default icon: %s\n", dynamicbox->icon);
	    xmlFree(dynamicbox->icon);
	}

	dynamicbox->icon = icon;
	return;
    }

    dlist_foreach(dynamicbox->i18n_list, l, i18n) {
	if (!xmlStrcasecmp(i18n->lang, lang)) {
	    if (i18n->icon) {
		DbgPrint("Override icon %s for %s\n", i18n->icon, i18n->name);
		xmlFree(i18n->icon);
	    }

	    i18n->icon = icon;
	    return;
	}
    }

    i18n = calloc(1, sizeof(*i18n));
    if (!i18n) {
	ErrPrint("Heap: %s\n", strerror(errno));
	xmlFree(icon);
	xmlFree(lang);
	return;
    }

    i18n->icon = icon;
    i18n->lang = lang;
    DbgPrint("Icon[%s] - [%s] added\n", i18n->lang, i18n->icon);
    dynamicbox->i18n_list = dlist_append(dynamicbox->i18n_list, i18n);
}

static inline void update_launch(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    xmlChar *launch;

    launch = xmlNodeGetContent(node);
    if (!launch) {
	DbgPrint("Has no launch\n");
	return;
    }

    if (dynamicbox->auto_launch) {
	xmlFree(dynamicbox->auto_launch);
    }

    dynamicbox->auto_launch = xmlStrdup(launch);
    if (!dynamicbox->auto_launch) {
	ErrPrint("Failed to duplicate string: %s\n", (char *)launch);
	return;
    }
}

static inline void update_category(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    xmlChar *category;

    category = xmlGetProp(node, (const xmlChar *)"name");
    if (!category) {
	DbgPrint("Has no valid category\n");
	return;
    }

    if (dynamicbox->category) {
	xmlFree(dynamicbox->category);
    }

    dynamicbox->category = xmlStrdup(category);
    if (!dynamicbox->category) {
	ErrPrint("Failed to duplicate string: %s\n", (char *)category);
	return;
    }
}

static inline void update_ui_appid(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    xmlChar *uiapp;
    uiapp = xmlNodeGetContent(node);
    if (!uiapp) {
	DbgPrint("Has no valid ui-appid\n");
	return;
    }

    if (dynamicbox->uiapp) {
	xmlFree(dynamicbox->uiapp);
    }

    dynamicbox->uiapp = xmlStrdup(uiapp);
    if (!dynamicbox->uiapp) {
	ErrPrint("Failed to duplicate string: %s\n", (char *)uiapp);
	return;
    }
}

static inline void update_setup(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    xmlChar *setup;
    setup = xmlNodeGetContent(node);
    if (!setup) {
	DbgPrint("Has no setup\n");
	return;
    }

    if (dynamicbox->setup) {
	xmlFree(dynamicbox->setup);
    }

    dynamicbox->setup = xmlStrdup(setup);
    if (!dynamicbox->setup) {
	ErrPrint("Failed to duplicate string: %s\n", (char *)setup);
	return;
    }
}

static inline void update_content(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    xmlChar *content;
    content = xmlNodeGetContent(node);
    if (!content) {
	DbgPrint("Has no content\n");
	return;
    }

    if (dynamicbox->content) {
	xmlFree(dynamicbox->content);
    }

    dynamicbox->content = xmlStrdup(content);
    if (!dynamicbox->content) {
	ErrPrint("Failed to duplicate string: %s\n", (char *)content);
	return;
    }
}

static void update_size_info(struct dynamicbox *dynamicbox, int idx, xmlNodePtr node)
{
    if (xmlHasProp(node, (const xmlChar *)"preview")) {
	dynamicbox->preview[idx] = xmlGetProp(node, (const xmlChar *)"preview");
    }

    if (xmlHasProp(node, (const xmlChar *)"need_frame")) {
	xmlChar *need_frame;

	need_frame = xmlGetProp(node, (const xmlChar *)"need_frame");
	if (need_frame) {
	    dynamicbox->need_frame[idx] = !xmlStrcasecmp(need_frame, (const xmlChar *)"true");
	    xmlFree(need_frame);
	} else {
	    dynamicbox->need_frame[idx] = dynamicbox->default_need_frame;
	}
    } else {
	dynamicbox->need_frame[idx] = dynamicbox->default_need_frame;
    }

    if (xmlHasProp(node, (const xmlChar *)"touch_effect")) {
	xmlChar *touch_effect;

	touch_effect = xmlGetProp(node, (const xmlChar *)"touch_effect");
	if (touch_effect) {
	    dynamicbox->touch_effect[idx] = !xmlStrcasecmp(touch_effect, (const xmlChar *)"true");
	    xmlFree(touch_effect);
	} else {
	    dynamicbox->touch_effect[idx] = dynamicbox->default_touch_effect;
	}
    } else {
	dynamicbox->touch_effect[idx] = dynamicbox->default_touch_effect;
    }

    if (xmlHasProp(node, (const xmlChar *)"mouse_event")) {
	xmlChar *mouse_event;

	mouse_event = xmlGetProp(node, (const xmlChar *)"mouse_event");
	if (mouse_event) {
	    dynamicbox->mouse_event[idx] = !xmlStrcasecmp(mouse_event, (const xmlChar *)"true");
	    xmlFree(mouse_event);
	} else {
	    dynamicbox->mouse_event[idx] = dynamicbox->default_mouse_event;
	}
    } else {
	dynamicbox->mouse_event[idx] = dynamicbox->default_mouse_event;
    }
}

static void update_box(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    if (!xmlHasProp(node, (const xmlChar *)"type")) {
	dynamicbox->dbox_type = DBOX_TYPE_FILE;
    } else {
	xmlChar *type;

	type = xmlGetProp(node, (const xmlChar *)"type");
	if (!type) {
	    ErrPrint("Type is NIL\n");
	    dynamicbox->dbox_type = DBOX_TYPE_FILE;
	} else {
	    if (!xmlStrcasecmp(type, (const xmlChar *)"text")) {
		dynamicbox->dbox_type = DBOX_TYPE_TEXT;
	    } else if (!xmlStrcasecmp(type, (const xmlChar *)"buffer")) {
		dynamicbox->dbox_type = DBOX_TYPE_BUFFER;
	    } else if (!xmlStrcasecmp(type, (const xmlChar *)"script")) {
		dynamicbox->dbox_type = DBOX_TYPE_SCRIPT;
	    } else if (!xmlStrcasecmp(type, (const xmlChar *)"elm")) {
		dynamicbox->dbox_type = DBOX_TYPE_UIFW;
	    } else { /* Default */
		dynamicbox->dbox_type = DBOX_TYPE_FILE;
	    }

	    xmlFree(type);
	}
    }

    if (!xmlHasProp(node, (const xmlChar *)"mouse_event")) {
	dynamicbox->default_mouse_event = 0;
    } else {
	xmlChar *mouse_event;

	mouse_event = xmlGetProp(node, (const xmlChar *)"mouse_event");
	if (!mouse_event) {
	    ErrPrint("mouse_event is NIL\n");
	    dynamicbox->default_mouse_event = 0;
	} else {
	    dynamicbox->default_mouse_event = !xmlStrcasecmp(mouse_event, (const xmlChar *)"true");
	    xmlFree(mouse_event);
	}
    }

    if (!xmlHasProp(node, (const xmlChar *)"touch_effect")) {
	dynamicbox->default_touch_effect = 1;
    } else {
	xmlChar *touch_effect;

	touch_effect = xmlGetProp(node, (const xmlChar *)"touch_effect");
	if (!touch_effect) {
	    ErrPrint("default touch_effect is NIL\n");
	    dynamicbox->default_touch_effect = 1;
	} else {
	    dynamicbox->default_touch_effect = !xmlStrcasecmp(touch_effect, (const xmlChar *)"true");
	    xmlFree(touch_effect);
	}
    }

    if (!xmlHasProp(node, (const xmlChar *)"need_frame")) {
	dynamicbox->default_need_frame = 0;
    } else {
	xmlChar *need_frame;

	need_frame = xmlGetProp(node, (const xmlChar *)"need_frame");
	if (!need_frame) {
	    ErrPrint("default need_frame is NIL\n");
	    dynamicbox->default_need_frame = 0;
	} else {
	    dynamicbox->default_need_frame = !xmlStrcasecmp(need_frame, (const xmlChar *)"true");
	    xmlFree(need_frame);
	}
    }

    for (node = node->children; node; node = node->next) {
	if (!xmlStrcasecmp(node->name, (const xmlChar *)"size")) {
	    xmlChar *size;
	    int is_easy = 0;

	    size = xmlNodeGetContent(node);
	    if (!size) {
		ErrPrint("Invalid size tag\n");
		continue;
	    }

	    if (xmlHasProp(node, (const xmlChar *)"mode")) {
		xmlChar *mode;
		mode = xmlGetProp(node, (const xmlChar *)"mode");
		if (mode) {
		    DbgPrint("Easy mode: %s\n", mode);
		    is_easy = !xmlStrcasecmp(mode, (const xmlChar *)"easy");
		    xmlFree(mode);
		}
	    }

	    if (!xmlStrcasecmp(size, (const xmlChar *)"1x1")) {
		if (is_easy) {
		    dynamicbox->size_list |= DBOX_SIZE_TYPE_EASY_1x1;
		    update_size_info(dynamicbox, 9, node);
		} else {
		    dynamicbox->size_list |= DBOX_SIZE_TYPE_1x1;
		    update_size_info(dynamicbox, 0, node);
		}
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"3x1")) {
		if (is_easy) {
		    dynamicbox->size_list |= DBOX_SIZE_TYPE_EASY_3x1;
		    update_size_info(dynamicbox, 10, node);
		} else {
		    ErrPrint("Invalid size tag (%s)\n", size);
		}
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"3x3")) {
		if (is_easy) {
		    dynamicbox->size_list |= DBOX_SIZE_TYPE_EASY_3x3;
		    update_size_info(dynamicbox, 11, node);
		} else {
		    ErrPrint("Invalid size tag (%s)\n", size);
		}
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"2x1")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_2x1;
		update_size_info(dynamicbox, 1, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"2x2")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_2x2;
		update_size_info(dynamicbox, 2, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"4x1")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_4x1;
		update_size_info(dynamicbox, 3, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"4x2")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_4x2;
		update_size_info(dynamicbox, 4, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"4x3")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_4x3;
		update_size_info(dynamicbox, 5, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"4x4")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_4x4;
		update_size_info(dynamicbox, 6, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"4x5")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_4x5;
		update_size_info(dynamicbox, 7, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"4x6")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_4x6;
		update_size_info(dynamicbox, 8, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"21x21")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_EASY_1x1;
		update_size_info(dynamicbox, 9, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"23x21")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_EASY_3x1;
		update_size_info(dynamicbox, 10, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"23x23")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_EASY_3x3;
		update_size_info(dynamicbox, 11, node);
	    } else if (!xmlStrcasecmp(size, (const xmlChar *)"0x0")) {
		dynamicbox->size_list |= DBOX_SIZE_TYPE_0x0;
		update_size_info(dynamicbox, 12, node);
	    } else {
		ErrPrint("Invalid size tag (%s)\n", size);
	    }

	    xmlFree(size);
	} else if (!xmlStrcasecmp(node->name, (const xmlChar *)"script")) {
	    xmlChar *src;

	    if (!xmlHasProp(node, (const xmlChar *)"src")) {
		ErrPrint("Invalid script tag. has no src\n");
		continue;
	    }

	    src = xmlGetProp(node, (const xmlChar *)"src");
	    if (!src) {
		ErrPrint("Invalid script tag. src is NIL\n");
		continue;
	    }

	    if (dynamicbox->dbox_src) {
		DbgPrint("Override lb src: %s\n", dynamicbox->dbox_src);
		xmlFree(dynamicbox->dbox_src);
	    }

	    dynamicbox->dbox_src = src;

	    if (xmlHasProp(node, (const xmlChar *)"group")) {
		xmlChar *group;
		group = xmlGetProp(node, (const xmlChar *)"group");
		if (!group) {
		    ErrPrint("Group is NIL\n");
		} else {
		    if (dynamicbox->dbox_group) {
			DbgPrint("Override lb group: %s\n", dynamicbox->dbox_group);
			xmlFree(dynamicbox->dbox_group);
		    }

		    dynamicbox->dbox_group = group;
		}
	    }
	}
    }
}

static inline void update_group(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    xmlNodePtr cluster;
    xmlNodePtr category;
    xmlNodePtr option_item;
    xmlChar *cluster_name;
    xmlChar *category_name;
    xmlChar *ctx_item;

    xmlChar *key;
    xmlChar *value;

    struct group *group;
    struct option *option;

    cluster = node;
    for (cluster = cluster->children; cluster; cluster = cluster->next) {
	if (xmlStrcasecmp(cluster->name, (const xmlChar *)"cluster")) {
	    DbgPrint("Skip: %s\n", cluster->name);
	    continue;
	}

	if (!xmlHasProp(cluster, (const xmlChar *)"name")) {
	    ErrPrint("Invalid cluster, has no name\n");
	    continue;
	}

	cluster_name = xmlGetProp(cluster, (const xmlChar *)"name");
	if (!cluster_name) {
	    ErrPrint("Invalid cluster name. NIL\n");
	    continue;
	}

	for (category = cluster->children; category; category = category->next) {
	    if (xmlStrcasecmp(category->name, (const xmlChar *)"category")) {
		DbgPrint("Skip: %s\n", category->name);
		continue;
	    }

	    if (!xmlHasProp(category, (const xmlChar *)"name")) {
		ErrPrint("Invalid category, has no name\n");
		continue;
	    }

	    category_name = xmlGetProp(category, (const xmlChar *)"name");
	    if (!category_name) {
		ErrPrint("Invalid category name. NIL\n");
		continue;
	    }

	    group = calloc(1, sizeof(*group));
	    if (!group) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree(category_name);
		continue;
	    }

	    group->cluster = xmlStrdup(cluster_name);
	    if (!group->cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		xmlFree(category_name);
		free(group);
		continue;
	    }

	    group->category = category_name;
	    dynamicbox->group_list = dlist_append(dynamicbox->group_list, group);

	    if (!xmlHasProp(category, (const xmlChar *)"context")) {
		DbgPrint("%s, %s has no ctx info\n", group->cluster, group->category);
		continue;
	    }

	    ctx_item = xmlGetProp(category, (const xmlChar *)"context");
	    if (!ctx_item) {
		ErrPrint("Failed to get context ID (%s, %s)\n", group->cluster, group->category);
		continue;
	    }

	    group->ctx_item = ctx_item;
	    DbgPrint("Build group item: %s - %s - %s\n", group->cluster, group->category, group->ctx_item);

	    for (option_item = category->children; option_item; option_item = option_item->next) {
		if (xmlStrcasecmp(option_item->name, (const xmlChar *)"option")) {
		    DbgPrint("Skip: %s\n", option_item->name);
		    continue;
		}

		if (!xmlHasProp(option_item, (const xmlChar *)"key")) {
		    ErrPrint("Invalid option, has no key\n");
		    continue;
		}

		if (!xmlHasProp(option_item, (const xmlChar *)"value")) {
		    ErrPrint("Invalid option, has no value\n");
		    continue;
		}

		key = xmlGetProp(option_item, (const xmlChar *)"key");
		if (!key) {
		    ErrPrint("Invalid key. NIL\n");
		    continue;
		}

		value = xmlGetProp(option_item, (const xmlChar *)"value");
		if (!value) {
		    ErrPrint("Invalid valid. NIL\n");
		    xmlFree(key);
		    continue;
		}

		option = calloc(1, sizeof(*option));
		if (!option) {
		    ErrPrint("Heap: %s\n", strerror(errno));
		    xmlFree(key);
		    xmlFree(value);
		    continue;
		}

		option->key = key;
		option->value = value;

		group->option_list = dlist_append(group->option_list, option);
	    }
	}

	xmlFree(cluster_name);
    }
}

static inline void update_pd(struct dynamicbox *dynamicbox, xmlNodePtr node)
{
    if (!xmlHasProp(node, (const xmlChar *)"type")) {
	dynamicbox->gbar_type = GBAR_TYPE_SCRIPT;
    } else {
	xmlChar *type;

	type = xmlGetProp(node, (const xmlChar *)"type");
	if (!type) {
	    ErrPrint("type is NIL\n");
	    return;
	}

	if (!xmlStrcasecmp(type, (const xmlChar *)"text")) {
	    dynamicbox->gbar_type = GBAR_TYPE_TEXT;
	} else if (!xmlStrcasecmp(type, (const xmlChar *)"buffer")) {
	    dynamicbox->gbar_type = GBAR_TYPE_BUFFER;
	} else if (!xmlStrcasecmp(type, (const xmlChar *)"elm")) {
	    dynamicbox->gbar_type = GBAR_TYPE_UIFW;
	} else {
	    dynamicbox->gbar_type = GBAR_TYPE_SCRIPT;
	}

	xmlFree(type);
    }

    for (node = node->children; node; node = node->next) {
	if (!xmlStrcasecmp(node->name, (const xmlChar *)"size")) {
	    xmlChar *size;

	    size = xmlNodeGetContent(node);
	    if (!size) {
		ErrPrint("Invalid size tag\n");
		continue;
	    }

	    if (dynamicbox->gbar_size) {
		DbgPrint("Override pd size: %s\n", dynamicbox->gbar_size);
		xmlFree(dynamicbox->gbar_size);
	    }
	    dynamicbox->gbar_size = size;
	} else if (!xmlStrcasecmp(node->name, (const xmlChar *)"script")) {
	    xmlChar *src;

	    if (!xmlHasProp(node, (const xmlChar *)"src")) {
		ErrPrint("Invalid script tag, has no src\n");
		continue;
	    }

	    src = xmlGetProp(node, (const xmlChar *)"src");
	    if (!src) {
		ErrPrint("src is NIL\n");
		continue;
	    }

	    if (dynamicbox->gbar_src) {
		DbgPrint("Overide PD src: %s\n", dynamicbox->gbar_src);
		xmlFree(dynamicbox->gbar_src);
	    }

	    dynamicbox->gbar_src = src;

	    if (xmlHasProp(node, (const xmlChar *)"group")) {
		xmlChar *group;
		group = xmlGetProp(node, (const xmlChar *)"group");
		if (!group) {
		    ErrPrint("Group is NIL\n");
		} else {
		    if (dynamicbox->gbar_group) {
			DbgPrint("Override PD group : %s\n", dynamicbox->gbar_group);
			xmlFree(dynamicbox->gbar_group);
		    }

		    dynamicbox->gbar_group = group;
		}
	    }
	}
    }
}

static int db_insert_dynamicbox(struct dynamicbox *dynamicbox, const char *appid)
{
    struct dlist *l;
    struct dlist *il;
    struct i18n *i18n;
    struct group *group;
    int ret;
    int id;
    struct option *option;

    begin_transaction();
    ret = db_insert_pkgmap(appid, (char *)dynamicbox->pkgid, (char *)dynamicbox->uiapp, dynamicbox->primary, (char *)dynamicbox->category);
    if (ret < 0) {
	goto errout;
    }

    ret = db_insert_provider(dynamicbox);
    if (ret < 0) {
	goto errout;
    }

    ret = db_insert_client(dynamicbox);
    if (ret < 0) {
	goto errout;
    }

    dlist_foreach(dynamicbox->i18n_list, l, i18n) {
	ret = db_insert_i18n((char *)dynamicbox->pkgid, (char *)i18n->lang, (char *)i18n->name, (char *)i18n->icon);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_1x1) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_1x1, (char *)dynamicbox->preview[0], dynamicbox->touch_effect[0], dynamicbox->need_frame[0], dynamicbox->mouse_event[0]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_2x1) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_2x1, (char *)dynamicbox->preview[1], dynamicbox->touch_effect[1], dynamicbox->need_frame[1], dynamicbox->mouse_event[1]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_2x2) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_2x2, (char *)dynamicbox->preview[2], dynamicbox->touch_effect[2], dynamicbox->need_frame[2], dynamicbox->mouse_event[2]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_4x1) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_4x1, (char *)dynamicbox->preview[3], dynamicbox->touch_effect[3], dynamicbox->need_frame[3], dynamicbox->mouse_event[3]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_4x2) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_4x2, (char *)dynamicbox->preview[4], dynamicbox->touch_effect[4], dynamicbox->need_frame[4], dynamicbox->mouse_event[4]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_4x3) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_4x3, (char *)dynamicbox->preview[5], dynamicbox->touch_effect[5], dynamicbox->need_frame[5], dynamicbox->mouse_event[5]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_4x4) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_4x4, (char *)dynamicbox->preview[6], dynamicbox->touch_effect[6], dynamicbox->need_frame[6], dynamicbox->mouse_event[6]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_4x5) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_4x5, (char *)dynamicbox->preview[7], dynamicbox->touch_effect[7], dynamicbox->need_frame[7], dynamicbox->mouse_event[7]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_4x6) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_4x6, (char *)dynamicbox->preview[8], dynamicbox->touch_effect[8], dynamicbox->need_frame[8], dynamicbox->mouse_event[8]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_EASY_1x1) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_EASY_1x1, (char *)dynamicbox->preview[9], dynamicbox->touch_effect[9], dynamicbox->need_frame[9], dynamicbox->mouse_event[9]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_EASY_3x1) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_EASY_3x1, (char *)dynamicbox->preview[10], dynamicbox->touch_effect[10], dynamicbox->need_frame[10], dynamicbox->mouse_event[10]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_EASY_3x3) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_EASY_3x3, (char *)dynamicbox->preview[11], dynamicbox->touch_effect[11], dynamicbox->need_frame[11], dynamicbox->mouse_event[11]);
	if (ret < 0) {
	    goto errout;
	}
    }

    if (dynamicbox->size_list & DBOX_SIZE_TYPE_0x0) {
	ret = db_insert_box_size((char *)dynamicbox->pkgid, DBOX_SIZE_TYPE_0x0, (char *)dynamicbox->preview[12], dynamicbox->touch_effect[12], dynamicbox->need_frame[12], dynamicbox->mouse_event[12]);
	if (ret < 0) {
	    goto errout;
	}
    }

    dlist_foreach(dynamicbox->group_list, l, group) {
	/* group ID "id" */
	id = db_get_group_id((char *)group->cluster, (char *)group->category);
	if (id < 0) {
	    int ret;

	    ret = db_insert_group((char *)dynamicbox->pkgid, (char *)group->cluster, (char *)group->category);
	    if (ret < 0) {
		ErrPrint("[%s]-[%s] is not exists\n", group->cluster, group->category);
		continue;
	    }

	    DbgPrint("New group name is built - %s/%s\n", group->cluster, group->category);
	    id = db_get_group_id((char *)group->cluster, (char *)group->category);
	    if (id < 0) {
		ErrPrint("Failed to get group id for %s/%s\n", group->cluster, group->category);
		continue;
	    }
	}

	if (!group->ctx_item) {
	    DbgPrint("%s, %s - has no ctx info\n", group->cluster, group->category);
	    continue;
	}

	ret = db_insert_groupmap(id, (char *)dynamicbox->pkgid, (char *)group->ctx_item);
	if (ret < 0) {
	    goto errout;
	}

	/* REUSE "id" from here , option ID */
	id = db_get_option_id(id, (char *)dynamicbox->pkgid, (char *)group->ctx_item);
	if (id < 0) {
	    goto errout;
	}

	dlist_foreach(group->option_list, il, option) {
	    ret = db_insert_option((char *)dynamicbox->pkgid, id, (char *)option->key, (char *)option->value);
	    if (ret < 0) {
		goto errout;
	    }
	}
    }

    commit_transaction();
    dynamicbox_destroy(dynamicbox);
    return 0;

errout:
    ErrPrint("ROLLBACK\n");
    rollback_transaction();
    dynamicbox_destroy(dynamicbox);
    return ret;
}

static int do_install(xmlNodePtr node, const char *appid)
{
    struct dynamicbox *dynamicbox;
    xmlChar *pkgid;
    xmlChar *tmp;

    if (!xmlHasProp(node, (const xmlChar *)"appid")) {
	ErrPrint("Missing appid\n");
	return -EINVAL;
    }

    pkgid = xmlGetProp(node, (const xmlChar *)"appid");
    if (!pkgid || !validate_pkgid(appid, (char *)pkgid)) {
	ErrPrint("Invalid appid\n");
	xmlFree(pkgid);
	return -EINVAL;
    }

    DbgPrint("appid: %s\n", (char *)pkgid);

    dynamicbox = calloc(1, sizeof(*dynamicbox));
    if (!dynamicbox) {
	ErrPrint("Heap: %s\n", strerror(errno));
	xmlFree(pkgid);
	return -ENOMEM;
    }

    dynamicbox->pkgid = pkgid;

    if (xmlHasProp(node, (const xmlChar *)"count")) {
	tmp = xmlGetProp(node, (const xmlChar *)"count");
	if (sscanf((const char *)tmp, "%d", &dynamicbox->count) != 1) {
	    ErrPrint("Invalid syntax: %s\n", (const char *)tmp);
	}
	xmlFree(tmp);
    }

    if (xmlHasProp(node, (const xmlChar *)"primary")) {
	tmp = xmlGetProp(node, (const xmlChar *)"primary");
	dynamicbox->primary = !xmlStrcasecmp(tmp, (const xmlChar *)"true");
	xmlFree(tmp);
    }

    if (xmlHasProp(node, (const xmlChar *)"script")) {
	dynamicbox->script = xmlGetProp(node, (const xmlChar *)"script");
	if (!dynamicbox->script) {
	    ErrPrint("script is NIL\n");
	}
    }

    if (xmlHasProp(node, (const xmlChar *)"nodisplay")) {
	tmp = xmlGetProp(node, (const xmlChar *)"nodisplay");
	dynamicbox->nodisplay = tmp && !xmlStrcasecmp(tmp, (const xmlChar *)"true");
	xmlFree(tmp);
    }

    if (xmlHasProp(node, (const xmlChar *)"pinup")) {
	tmp = xmlGetProp(node, (const xmlChar *)"pinup");
	dynamicbox->pinup = tmp && !xmlStrcasecmp(tmp, (const xmlChar *)"true");
	xmlFree(tmp);
    }

    if (xmlHasProp(node, (const xmlChar *)"period")) {
	dynamicbox->period = xmlGetProp(node, (const xmlChar *)"period");
	if (!dynamicbox->period) {
	    ErrPrint("Period is NIL\n");
	}
    }

    if (xmlHasProp(node, (const xmlChar *)"timeout")) {
	dynamicbox->timeout = xmlGetProp(node, (const xmlChar *)"timeout");
	if (!dynamicbox->timeout) {
	    ErrPrint("Timeout is NIL\n");
	}
    }

    if (xmlHasProp(node, (const xmlChar *)"secured")) {
	tmp = xmlGetProp(node, (const xmlChar *)"secured");
	dynamicbox->secured = tmp && !xmlStrcasecmp(tmp, (const xmlChar *)"true");
	xmlFree(tmp);
    }

    if (xmlHasProp(node, (const xmlChar *)"network")) {
	tmp = xmlGetProp(node, (const xmlChar *)"network");
	dynamicbox->network = tmp && !xmlStrcasecmp(tmp, (const xmlChar *)"true");
	xmlFree(tmp);
    }

    if (xmlHasProp(node, (const xmlChar *)"direct_input")) {
	tmp = xmlGetProp(node, (const xmlChar *)"direct_input");
	dynamicbox->direct_input = tmp && !xmlStrcasecmp(tmp, (const xmlChar *)"true");
	xmlFree(tmp);
    }

    if (xmlHasProp(node, (const xmlChar *)"hw-acceleration")) {
	dynamicbox->hw_acceleration = xmlGetProp(node, (const xmlChar *)"hw-acceleration");
	if (!dynamicbox->hw_acceleration) {
	    ErrPrint("hw-acceleration is NIL\n");
	}
    }

    if (xmlHasProp(node, (const xmlChar *)"abi")) {
	dynamicbox->abi = xmlGetProp(node, (const xmlChar *)"abi");
	if (!dynamicbox->abi) {
	    ErrPrint("ABI is NIL\n");
	    dynamicbox_destroy(dynamicbox);
	    return -EFAULT;
	}
    } else {
	dynamicbox->abi = xmlStrdup((const xmlChar *)"c");
	if (!dynamicbox->abi) {
	    ErrPrint("Heap: %s\n", strerror(errno));
	    dynamicbox_destroy(dynamicbox);
	    return -ENOMEM;
	}
    }

    if (xmlHasProp(node, (const xmlChar *)"libexec")) {
	dynamicbox->libexec = xmlGetProp(node, (const xmlChar *)"libexec");
	if (!dynamicbox->libexec) {
	    ErrPrint("libexec is NIL\n");
	    dynamicbox_destroy(dynamicbox);
	    return -EFAULT;
	}
    } else if (!xmlStrcasecmp(dynamicbox->abi, (const xmlChar *)"c") || !xmlStrcasecmp(dynamicbox->abi, (const xmlChar *)"cpp")) {
	char *filename;
	int len;

	len = strlen((char *)dynamicbox->pkgid) + strlen("/libexec/liblive-.so") + 1;

	filename = malloc(len);
	if (!filename) {
	    dynamicbox_destroy(dynamicbox);
	    return -ENOMEM;
	}

	snprintf(filename, len, "/libexec/liblive-%s.so", dynamicbox->pkgid);
	dynamicbox->libexec = xmlStrdup((xmlChar *)filename);
	DbgPrint("Use the default libexec: %s\n", filename);
	free(filename);

	if (!dynamicbox->libexec) {
	    dynamicbox_destroy(dynamicbox);
	    return -ENOMEM;
	}
    }

    for (node = node->children; node; node = node->next) {
	if (!xmlStrcmp(node->name, (const xmlChar *)"text")) {
	    continue;
	}

	DbgPrint("Nodename: %s\n", node->name);
	if (!xmlStrcasecmp(node->name, (const xmlChar *)"label")) {
	    update_i18n_name(dynamicbox, node);
	    continue;
	}

	if (!xmlStrcasecmp(node->name, (const xmlChar *)"icon")) {
	    update_i18n_icon(dynamicbox, node);
	    continue;
	}

	if (!xmlStrcasecmp(node->name, (const xmlChar *)"box")) {
	    update_box(dynamicbox, node);
	    continue;
	}

	if (!xmlStrcasecmp(node->name, (const xmlChar *)"glancebar")) {
	    update_pd(dynamicbox, node);
	    continue;
	}

	if (!xmlStrcasecmp(node->name, (const xmlChar *)"group")) {
	    update_group(dynamicbox, node);
	    continue;
	}

	if (!xmlStrcasecmp(node->name, (const xmlChar *)"content")) {
	    update_content(dynamicbox, node);
	    continue;
	}

	if (!xmlStrcasecmp(node->name, (const xmlChar *)"setup")) {
	    update_setup(dynamicbox, node);
	    continue;
	}

	if (!xmlStrcasecmp(node->name, (const xmlChar *)"launch")) {
	    update_launch(dynamicbox, node);
	    continue;
	}

	if (!xmlStrcasecmp(node->name, (const xmlChar *)"ui-appid")) {
	    update_ui_appid(dynamicbox, node);
	    continue;
	}

	if (!xmlStrcasecmp(node->name, (const xmlChar *)"category")) {
	    update_category(dynamicbox, node);
	    continue;
	}
    }

    return db_insert_dynamicbox(dynamicbox, appid);
}

static inline int do_uninstall(xmlNodePtr node, const char *appid)
{
    xmlChar *pkgid;
    int ret;

    if (!xmlHasProp(node, (const xmlChar *)"appid")) {
	ErrPrint("Missing appid\n");
	return -EINVAL;
    }

    pkgid = xmlGetProp(node, (const xmlChar *)"appid");
    if (!validate_pkgid(appid, (char *)pkgid)) {
	ErrPrint("Invalid package\n");
	xmlFree(pkgid);
	return -EINVAL;
    }

    begin_transaction();
    ret = db_remove_box_size((char *)pkgid);
    if (ret < 0) {
	goto errout;
    }

    ret = db_remove_i18n((char *)pkgid);
    if (ret < 0) {
	goto errout;
    }

    ret = db_remove_client((char *)pkgid);
    if (ret < 0) {
	goto errout;
    }

    ret = db_remove_provider((char *)pkgid);
    if (ret < 0) {
	goto errout;
    }

    ret = db_remove_option((char *)pkgid);
    DbgPrint("Remove option: %d\n", ret);

    ret = db_remove_groupmap((char *)pkgid);
    DbgPrint("Remove groupmap: %d\n", ret);

    ret = db_remove_group((char *)pkgid);
    if (ret < 0) {
	goto errout;
    }

    ret = db_remove_pkgmap((char *)pkgid);
    if (ret < 0) {
	goto errout;
    }

    commit_transaction();
    xmlFree(pkgid);

    return 0;

errout:
    rollback_transaction();
    xmlFree(pkgid);
    return ret;
}

static int pkglist_get_via_callback(const char *appid, void (*cb)(const char *appid, const char *pkgid, int prime, void *data), void *data)
{
    const char *dml = "SELECT pkgid, prime FROM pkgmap WHERE appid = ?";
    int ret;
    sqlite3_stmt *stmt;
    const char *pkgid;
    int prime;
    int cnt = 0;

    if (!cb || !appid || !strlen(appid)) {
	return -EINVAL;
    }

    if (!s_info.handle) {
	if (db_init() < 0) {
	    ErrPrint("Failed to init DB\n");
	    return -EIO;
	}
    }

    ret = sqlite3_prepare_v2(s_info.handle, dml, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
	ErrPrint("Failed to prepare the intial DML(%s)\n", sqlite3_errmsg(s_info.handle));
	return -EIO;
    }

    ret = -EIO;
    if (sqlite3_bind_text(stmt, 1, appid, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
	ErrPrint("Failed to bind a cluster - %s\n", sqlite3_errmsg(s_info.handle));
	goto out;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
	pkgid = (const char *)sqlite3_column_text(stmt, 0);
	if (!pkgid || !strlen(pkgid)) {
	    continue;
	}

	prime = sqlite3_column_int(stmt, 1);
	cb(appid, pkgid, prime, data);
	cnt++;
    }

out:
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_finalize(stmt);
    return cnt;
}

static void clear_all_pkg(const char *appid, const char *pkgid, int prime, void *data)
{
    int ret;

    ErrPrintWithConsole("Remove old package info: appid(%s), pkgid(%s)\n", appid, pkgid);

    ret = db_remove_box_size((char *)pkgid);
    if (ret < 0) {
	ErrPrint("Remove box size: %d\n", ret);
    }

    ret = db_remove_i18n((char *)pkgid);
    if (ret < 0) {
	ErrPrint("Remove i18n: %d\n", ret);
    }

    ret = db_remove_client((char *)pkgid);
    if (ret < 0) {
	ErrPrint("Remove client: %d\n", ret);
    }

    ret = db_remove_provider((char *)pkgid);
    if (ret < 0) {
	ErrPrint("Remove provider: %d\n", ret);
    }

    ret = db_remove_option((char *)pkgid);
    if (ret < 0) {
	ErrPrint("Remove option: %d\n", ret);
    }

    ret = db_remove_groupmap((char *)pkgid);
    if (ret < 0) {
	ErrPrint("Remove groupmap: %d\n", ret);
    }

    ret = db_remove_group((char *)pkgid);
    if (ret < 0) {
	ErrPrint("Remove group: %d\n", ret);
    }

    ret = db_remove_pkgmap((char *)pkgid);
    if (ret < 0) {
	ErrPrint("Remove pkgmap: %d\n", ret);
    }
}

int PKGMGR_PARSER_PLUGIN_PRE_INSTALL(const char *appid)
{
    int cnt;

    ErrPrintWithConsole("%s\n", appid);

    if (!s_info.handle) {
	if (db_init() < 0) {
	    ErrPrintWithConsole("Failed to init DB\n");
	    return -EIO;
	}
    }

    do_upgrade_db_schema();

    begin_transaction();
    cnt = pkglist_get_via_callback(appid, clear_all_pkg, NULL);
    commit_transaction();

    if (cnt > 0) {
	DbgPrint("Package[%s] is not deleted: %d\n", appid, cnt);
    }
    return 0;
}

int PKGMGR_PARSER_PLUGIN_POST_INSTALL(const char *appid)
{
    ErrPrintWithConsole("[%s]\n", appid);
    db_fini();
    return 0;
}

int PKGMGR_PARSER_PLUGIN_INSTALL(xmlDocPtr docPtr, const char *appid)
{
    xmlNodePtr node;
    int ret;

    ErrPrintWithConsole("[%s]\n", appid);

    if (!s_info.handle) {
	ErrPrintWithConsole("Failed to init DB\n");
	return -EIO;
    }

    node = xmlDocGetRootElement(docPtr);
    if (!node) {
	ErrPrintWithConsole("Invalid document\n");
	return -EINVAL;
    }

    for (node = node->children; node; node = node->next) {
	DbgPrint("node->name: %s\n", node->name);
	if (!xmlStrcasecmp(node->name, (const xmlChar *)"dynamicbox")) {
	    ret = do_install(node, appid);
	    if (ret < 0) {
		DbgPrint("Returns: %d\n", ret);
	    }
	}
    }

    return 0;
}

int PKGMGR_PARSER_PLUGIN_PRE_UPGRADE(const char *appid)
{
    int cnt;

    ErrPrintWithConsole("[%s]\n", appid);

    if (!s_info.handle) {
	if (db_init() < 0) {
	    ErrPrint("Failed to init DB\n");
	    return -EIO;
	}
    }

    do_upgrade_db_schema();

    begin_transaction();
    cnt = pkglist_get_via_callback(appid, clear_all_pkg, NULL);
    commit_transaction();

    if (cnt > 0) {
	DbgPrint("Package %s is deleted: %d\n", appid, cnt);
    }
    return 0;
}

int PKGMGR_PARSER_PLUGIN_POST_UPGRADE(const char *appid)
{
    ErrPrintWithConsole("[%s]\n", appid);
    db_fini();
    return 0;
}

int PKGMGR_PARSER_PLUGIN_UPGRADE(xmlDocPtr docPtr, const char *appid)
{
    xmlNodePtr node;
    int ret;

    ErrPrintWithConsole("[%s]\n", appid);

    if (!s_info.handle) {
	ErrPrint("Failed to init DB\n");
	return -EIO;
    }

    node = xmlDocGetRootElement(docPtr);
    if (!node) {
	ErrPrint("Invalid document\n");
	return -EINVAL;
    }

    for (node = node->children; node; node = node->next) {
	if (!xmlStrcasecmp(node->name, (const xmlChar *)"dynamicbox")) {
	    ret = do_install(node, appid);
	    if (ret < 0) {
		DbgPrint("Returns: %d\n", ret);
	    }
	}
    }

    return 0;
}

int PKGMGR_PARSER_PLUGIN_PRE_UNINSTALL(const char *appid)
{
    ErrPrintWithConsole("[%s]\n", appid);

    if (!s_info.handle) {
	if (db_init() < 0) {
	    ErrPrint("Failed to init DB\n");
	    return -EIO;
	}
    }

    do_upgrade_db_schema();
    return 0;
}

int PKGMGR_PARSER_PLUGIN_POST_UNINSTALL(const char *appid)
{
    int cnt;

    ErrPrintWithConsole("[%s]\n", appid);

    if (!s_info.handle) {
	return -EIO;
    }

    begin_transaction();
    cnt = pkglist_get_via_callback(appid, clear_all_pkg, NULL);
    commit_transaction();

    if (cnt > 0) {
	DbgPrint("Package %s is deleted: %d\n", appid, cnt);
    }
    db_fini();
    return 0;
}

int PKGMGR_PARSER_PLUGIN_UNINSTALL(xmlDocPtr docPtr, const char *appid)
{
    ErrPrintWithConsole("[%s]\n", appid);
    if (!s_info.handle) {
	return -EIO;
    }
    /* Doesn't need to do anything from here, we already dealt it with this */
    return 0;
}

/* End of a file */
