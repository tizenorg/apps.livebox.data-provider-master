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
#include <sys/ipc.h>
#include <sys/shm.h>
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <sqlite3.h>

#include <gio/gio.h>
#include <dlog.h>
#include <dynamicbox_errno.h>

#include "conf.h"
#include "debug.h"
#include "client_life.h"
#include "slave_life.h"
#include "main.h"
#include "util.h"
#include "setting.h"
#include "xmonitor.h"

int errno;

struct event_item {
	int (*cb)(void *user_data);
	void *user_data;
};

static struct info {
	Ecore_Event_Handler *create_handler;
	Ecore_Event_Handler *destroy_handler;
	Ecore_Event_Handler *client_handler;

	Eina_List *pause_list;
	Eina_List *resume_list;

	int paused;
} s_info = {
	.create_handler = NULL,
	.destroy_handler = NULL,
	.client_handler = NULL,

	.pause_list = NULL,
	.resume_list = NULL,

	.paused = 1, /*!< The provider is treated as paused process when it is launched */
};

static inline void touch_paused_file(void)
{
	int fd;
	fd = creat(PAUSED_FILE, 0644);
	if (fd >= 0) {
		if (close(fd) < 0) {
			ErrPrint("close: %s\n", strerror(errno));
		}
	} else {
		ErrPrint("Create .live.paused: %s\n", strerror(errno));
	}
}

static inline void remove_paused_file(void)
{
	if (unlink(PAUSED_FILE) < 0) {
		ErrPrint("Unlink .live.paused: %s\n", strerror(errno));
	}
}

HAPI void xmonitor_handle_state_changes(void)
{
	int paused;
	Eina_List *l;
	struct event_item *item;

	paused = client_is_all_paused() || setting_is_lcd_off();
	if (s_info.paused == paused) {
		return;
	}

	s_info.paused = paused;

	if (s_info.paused) {
		EINA_LIST_FOREACH(s_info.pause_list, l, item) {
			if (item->cb) {
				item->cb(item->user_data);
			}
		}

		touch_paused_file();

		sqlite3_release_memory(SQLITE_FLUSH_MAX);
		malloc_trim(0);
	} else {
		remove_paused_file();

		EINA_LIST_FOREACH(s_info.resume_list, l, item) {
			if (item->cb) {
				item->cb(item->user_data);
			}
		}
	}
}

HAPI int xmonitor_update_state(int target_pid)
{
	struct client_node *client;

	if (!USE_XMONITOR || target_pid < 0) {
		return DBOX_STATUS_ERROR_NONE;
	}

	/*!
	 * \TODO
	 * Find the top(focuesd) window's PID
	 * Compare it with target_pid.
	 * If it is what we finding, call the
	 * xmonitor_pause or xmonitor_resume
	 */

	xmonitor_handle_state_changes();
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int xmonitor_pause(struct client_node *client)
{
	DbgPrint("%d is paused\n", client_pid(client));
	client_paused(client);
	xmonitor_handle_state_changes();
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int xmonitor_resume(struct client_node *client)
{
	DbgPrint("%d is resumed\n", client_pid(client));
	client_resumed(client);
	xmonitor_handle_state_changes();
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int xmonitor_init(void)
{
	if (USE_XMONITOR) {
		return DBOX_STATUS_ERROR_NONE;
	}

	s_info.paused = client_is_all_paused() || setting_is_lcd_off();
	if (s_info.paused) {
		touch_paused_file();
	} else {
		remove_paused_file();
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI void xmonitor_fini(void)
{
	if (USE_XMONITOR) {
	}
}

HAPI int xmonitor_add_event_callback(enum xmonitor_event event, int (*cb)(void *user_data), void *user_data)
{
	struct event_item *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	item->cb = cb;
	item->user_data = user_data;

	switch (event) {
	case XMONITOR_PAUSED:
		s_info.pause_list = eina_list_prepend(s_info.pause_list, item);
		break;
	case XMONITOR_RESUMED:
		s_info.resume_list = eina_list_prepend(s_info.resume_list, item);
		break;
	default:
		ErrPrint("Invalid event type\n");
		DbgFree(item);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int xmonitor_del_event_callback(enum xmonitor_event event, int (*cb)(void *user_data), void *user_data)
{
	struct event_item *item;
	Eina_List *l;
	Eina_List *n;

	switch (event) {
	case XMONITOR_PAUSED:
		EINA_LIST_FOREACH_SAFE(s_info.pause_list, l, n, item) {
			if (item->cb == cb && item->user_data == user_data) {
				s_info.pause_list = eina_list_remove(s_info.pause_list, item);
				DbgFree(item);
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;

	case XMONITOR_RESUMED:
		EINA_LIST_FOREACH_SAFE(s_info.resume_list, l, n, item) {
			if (item->cb == cb && item->user_data == user_data) {
				s_info.resume_list = eina_list_remove(s_info.resume_list, item);
				DbgFree(item);
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;
	default:
		ErrPrint("Invalid event type\n");
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	return DBOX_STATUS_ERROR_NOT_EXIST;
}

HAPI int xmonitor_is_paused(void)
{
	return s_info.paused;
}

/* End of a file */

