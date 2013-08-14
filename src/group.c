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
#include <ctype.h>
#include <stdlib.h> /* malloc */
#include <errno.h>
#include <string.h> /* strdup */

#include <dlog.h>
#include <Eina.h>
#include <livebox-errno.h>

#include "util.h"
#include "debug.h"
#include "group.h"
#include "conf.h"

int errno;

static struct info {
	Eina_List *cluster_list;
} s_info = {
	.cluster_list = NULL,
};

struct cluster {
	char *name;
	Eina_List *category_list;
};

struct category {
	char *name;
	struct cluster *cluster;
	Eina_List *info_list; /* list of instances of the struct inst_info */
};

struct context_info {
	char *pkgname;
	struct category *category;
	Eina_List *context_list; /* context item list */
};

struct context_item_data {
	char *tag;
	void *data;
};

struct context_item {
	char *ctx_item;
	struct context_info *info;
	Eina_List *option_list;
	Eina_List *data_list;
};

struct context_option {
	struct context_item *item;
	char *key;
	char *value;
};

HAPI struct context_info *group_create_context_info(struct category *category, const char *pkgname)
{
	struct context_info *info;

	info = calloc(1, sizeof(*info));
	if (!info) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	info->pkgname = strdup(pkgname);
	if (!info->pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(info);
		return NULL;
	}

	info->category = category;
	category->info_list = eina_list_append(category->info_list, info);
	return info;
}

static inline void del_options(struct context_item *item)
{
	struct context_option *option;

	EINA_LIST_FREE(item->option_list, option) {
		DbgFree(option->key);
		DbgFree(option->value);
		DbgFree(option);
	}
}

static inline void del_context_item(struct context_info *info)
{
	struct context_item *item;

	EINA_LIST_FREE(info->context_list, item) {
		del_options(item);
		DbgFree(item->ctx_item);
		DbgFree(item);
	}
}

HAPI struct context_item *group_add_context_item(struct context_info *info, const char *ctx_item)
{
	struct context_item *item;

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	item->ctx_item = strdup(ctx_item);
	if (!item->ctx_item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item);
		return NULL;
	}

	item->info = info;
	info->context_list = eina_list_append(info->context_list, item);
	return item;
}

HAPI int group_add_option(struct context_item *item, const char *key, const char *value)
{
	struct context_option *option;

	option = calloc(1, sizeof(*option));
	if (!option) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	option->key = strdup(key);
	if (!option->key) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(option);
		return LB_STATUS_ERROR_MEMORY;
	}

	option->value = strdup(value);
	if (!option->value) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(option->key);
		DbgFree(option);
		return LB_STATUS_ERROR_MEMORY;
	}

	option->item = item;
	item->option_list = eina_list_append(item->option_list, option);
	return LB_STATUS_SUCCESS;
}

HAPI int group_destroy_context_info(struct context_info *info)
{
	struct category *category;

	category = info->category;
	if (!category) {
		ErrPrint("No category found\n");
		return LB_STATUS_ERROR_INVALID;
	}

	category->info_list = eina_list_remove(category->info_list, info);

	del_context_item(info);
	DbgFree(info->pkgname);
	DbgFree(info);
	return LB_STATUS_SUCCESS;
}

HAPI struct cluster *group_create_cluster(const char *name)
{
	struct cluster *cluster;

	cluster = malloc(sizeof(*cluster));
	if (!cluster) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	cluster->name = strdup(name);
	if (!cluster->name) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(cluster);
		return NULL;
	}

	cluster->category_list = NULL;

	s_info.cluster_list = eina_list_append(s_info.cluster_list, cluster);
	return cluster;
}

HAPI struct cluster *group_find_cluster(const char *name)
{
	Eina_List *l;
	struct cluster *cluster;

	EINA_LIST_FOREACH(s_info.cluster_list, l, cluster) {
		if (!strcasecmp(cluster->name, name)) {
			return cluster;
		}
	}

	return NULL;
}

HAPI struct category *group_create_category(struct cluster *cluster, const char *name)
{
	struct category *category;

	category = malloc(sizeof(*category));
	if (!category) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	category->name = strdup(name);
	if (!category->name) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(category);
		return NULL;
	}

	category->cluster = cluster;
	category->info_list = NULL;

	cluster->category_list = eina_list_append(cluster->category_list, category);
	return category;
}

static inline void destroy_cluster(struct cluster *cluster)
{
	struct category *category;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(cluster->category_list, l, n, category) {
		group_destroy_category(category);
	}

	DbgFree(cluster->name);
	DbgFree(cluster);
}

HAPI int group_destroy_cluster(struct cluster *cluster)
{
	Eina_List *l;
	Eina_List *n;
	struct cluster *item;

	EINA_LIST_FOREACH_SAFE(s_info.cluster_list, l, n, item) {
		if (item == cluster) {
			s_info.cluster_list = eina_list_remove_list(s_info.cluster_list, l);
			destroy_cluster(cluster);
			return LB_STATUS_SUCCESS;
		}
	}

	return LB_STATUS_ERROR_NOT_EXIST;
}

static inline void destroy_category(struct category *category)
{
	Eina_List *l;
	Eina_List *n;
	struct context_info *info;

	EINA_LIST_FOREACH_SAFE(category->info_list, l, n, info) {
		group_destroy_context_info(info);
	}

	DbgFree(category->name);
	DbgFree(category);
}

HAPI int group_destroy_category(struct category *category)
{
	struct cluster *cluster;

	cluster = category->cluster;
	if (cluster) {
		cluster->category_list = eina_list_remove(cluster->category_list, category);
	}

	destroy_category(category);
	return LB_STATUS_SUCCESS;
}

HAPI struct category *group_find_category(struct cluster *cluster, const char *name)
{
	struct category *category;
	Eina_List *l;

	EINA_LIST_FOREACH(cluster->category_list, l, category) {
		if (!strcasecmp(category->name, name)) {
			return category;
		}
	}

	return NULL;
}

HAPI Eina_List * const group_context_info_list(struct category *category)
{
	return category->info_list;
}

HAPI Eina_List *const group_context_item_list(struct context_info *info)
{
	return info->context_list;
}

HAPI Eina_List *const group_context_option_list(struct context_item *item)
{
	return item->option_list;
}

HAPI Eina_List *const group_cluster_list(void)
{
	return s_info.cluster_list;
}

HAPI Eina_List * const group_category_list(struct cluster *cluster)
{
	return cluster->category_list;
}

HAPI struct context_info * const group_context_info_from_item(struct context_item *item)
{
	return item->info;
}

HAPI struct category * const group_category_from_context_info(struct context_info *info)
{
	return info->category;
}

HAPI const char * const group_pkgname_from_context_info(struct context_info *info)
{
	return info->pkgname;
}

HAPI const char * const group_option_item_key(struct context_option *option)
{
	return option->key;
}

HAPI const char * const group_option_item_value(struct context_option *option)
{
	return option->value;
}

HAPI const char * const group_context_item(struct context_item *item)
{
	return item->ctx_item;
}

HAPI int group_context_item_add_data(struct context_item *item, const char *tag, void *data)
{
	struct context_item_data *tmp;

	tmp = malloc(sizeof(*tmp));
	if (!tmp) {
		return LB_STATUS_ERROR_MEMORY;
	}

	tmp->tag = strdup(tag);
	if (!tmp->tag) {
		DbgFree(tmp);
		return LB_STATUS_ERROR_MEMORY;
	}

	tmp->data = data;
	item->data_list = eina_list_append(item->data_list, tmp);
	return LB_STATUS_SUCCESS;
}

HAPI void *group_context_item_data(struct context_item *item, const char *tag)
{
	struct context_item_data *tmp;
	Eina_List *l;

	EINA_LIST_FOREACH(item->data_list, l, tmp) {
		if (!strcmp(tmp->tag, tag)) {
			return tmp->data;
		}
	}

	return NULL;
}

HAPI void *group_context_item_del_data(struct context_item *item, const char *tag)
{
	struct context_item_data *tmp;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(item->data_list, l, n, tmp) {
		if (!strcmp(tmp->tag, tag)) {
			void *data;

			item->data_list = eina_list_remove(item->data_list, tmp);

			data = tmp->data;

			DbgFree(tmp->tag);
			DbgFree(tmp);

			return data;
		}
	}

	return NULL;
}

HAPI const char * const group_category_name(struct category *category)
{
	return category ? category->name : NULL;
}

HAPI const char * const group_cluster_name(struct cluster *cluster)
{
	return cluster ? cluster->name : NULL;
}

HAPI const char *group_cluster_name_by_category(struct category *category)
{
	return !category ? NULL : (category->cluster ? category->cluster->name : NULL);
}

static inline char *get_token(char *ptr, int *len)
{
	char *name;
	int _len;

	if (*len == 0) {
		ErrPrint("Start brace but len = 0\n");
		return NULL;
	}

	_len = *len;

	while (_len > 0 && isspace(ptr[_len])) {
		_len--;
	}

	if (_len == 0) {
		ErrPrint("Token has no string\n");
		return NULL;
	}

	name = malloc(_len + 1);
	if (!name) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	strncpy(name, ptr - *len, _len);
	name[_len] = '\0';

	*len = _len;
	return name;
}

HAPI int group_add_livebox(const char *group, const char *pkgname)
{
	struct cluster *cluster;
	struct category *category;
	struct context_info *info = NULL;
	struct context_item *item = NULL;
	char *key;
	char *name;
	char *ptr;
	int len;
	int is_open = 0;
	enum {
		CLUSTER,
		CATEGORY,
		CONTEXT_ITEM,
		CONTEXT_OPTION_KEY,
		CONTEXT_OPTION_VALUE,
		CONTEXT_ERROR = 0xFFFFFFFF
	} state;

	state = CLUSTER;

	ptr = (char *)group;
	len = 0;
	key = NULL;

	/* Skip the first space characters */
	while (*ptr && isspace(*ptr)) ptr++;

	cluster = NULL;
	while (*ptr) {
		if (*ptr == '{') {
			name = get_token(ptr, &len);
			if (!name) {
				ErrPrint("Failed to get token\n");
				return LB_STATUS_ERROR_FAULT;
			}
			/* cluster{category{context{key=value,key=value},context{key=value}}} */
			/* cluster{category} */

			switch (state) {
			case CLUSTER:
				cluster = group_find_cluster(name);
				if (!cluster) {
					cluster = group_create_cluster(name);
				}

				if (!cluster) {
					ErrPrint("Failed to get cluster\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				state = CATEGORY;
				break;

			case CATEGORY:
				category = group_find_category(cluster, name);
				if (!category) {
					category = group_create_category(cluster, name);
				}

				if (!category) {
					ErrPrint("Failed to get category\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				info = group_create_context_info(category, pkgname);
				if (!info) {
					ErrPrint("Failed to create ctx info\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				state = CONTEXT_ITEM;
				break;

			case CONTEXT_ITEM:
				item = group_add_context_item(info, name);
				if (!item) {
					ErrPrint("Failed to create a context item\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				state = CONTEXT_OPTION_KEY;
				break;

			case CONTEXT_OPTION_KEY:
			case CONTEXT_OPTION_VALUE:
			default:
				ErrPrint("Invalid state\n");
				DbgFree(name);
				return LB_STATUS_ERROR_FAULT;
			}

			DbgFree(name);
			is_open++;
			len = 0;
			ptr++;
			while (*ptr && isspace(*ptr)) ptr++;
			continue;
		} else if (*ptr == ',') {
			name = get_token(ptr, &len);
			if (!name) {
				ErrPrint("Failed to get token (len:%d)\n", len);
				len = 0;
				ptr++;
				while (*ptr && isspace(*ptr)) ptr++;
				continue;
			}

			switch (state) {
			case CLUSTER:
				if (is_open != 0) {
					ErrPrint("Invalid state\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}
				cluster = group_find_cluster(name);
				if (!cluster) {
					cluster = group_create_cluster(name);
				}

				if (!cluster) {
					ErrPrint("Failed to get cluster\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				state = CATEGORY;
				break;

			case CATEGORY:
				if (is_open != 1) {
					ErrPrint("Invalid state\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}
				category = group_find_category(cluster, name);
				if (!category) {
					category = group_create_category(cluster, name);
				}

				if (!category) {
					ErrPrint("Failed to get category\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				info = group_create_context_info(category, pkgname);
				if (!info) {
					ErrPrint("Failed to create ctx info\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				state = CONTEXT_ITEM;
				break;
			case CONTEXT_ITEM:
				if (is_open == 1) {
					category = group_find_category(cluster, name);
					if (!category) {
						category = group_create_category(cluster, name);
					}

					if (!category) {
						ErrPrint("Failed to get category\n");
						DbgFree(name);
						return LB_STATUS_ERROR_FAULT;
					}

					info = group_create_context_info(category, pkgname);
					if (!info) {
						ErrPrint("Failed to create ctx info\n");
						DbgFree(name);
						return LB_STATUS_ERROR_FAULT;
					}
				} else if (is_open == 2) {
					item = group_add_context_item(info, name);
					if (!item) {
						ErrPrint("Failed to create a context item\n");
						DbgFree(name);
						return LB_STATUS_ERROR_FAULT;
					}
					state = CONTEXT_OPTION_KEY;
				} else {
					ErrPrint("Invalid state\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				break;
			case CONTEXT_OPTION_VALUE:
				if (is_open != 3) {
					ErrPrint("Invalid state\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				if (group_add_option(item, key, name) < 0) {
					ErrPrint("Failed to add a new option: %s - %s\n", key, name);
				}

				DbgFree(key);
				key = NULL;

				state = CONTEXT_OPTION_KEY;
				break;
			case CONTEXT_OPTION_KEY:
			default:
				ErrPrint("Invalid state (%s)\n", name);
				DbgFree(name);
				return LB_STATUS_ERROR_FAULT;
			}

			DbgFree(name);
			len = 0;
			ptr++;
			while (*ptr && isspace(*ptr)) ptr++;
			continue;
		} else if (*ptr == '=') {
			if (is_open != 3 || state != CONTEXT_OPTION_KEY) {
				ErrPrint("Invalid state\n");
				return LB_STATUS_ERROR_FAULT;
			}

			key = get_token(ptr, &len);
			if (!key) {
				ErrPrint("Failed to get token\n");
				return LB_STATUS_ERROR_FAULT;
			}

			state = CONTEXT_OPTION_VALUE;
			len = 0;
			ptr++;
			while (*ptr && isspace(*ptr)) ptr++;
			continue;
		} else if (*ptr == '}') {
			if (is_open <= 0) {
				ErrPrint("Invalid state\n");
				return LB_STATUS_ERROR_FAULT;
			}

			name = get_token(ptr, &len);
			if (!name) {
				ErrPrint("Failed to get token, len:%d\n", len);
				is_open--;
				len = 0;
				ptr++;
				while (*ptr && isspace(*ptr)) ptr++;
				continue;
			}

			switch (state) {
			case CATEGORY:
				category = group_find_category(cluster, name);
				if (!category) {
					category = group_create_category(cluster, name);
				}

				if (!category) {
					ErrPrint("Failed to get category\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				info = group_create_context_info(category, pkgname);
				if (!info) {
					ErrPrint("Failed to create ctx info\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				state = CLUSTER;
				break;
			case CONTEXT_ITEM:
				if (is_open == 1) {
					category = group_find_category(cluster, name);
					if (!category) {
						category = group_create_category(cluster, name);
					}

					if (!category) {
						ErrPrint("Failed to get category\n");
						DbgFree(name);
						return LB_STATUS_ERROR_FAULT;
					}

					info = group_create_context_info(category, pkgname);
					if (!info) {
						ErrPrint("Failed to create ctx info\n");
						DbgFree(name);
						return LB_STATUS_ERROR_FAULT;
					}

					state = CLUSTER;
				} else if (is_open == 2) {
					state = CATEGORY;
				} else {
					ErrPrint("Invalid state\n");
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}
				break;
			case CONTEXT_OPTION_VALUE:
				if (is_open != 2) {
					ErrPrint("Invalid state (%s)\n", name);
					DbgFree(name);
					return LB_STATUS_ERROR_FAULT;
				}

				if (group_add_option(item, key, name) < 0) {
					ErrPrint("Failed to add a new option: %s - %s\n", key, name);
				}

				DbgFree(key);
				key = NULL;

				state = CONTEXT_ITEM;
				break;
			case CONTEXT_OPTION_KEY:
			case CLUSTER:
			default:
				ErrPrint("Invalid state (%s)\n", name);
				break;
			}

			DbgFree(name);
			is_open--;
			len = 0;
			ptr++;
			while (*ptr && isspace(*ptr)) ptr++;
			continue;
		}

		len++;
		ptr++;
	}

	/* If some cases, the key is not released, try release it, doesn't need to check NULL */
	DbgFree(key);

	if (state != CLUSTER) {
		return LB_STATUS_ERROR_INVALID;
	}

	return LB_STATUS_SUCCESS;
}

HAPI int group_del_livebox(const char *pkgname)
{
	Eina_List *l;
	Eina_List *n;
	Eina_List *s_l;
	Eina_List *s_n;
	Eina_List *i_l;
	Eina_List *i_n;
	struct cluster *cluster;
	struct category *category;
	struct context_info *info;

	EINA_LIST_FOREACH_SAFE(s_info.cluster_list, l, n, cluster) {
		EINA_LIST_FOREACH_SAFE(cluster->category_list, s_l, s_n, category) {
			EINA_LIST_FOREACH_SAFE(category->info_list, i_l, i_n, info) {
				if (!strcmp(pkgname, info->pkgname)) {
					group_destroy_context_info(info);
				}
			}

			if (!category->info_list) {
				group_destroy_category(category);
			}
		}

		if (!cluster->category_list) {
			group_destroy_cluster(cluster);
		}
	}

	return LB_STATUS_SUCCESS;
}

HAPI int group_init(void)
{
	return LB_STATUS_SUCCESS;
}

HAPI int group_fini(void)
{
	struct cluster *cluster;
	struct category *category;

	EINA_LIST_FREE(s_info.cluster_list, cluster) {

		EINA_LIST_FREE(cluster->category_list, category) {
			destroy_category(category);
		}

		destroy_cluster(cluster);
	}
	return LB_STATUS_SUCCESS;
}

/* End of a file */
