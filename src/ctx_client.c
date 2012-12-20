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

#include <stdio.h>
#include <unistd.h>

#include <Ecore.h>
#include <gio/gio.h>

#include <vconf.h>
#include <dlog.h>

#include <packet.h>

#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "package.h"
#include "group.h"
#include "conf.h"
#include "util.h"
#include "rpc_to_slave.h"
#include "setting.h"
#include "ctx_wrapper.h"
#include "xmonitor.h"

static struct info {
	Eina_List *event_list;
} s_info = {
	.event_list = NULL,
};

struct pended_ctx_info {
	char *cluster;
	char *category;
	char *pkgname;
};

static inline void processing_ctx_event(const char *cluster, const char *category, const char *pkgname)
{
	slave_rpc_request_update(pkgname, "", cluster, category);
	if (util_free_space(IMAGE_PATH) > MINIMUM_SPACE) {
		if (client_nr_of_subscriber(cluster, category) > 0) {
			double timestamp;
			struct inst_info *inst;

			timestamp = util_timestamp();
			inst = instance_create(NULL, timestamp, pkgname, DEFAULT_CONTENT, cluster, category, DEFAULT_PERIOD, 0, 0);
			if (!inst)
				ErrPrint("Failed to create an instance (%s / %s - %s)\n", cluster, category, pkgname);
		} else {
			DbgPrint("No subscribed clients. Ignore ctx event (%s / %s - %s)\n", cluster, category, pkgname);
		}
	} else {
		ErrPrint("Not enough space\n");
	}

	DbgPrint("Context event is updated\n");
}

static inline int is_already_pended(const char *c_name, const char *s_name, const char *pkgname)
{
	Eina_List *l;
	struct pended_ctx_info *info;

	EINA_LIST_FOREACH(s_info.event_list, l, info) {
		if (strcmp(pkgname, info->pkgname))
			continue;

		if (strcmp(s_name, info->category))
			continue;

		if (strcmp(c_name, info->cluster))
			continue;

		return 1;
	}

	return 0;
}

static inline void push_pended_item(const char *c_name, const char *s_name, const char *pkgname)
{
	struct pended_ctx_info *pending_item;

	if (eina_list_count(s_info.event_list) >= MAX_PENDED_CTX_EVENTS) {
		ErrPrint("Reach to count of a maximum pended ctx events\n");
		return;
	}

	pending_item = malloc(sizeof(*pending_item));
	if (!pending_item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return;
	}

	pending_item->cluster = strdup(c_name);
	if (!pending_item->cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(pending_item);
		return;
	}

	pending_item->category = strdup(s_name);
	if (!pending_item->category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(pending_item->cluster);
		DbgFree(pending_item);
		return;
	}

	pending_item->pkgname = strdup(pkgname);
	if (!pending_item->pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(pending_item->cluster);
		DbgFree(pending_item->category);
		DbgFree(pending_item);
		return;
	}

	s_info.event_list = eina_list_append(s_info.event_list, pending_item);
	ErrPrint("Context event is pended (%s/%s - %s)\n", c_name, s_name, pkgname);
}

static int ctx_changed_cb(struct context_item *item, void *user_data)
{
	const char *c_name;
	const char *s_name;
	const char *pkgname;
	struct context_info *info;
	struct category *category;

	info = group_context_info_from_item(item);
	if (!info) {
		ErrPrint("Context info is not valid (%p)\n", item);
		return 0;
	}

	category = group_category_from_context_info(info);
	if (!category) {
		ErrPrint("Category info is not valid: %p\n", info);
		return 0;
	}

	c_name = group_cluster_name_by_category(category);
	s_name = group_category_name(category);
	pkgname = group_pkgname_from_context_info(info);

	if (!c_name || !s_name || !pkgname) {
		ErrPrint("Name is not valid (%s/%s/%s)\n", c_name, s_name, pkgname);
		return 0;
	}

	if (xmonitor_is_paused()) {
		if (!is_already_pended(c_name, s_name, pkgname)) {
			push_pended_item(c_name, s_name, pkgname);
		} else {
			DbgPrint("Already pended event : %s %s / %s\n", c_name, s_name, pkgname);
		}
	} else {
		processing_ctx_event(c_name, s_name, pkgname);
	}

	return 0;
}

static inline void enable_event_handler(struct context_info *info)
{
	Eina_List *l;
	Eina_List *item_list;
	struct context_item *item;

	item_list = group_context_item_list(info);
	EINA_LIST_FOREACH(item_list, l, item) {
		void *handler;

		handler = group_context_item_data(item, "callback");
		if (handler) {
			ErrPrint("Already registered ctx callback\n");
			continue;
		}

		handler = ctx_wrapper_register_callback(item, ctx_changed_cb, NULL);
		if (group_context_item_add_data(item, "callback", handler) < 0)
			ctx_wrapper_unregister_callback(handler);
	}
}

static inline void register_callbacks(void)
{
	Eina_List *cluster_list;
	Eina_List *l1;
	struct cluster *cluster;

	Eina_List *category_list;
	Eina_List *l2;
	struct category *category;

	Eina_List *info_list;
	Eina_List *l3;
	struct context_info *info;

	cluster_list = group_cluster_list();
	EINA_LIST_FOREACH(cluster_list, l1, cluster) {
		category_list = group_category_list(cluster);
		EINA_LIST_FOREACH(category_list, l2, category) {
			info_list = group_context_info_list(category);
			EINA_LIST_FOREACH(info_list, l3, info) {
				enable_event_handler(info);
			} // info
		} // category
	} // cluster
}

HAPI int ctx_enable_event_handler(struct context_info *info)
{
	int enabled;

	if (vconf_get_int(SYS_CLUSTER_KEY, &enabled) < 0)
		enabled = 0;

	if (!enabled) {
		DbgPrint("CTX in not enabled\n");
		return 0;
	}

	enable_event_handler(info);
	return 0;
}

HAPI int ctx_disable_event_handler(struct context_info *info)
{
	Eina_List *l;
	Eina_List *item_list;
	struct context_item *item;

	item_list = group_context_item_list(info);
	EINA_LIST_FOREACH(item_list, l, item) {
		void *handler;
		handler = group_context_item_del_data(item, "callback");
		if (handler)
			ctx_wrapper_unregister_callback(handler);
	}

	return 0;
}

static inline void unregister_callbacks(void)
{
	Eina_List *cluster_list;
	Eina_List *l1;
	struct cluster *cluster;

	Eina_List *category_list;
	Eina_List *l2;
	struct category *category;

	Eina_List *info_list;
	Eina_List *l3;
	struct context_info *info;

	cluster_list = group_cluster_list();
	EINA_LIST_FOREACH(cluster_list, l1, cluster) {
		category_list = group_category_list(cluster);
		EINA_LIST_FOREACH(category_list, l2, category) {
			info_list = group_context_info_list(category);
			EINA_LIST_FOREACH(info_list, l3, info) {
				ctx_disable_event_handler(info);
			} // info
		} // category
	} // cluster
}

static void ctx_vconf_cb(keynode_t *node, void *data)
{
	int enabled;

	if (!node) {
		/*!< Enable this for default option */
		if (vconf_get_int(SYS_CLUSTER_KEY, &enabled) < 0)
			enabled = 0;
	} else {
		enabled = vconf_keynode_get_int(node);
	}

	if (!enabled) {
		unregister_callbacks();
		ctx_wrapper_disable();
		return;
	}

	ctx_wrapper_enable();
	register_callbacks();
}

static int xmonitor_pause_cb(void *data)
{
	DbgPrint("XMonitor Paused: do nothing\n");
	return 0;
}

static int xmonitor_resume_cb(void *data)
{
	struct pended_ctx_info *item;

	EINA_LIST_FREE(s_info.event_list, item) {
		DbgPrint("Pended ctx event for %s - %s / %s\n", item->cluster, item->category, item->pkgname);
		processing_ctx_event(item->cluster, item->category, item->pkgname);

		DbgFree(item->cluster);
		DbgFree(item->category);
		DbgFree(item->pkgname);
		DbgFree(item);
	}

	return 0;
}

HAPI int ctx_client_init(void)
{
	int ret;

	xmonitor_add_event_callback(XMONITOR_PAUSED, xmonitor_pause_cb, NULL);
	xmonitor_add_event_callback(XMONITOR_RESUMED, xmonitor_resume_cb, NULL);

	ret = vconf_notify_key_changed(SYS_CLUSTER_KEY, ctx_vconf_cb, NULL);
	if (ret < 0)
		ErrPrint("Failed to register the system_cluster vconf\n");

	ctx_vconf_cb(NULL, NULL);
	return 0;
}

HAPI int ctx_client_fini(void)
{
	vconf_ignore_key_changed(SYS_CLUSTER_KEY, ctx_vconf_cb);

	xmonitor_del_event_callback(XMONITOR_PAUSED, xmonitor_pause_cb, NULL);
	xmonitor_del_event_callback(XMONITOR_RESUMED, xmonitor_resume_cb, NULL);
	return 0;
}

/* End of a file */
