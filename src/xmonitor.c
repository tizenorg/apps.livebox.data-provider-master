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
#include <sys/ipc.h>
#include <sys/shm.h>
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <Evas.h>
#include <Ecore_X.h>
#include <Ecore.h>

#include <sqlite3.h>

#include <gio/gio.h>
#include <dlog.h>

#include "conf.h"
#include "xmonitor.h"
#include "debug.h"
#include "client_life.h"
#include "slave_life.h"
#include "main.h"
#include "util.h"
#include "setting.h"

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

	.paused = 0,
};

static inline void touch_paused_file(void)
{
	int fd;
	fd = creat(PAUSED_FILE, 0644);
	if (fd >= 0)
		close(fd);
	else
		ErrPrint("Create .live.paused: %s\n", strerror(errno));
}

static inline void remove_paused_file(void)
{
	if (unlink(PAUSED_FILE) < 0)
		ErrPrint("Unlink .live.paused: %s\n", strerror(errno));
}

static inline int get_pid(Ecore_X_Window win)
{
	int pid;
	Ecore_X_Atom atom;
	unsigned char *in_pid;
	int num;

	atom = ecore_x_atom_get("X_CLIENT_PID");
	if (ecore_x_window_prop_property_get(win, atom, ECORE_X_ATOM_CARDINAL,
				sizeof(int), &in_pid, &num) == EINA_FALSE) {
		if (ecore_x_netwm_pid_get(win, &pid) == EINA_FALSE) {
			ErrPrint("Failed to get PID from a window 0x%X\n", win);
			return -EINVAL;
		}
	} else if (in_pid) {
		pid = *(int *)in_pid;
		DbgFree(in_pid);
	} else {
		ErrPrint("Failed to get PID\n");
		return -EINVAL;
	}

	return pid;
}

static Eina_Bool create_cb(void *data, int type, void *event)
{
	Ecore_X_Event_Window_Create *info = event;
	ecore_x_window_client_sniff(info->win);
	return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool destroy_cb(void *data, int type, void *event)
{
	// Ecore_X_Event_Window_Destroy *info = event;
	return ECORE_CALLBACK_PASS_ON;
}

HAPI void xmonitor_handle_state_changes(void)
{
	int paused;
	Eina_List *l;
	struct event_item *item;

	paused = client_is_all_paused() || setting_is_lcd_off();
	if (s_info.paused == paused)
		return;

	s_info.paused = paused;

	if (s_info.paused) {
		EINA_LIST_FOREACH(s_info.pause_list, l, item) {
			if (item->cb)
				item->cb(item->user_data);
		}

		touch_paused_file();

		sqlite3_release_memory(SQLITE_FLUSH_MAX);
		malloc_trim(0);
	} else {
		remove_paused_file();

		EINA_LIST_FOREACH(s_info.resume_list, l, item) {
			if (item->cb)
				item->cb(item->user_data);
		}
	}
}

HAPI int xmonitor_update_state(int target_pid)
{
	Ecore_X_Window win;
	struct client_node *client;
	int pid;

	win = ecore_x_window_focus_get();

	pid = get_pid(win);
	if (pid <= 0) {
		DbgPrint("Focused window has no PID %X\n", win);
		client = client_find_by_pid(target_pid);
		if (client) {
			DbgPrint("Client window has no focus now\n");
			client_paused(client);
		}
		return -ENOENT;
	}

	client = client_find_by_pid(pid);
	if (!client) {
		DbgPrint("Client %d is not registered yet\n", pid);
		client = client_find_by_pid(target_pid);
		if (client) {
			DbgPrint("Client window has no focus now\n");
			client_paused(client);
		}
		return -EINVAL;
	}

	if (target_pid != pid) {
		DbgPrint("Client is paused\n");
		client_paused(client);
	} else {
		DbgPrint("Client is resumed\n");
		client_resumed(client);
	}

	xmonitor_handle_state_changes();
	return 0;
}

static Eina_Bool client_cb(void *data, int type, void *event)
{
	Ecore_X_Event_Client_Message *info = event;
	struct client_node *client;
	char *name;
	int pid;

	pid = get_pid(info->win);
	if (pid <= 0)
		return ECORE_CALLBACK_PASS_ON;

	client = client_find_by_pid(pid);
	if (!client)
		return ECORE_CALLBACK_PASS_ON;

	name = ecore_x_atom_name_get(info->message_type);
	if (!name)
		return ECORE_CALLBACK_PASS_ON;

	if (!strcmp(name, "_X_ILLUME_DEACTIVATE_WINDOW")) {
		DbgPrint("PAUSE EVENT\n");
		client_paused(client);

		xmonitor_handle_state_changes();
	} else if (!strcmp(name, "_X_ILLUME_ACTIVATE_WINDOW")) {
		DbgPrint("RESUME EVENT\n");
		client_resumed(client);

		xmonitor_handle_state_changes();
	} else {
		/* ignore event */
	}

	DbgFree(name);
	return ECORE_CALLBACK_PASS_ON;
}

static inline void sniff_all_windows(void)
{
	Ecore_X_Window root;
	Ecore_X_Window ret;
	struct stack_item *new_item;
	struct stack_item *item;
	Eina_List *win_stack;
	//int pid;
	struct stack_item {
		Ecore_X_Window *wins;
		int nr_of_wins;
		int i;
	};

	root = ecore_x_window_root_first_get();
	ecore_x_window_sniff(root);

	new_item = malloc(sizeof(*new_item));
	if (!new_item) {
		ErrPrint("Error(%s)\n", strerror(errno));
		return;
	}

	new_item->nr_of_wins = 0;
	new_item->wins =
		ecore_x_window_children_get(root, &new_item->nr_of_wins);
	new_item->i = 0;

	win_stack = NULL;

	if (new_item->wins)
		win_stack = eina_list_append(win_stack, new_item);
	else
		DbgFree(new_item);

	while ((item = eina_list_nth(win_stack, 0))) {
		win_stack = eina_list_remove(win_stack, item);

		if (!item->wins) {
			DbgFree(item);
			continue;
		}

		while (item->i < item->nr_of_wins) {
			ret = item->wins[item->i];

			/*
			 * Now we don't need to care about visibility of window,
			 * just check whether it is registered or not.
			 * (ecore_x_window_visible_get(ret))
			 */
			ecore_x_window_client_sniff(ret);

			new_item = malloc(sizeof(*new_item));
			if (!new_item) {
				ErrPrint("Error %s\n", strerror(errno));
				item->i++;
				continue;
			}

			new_item->i = 0;
			new_item->nr_of_wins = 0;
			new_item->wins =
				ecore_x_window_children_get(ret,
							&new_item->nr_of_wins);
			if (new_item->wins) {
				win_stack =
					eina_list_append(win_stack, new_item);
			} else {
				DbgFree(new_item);
			}

			item->i++;
		}

		DbgFree(item->wins);
		DbgFree(item);
	}

	return;
}

HAPI int xmonitor_init(void)
{
	if (ecore_x_composite_query() == EINA_FALSE)
		DbgPrint("====> COMPOSITOR IS NOT ENABLED\n");

	s_info.create_handler =
		ecore_event_handler_add(ECORE_X_EVENT_WINDOW_CREATE,
							create_cb, NULL);
	if (!s_info.create_handler) {
		ErrPrint("Failed to add create event handler\n");
		return -EFAULT;
	}

	s_info.destroy_handler =
		ecore_event_handler_add(ECORE_X_EVENT_WINDOW_DESTROY,
							destroy_cb, NULL);
	if (!s_info.create_handler) {
		ErrPrint("Failed to add destroy event handler\n");
		ecore_event_handler_del(s_info.create_handler);
		s_info.create_handler = NULL;
		return -EFAULT;
	}

	s_info.client_handler =
		ecore_event_handler_add(ECORE_X_EVENT_CLIENT_MESSAGE,
							client_cb, NULL);
	if (!s_info.client_handler) {
		ErrPrint("Failed to add focus out event handler\n");
		ecore_event_handler_del(s_info.create_handler);
		ecore_event_handler_del(s_info.destroy_handler);
		s_info.create_handler = NULL;
		s_info.destroy_handler = NULL;
		return -EFAULT;
	}

	sniff_all_windows();

	s_info.paused = client_is_all_paused() || setting_is_lcd_off();
	if (s_info.paused) {
		touch_paused_file();
	} else {
		remove_paused_file();
	}
	return 0;
}

HAPI void xmonitor_fini(void)
{
	ecore_event_handler_del(s_info.create_handler);
	ecore_event_handler_del(s_info.destroy_handler);
	ecore_event_handler_del(s_info.client_handler);

	s_info.create_handler = NULL;
	s_info.destroy_handler = NULL;
	s_info.client_handler = NULL;
}

HAPI int xmonitor_add_event_callback(enum xmonitor_event event, int (*cb)(void *user_data), void *user_data)
{
	struct event_item *item;

	item = malloc(sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
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
		return -EINVAL;
	}

	return 0;
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
				return 0;
			}
		}
		break;

	case XMONITOR_RESUMED:
		EINA_LIST_FOREACH_SAFE(s_info.resume_list, l, n, item) {
			if (item->cb == cb && item->user_data == user_data) {
				s_info.resume_list = eina_list_remove(s_info.resume_list, item);
				DbgFree(item);
				return 0;
			}
		}
		break;
	default:
		ErrPrint("Invalid event type\n");
		return -EINVAL;
	}

	return -ENOENT;
}

HAPI int xmonitor_is_paused(void)
{
	return s_info.paused;
}

/* End of a file */
