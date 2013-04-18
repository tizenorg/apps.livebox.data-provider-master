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

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <Eina.h>
#include <dlog.h>
#include <livebox-errno.h>

#include "util.h"
#include "debug.h"
#include "conf.h"

int errno;

struct item {
	char *abi;
	char *pkgname; /*!< Slave package name */
};

static struct {
	Eina_List *list;
} s_abi = {
	.list = NULL,
};

HAPI int abi_add_entry(const char *abi, const char *pkgname)
{
	struct item *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Failed to add a new entry for abi[%s - %s]\n", abi, pkgname);
		return LB_STATUS_ERROR_MEMORY;
	}

	item->abi = strdup(abi);
	if (!item->abi) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item);
		return LB_STATUS_ERROR_MEMORY;
	}

	item->pkgname = strdup(pkgname);
	if (!item->pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(item->abi);
		DbgFree(item);
		return LB_STATUS_ERROR_MEMORY;
	}

	s_abi.list = eina_list_append(s_abi.list, item);
	return LB_STATUS_SUCCESS;
}

HAPI int abi_update_entry(const char *abi, const char *pkgname)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;
	char *_pkgname;

	_pkgname = strdup(pkgname);
	if (!_pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

	EINA_LIST_FOREACH_SAFE(s_abi.list, l, n, item) {
		if (!strcasecmp(item->abi, abi)) {
			DbgFree(item->pkgname);
			item->pkgname = _pkgname;
			return 0;
		}
	}

	DbgFree(_pkgname);
	return LB_STATUS_ERROR_NOT_EXIST;
}

HAPI int abi_del_entry(const char *abi)
{
	Eina_List *l;
	Eina_List *n;
	struct item *item;

	EINA_LIST_FOREACH_SAFE(s_abi.list, l, n, item) {
		if (!strcasecmp(item->abi, abi)) {
			s_abi.list = eina_list_remove(s_abi.list, item);
			DbgFree(item->abi);
			DbgFree(item->pkgname);
			DbgFree(item);
			return LB_STATUS_SUCCESS;
		}
	}

	return LB_STATUS_ERROR_NOT_EXIST;
}

HAPI void abi_del_all(void)
{
	struct item *item;

	EINA_LIST_FREE(s_abi.list, item) {
		DbgFree(item->abi);
		DbgFree(item->pkgname);
		DbgFree(item);
	}
}

HAPI const char *abi_find_slave(const char *abi)
{
	Eina_List *l;
	struct item *item;

	EINA_LIST_FOREACH(s_abi.list, l, item) {
		if (!strcasecmp(item->abi, abi))
			return item->pkgname;
	}

	return NULL;
}

HAPI const char *abi_find_by_pkgname(const char *pkgname)
{
	Eina_List *l;
	struct item *item;

	EINA_LIST_FOREACH(s_abi.list, l, item) {
		if (!strcmp(item->pkgname, pkgname))
			return item->abi;
	}

	return NULL;
}

/* End of a file */
