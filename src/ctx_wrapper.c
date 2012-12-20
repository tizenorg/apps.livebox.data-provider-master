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
#include <stdlib.h> /* malloc */
#include <unistd.h>
#include <errno.h>
#include <string.h> /* strerror */

#include <dlog.h>

#include <Eina.h>

#include <context_manager.h>

#include "util.h"
#include "debug.h"
#include "group.h"
#include "ctx_wrapper.h"
#include "conf.h"

static struct {
	int enabled;
	Eina_List *cbdata_list;
} s_info = {
	.enabled = 0,
	.cbdata_list = NULL,
};

struct cbfunc {
	int (*cb)(struct context_item *item, void *user_data);
	void *user_data;

	struct cbdata *cbdata;
};

struct cbdata {
	struct context_item *item;
	int req_id;
	context_option_s option;

	Eina_List *cbfunc_list;
};

static void update_context_cb(context_error_e error, int req_id, context_data_s *data, int data_size, void *user_data)
{
	Eina_List *l;
	struct cbfunc *cbfunc;
	struct cbdata *cbdata = (struct cbdata *)user_data;

	if (error != CONTEXT_ERROR_NONE) {
		ErrPrint("REQ_ID[%d] Context update event has an error: %d\n", req_id, error);
		return;
	}

	DbgPrint("req_id: %d\n", req_id);
	DbgPrint("data_size: %d\n", data_size);

	/*!
	 * \note
	 * Only for the safety. -_-
	 */
	if (cbdata->req_id != req_id) {
		ErrPrint("Request ID is not matched\n");
		return;
	}

	EINA_LIST_FOREACH(cbdata->cbfunc_list, l, cbfunc) {
		if (cbfunc->cb(cbdata->item, cbfunc->user_data) != 0) {
			ErrPrint("Callback is canceled\n");
			break;
		}
	}
}

static inline struct cbdata *find_registered_callback(const char *ctx_item, Eina_List *option_list)
{
	Eina_List *l;
	struct cbdata *cbdata;
	register int i;
	unsigned int mask;
	Eina_List *il;
	struct context_option *option;
	const char *key;
	const char *value;

	EINA_LIST_FOREACH(s_info.cbdata_list, l, cbdata) {
		mask = 0x0;

		EINA_LIST_FOREACH(option_list, il, option) {
			key = group_option_item_key(option);
			value = group_option_item_value(option);

			for (i = 0; i < cbdata->option.array_size; i++) {
				if (strcmp(cbdata->option.array[i].key, key))
					continue;

				if (strcmp(cbdata->option.array[i].value, value))
					continue;

				mask |= (0x01 << i);
			}
		}

		if (mask == ((0x01 << cbdata->option.array_size) - 1))
			return cbdata;
	}

	return NULL;
}

HAPI void ctx_wrapper_enable(void)
{
	int ret;

	if (s_info.enabled)
		return;

	ret = context_manager_connect();
	if (ret == CONTEXT_ERROR_NONE)
		s_info.enabled = 1;

	DbgPrint("Context engine is%senabled\n", s_info.enabled ? " " : " not ");
}

HAPI void ctx_wrapper_disable(void)
{
	int ret;

	if (!s_info.enabled)
		return;

	ret = context_manager_disconnect();
	if (ret == CONTEXT_ERROR_NONE)
		s_info.enabled = 0;

	DbgPrint("Context engine is%sdisabled\n", s_info.enabled ? " not " : " ");
}

static inline void register_ctx_callback(struct context_item *item, const char *ctx_item, struct cbfunc *cbfunc)
{
	Eina_List *l;
	Eina_List *option_list;
	struct context_option *option;
	struct cbdata *cbdata;
	const char *key;
	const char *value;
	int idx;
	int ret;

	option_list = group_context_option_list(item);
	if (!option_list) {
		ErrPrint("Has no option list\n");
		return;
	}

	cbdata = find_registered_callback(ctx_item, option_list);
	if (cbdata) {
		DbgPrint("Already registered\n");
		return;
	}

	cbdata = malloc(sizeof(*cbdata));
	if (!cbdata) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return;
	}

	cbdata->item = item;
	cbdata->option.array_size = eina_list_count(option_list);
	if (!cbdata->option.array_size) {
		ErrPrint("Option is not exists. ignore this context event\n");
		DbgFree(cbdata);
		return;
	}

	cbdata->option.array = calloc(cbdata->option.array_size, sizeof(*cbdata->option.array));
	if (!cbdata->option.array) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(cbdata);
		return;
	}

	idx = 0;
	EINA_LIST_FOREACH(option_list, l, option) {
		key = group_option_item_key(option);
		value = group_option_item_value(option);

		if (!key || !value) {
			ErrPrint("Key[%p], value[%p]\n", key, value);
			continue;
		}

		cbdata->option.array[idx].key = (char *)key;
		cbdata->option.array[idx].value = (char *)value;
		idx++;
	}

	/*!
	 * WHY DO WE NEED TO KEEP THE req_id?
	 * Every callback function has their own callback_data.
	 * then...... we don't need to use req_id -_-;;
	 */
	ret = context_manager_add_context_updates_cb(ctx_item, &cbdata->option, update_context_cb, cbdata, &cbdata->req_id);
	if (ret != CONTEXT_ERROR_NONE) {
		DbgFree(cbdata->option.array);
		DbgFree(cbdata);
		return;
	}

	s_info.cbdata_list = eina_list_append(s_info.cbdata_list, cbdata);

	cbdata->cbfunc_list = eina_list_prepend(cbdata->cbfunc_list, cbfunc);
	cbfunc->cbdata = cbdata;
}

HAPI void *ctx_wrapper_register_callback(struct context_item *item, int (*cb)(struct context_item *item, void *user_data), void *user_data)
{
	const char *ctx_item;
	struct cbfunc *cbfunc;

	if (!item) {
		ErrPrint("Item is not valid\n");
		return NULL;
	}

	if (s_info.enabled) {
		ErrPrint("CTX is not connected\n");
		return NULL;
	}

	cbfunc = malloc(sizeof(*cbfunc));
	if (!cbfunc) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	cbfunc->cb = cb;
	cbfunc->user_data = user_data;

	ctx_item = group_context_item(item);
	if (ctx_item)
		register_ctx_callback(item, ctx_item, cbfunc);
	else
		cbfunc->cbdata = NULL;

	return cbfunc;
}

HAPI void *ctx_wrapper_unregister_callback(void *_cbfunc)
{
	struct cbdata *cbdata;
	struct cbfunc *cbfunc = (struct cbfunc *)_cbfunc;
	void *data;

	if (!s_info.enabled) {
		ErrPrint("CTX is not connected\n");
		return NULL;
	}

	cbdata = cbfunc->cbdata;
	if (cbdata) {
		cbdata->cbfunc_list = eina_list_remove(cbdata->cbfunc_list, cbfunc);
		if (!eina_list_count(cbdata->cbfunc_list)) {
			/*!
			 * \TODO
			 * Remove CALLBACK
			 *
			 * context_manager_remove_context_updates_for_item_cb
			 * context_manager_remove_context_updates_cb
			 */

			s_info.cbdata_list = eina_list_remove(s_info.cbdata_list, cbdata);
			DbgFree(cbdata->option.array);
			DbgFree(cbdata);
			DbgPrint("Callback removed\n");
		}
	}

	data = cbfunc->user_data;
	DbgFree(cbfunc);
	return data;
}

/* End of a file */
