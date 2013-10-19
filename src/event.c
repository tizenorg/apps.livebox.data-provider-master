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

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/input.h>

#include <Eina.h>
#include <Ecore.h>
#include <dlog.h>
#include <livebox-errno.h>

#include "util.h"
#include "debug.h"
#include "conf.h"
#include "event.h"

#define EVENT_CH	'e'

#if !defined(ABS_MT_TOOL_X)
#define ABS_MT_TOOL_X           0x3c    /* Center X tool position */
#endif

#if !defined(ABS_MT_TOOL_Y)
#define ABS_MT_TOOL_Y           0x3d    /* Center Y tool position */
#endif

int errno;

static struct info {
	pthread_t tid;
	Eina_List *event_list;
	int handle;
	pthread_mutex_t event_list_lock;
	int evt_pipe[PIPE_MAX];
	int tcb_pipe[PIPE_MAX];
	Ecore_Fd_Handler *event_handler;

	struct event_data event_data;

	Eina_List *event_listener_list;
	Eina_List *reactivate_list;
} s_info = {
	.event_list = NULL,
	.handle = -1,
	.event_handler = NULL,

	.event_data = {
		.x = -1,
		.y = -1,
		.device = -1,
		.slot = -1,
	},

	.event_listener_list = NULL,
	.reactivate_list = NULL,
};

struct event_listener {
	int (*event_cb)(enum event_state state, struct event_data *event, void *data);
	void *cbdata;

	enum event_state state;

#if defined(_USE_ECORE_TIME_GET)
	double tv;
#else
	struct timeval tv; /* Recording Activate / Deactivate time */
#endif
	int x; /* RelX */
	int y; /* RelY */
};

static int activate_thread(void);

HAPI int event_init(void)
{
	int ret;
	ret = pthread_mutex_init(&s_info.event_list_lock, NULL);
	if (ret != 0) {
		ErrPrint("Mutex: %s\n", strerror(ret));
		return LB_STATUS_ERROR_FAULT;
	}
	return LB_STATUS_SUCCESS;
}

HAPI int event_fini(void)
{
	int ret;
	ret = pthread_mutex_destroy(&s_info.event_list_lock);
	if (ret != 0) {
		ErrPrint("Mutex destroy failed: %s\n", strerror(ret));
	}
	return LB_STATUS_SUCCESS;
}

static inline int processing_input_event(struct input_event *event)
{
	struct event_data *item;

	switch (event->type) {
	case EV_SYN:
		switch (event->code) {
			break;
		case SYN_CONFIG:
			break;
		case SYN_MT_REPORT:
		case SYN_REPORT:
			if (s_info.event_data.x < 0 || s_info.event_data.y < 0) {
				/* Waiting full event packet */
				break;
			}

			item = malloc(sizeof(*item));
			if (item) {
				char event_ch = EVENT_CH;

#if defined(_USE_ECORE_TIME_GET)
				s_info.event_data.tv = ecore_time_get();
#else
				if (gettimeofday(&s_info.event_data.tv, NULL) < 0) {
					ErrPrint("gettimeofday: %s\n", strerror(errno));
				}
#endif

				memcpy(item, &s_info.event_data, sizeof(*item));

				CRITICAL_SECTION_BEGIN(&s_info.event_list_lock);
				s_info.event_list = eina_list_append(s_info.event_list, item);
				CRITICAL_SECTION_END(&s_info.event_list_lock);

				if (write(s_info.evt_pipe[PIPE_WRITE], &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
					ErrPrint("Unable to send an event: %s\n", strerror(errno));
					return LB_STATUS_ERROR_IO;
				}

				/* Take a breathe */
				pthread_yield();
			} else {
				ErrPrint("Heap: %s\n", strerror(errno));
			}

			if (s_info.event_data.device < 0) {
				s_info.event_data.x = -1;
				s_info.event_data.y = -1;
				s_info.event_data.slot = -1;
			}
			break;
		/*
		case SYN_DROPPED:
			DbgPrint("EV_SYN, SYN_DROPPED\n");
			break;
		*/
		default:
			DbgPrint("EV_SYN, 0x%x\n", event->code);
			break;
		}
		break;
	case EV_KEY:
		break;
	case EV_REL:
		break;
	case EV_ABS:
		switch (event->code) {
		case ABS_DISTANCE:
			s_info.event_data.distance = event->value;
			break;
		case ABS_MT_TOOL_X:
		case ABS_MT_TOOL_Y:
			break;
		case ABS_MT_POSITION_X:
			s_info.event_data.x = event->value;
			break;
		case ABS_MT_POSITION_Y:
			s_info.event_data.y = event->value;
			break;
		case ABS_MT_SLOT:
			s_info.event_data.slot = event->value;
			break;
		case ABS_MT_TRACKING_ID:
			s_info.event_data.device = event->value;
			break;
		case ABS_MT_TOUCH_MAJOR:
			s_info.event_data.touch.major = event->value;
			break;
		case ABS_MT_TOUCH_MINOR:
			s_info.event_data.touch.minor = event->value;
			break;
		case ABS_MT_WIDTH_MAJOR:
			s_info.event_data.width.major = event->value;
			break;
		case ABS_MT_WIDTH_MINOR:
			s_info.event_data.width.minor = event->value;
			break;
		default:
			DbgPrint("EV_ABS, 0x%x\n", event->code);
			break;
		}
		break;
	case EV_MSC:
		break;
	case EV_SW:
		break;
	case EV_LED:
		break;
	case EV_SND:
		break;
	case EV_REP:
		break;
	case EV_FF:
		break;
	case EV_PWR:
		break;
	case EV_FF_STATUS:
		break;
	default:
		DbgPrint("0x%X, 0x%X\n", event->type, event->code);
		break;
	}

	return LB_STATUS_SUCCESS;
}

static void *event_thread_main(void *data)
{
	fd_set set;
	long ret = 0;
	struct input_event input_event;
	char *ptr = (char *)&input_event;
	int offset = 0;
	int readsize = 0;
	int fd;

	DbgPrint("Initiated\n");

	while (1) {
		FD_ZERO(&set);
		FD_SET(s_info.handle, &set);
		FD_SET(s_info.tcb_pipe[PIPE_READ], &set);

		fd = s_info.handle > s_info.tcb_pipe[PIPE_READ] ? s_info.handle : s_info.tcb_pipe[PIPE_READ];
		ret = select(fd + 1, &set, NULL, NULL, NULL);
		if (ret < 0) {
			ret = -errno;
			if (errno == EINTR) {
				DbgPrint("Select receives INTR\n");
				continue;
			}
			ErrPrint("Error: %s\n", strerror(errno));
			break;
		} else if (ret == 0) {
			ErrPrint("Timeout expired\n");
			ret = LB_STATUS_ERROR_TIMEOUT;
			break;
		}

		if (FD_ISSET(s_info.handle, &set)) {
			readsize = read(s_info.handle, ptr + offset, sizeof(input_event) - offset);
			if (readsize < 0) {
				ErrPrint("Unable to read device: %s / fd: %d / offset: %d / size: %d - %d\n", strerror(errno), s_info.handle, offset, sizeof(input_event), readsize);
				ret = LB_STATUS_ERROR_FAULT;
				break;
			}

			offset += readsize;
			if (offset == sizeof(input_event)) {
				offset = 0;
				if (processing_input_event(&input_event) < 0) {
					ret = LB_STATUS_ERROR_FAULT;
					break;
				}
			}
		}

		if (FD_ISSET(s_info.tcb_pipe[PIPE_READ], &set)) {
			char event_ch;

			if (read(s_info.tcb_pipe[PIPE_READ], &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
				ErrPrint("Unable to read TCB_PIPE: %s\n", strerror(errno));
			}

			ret = LB_STATUS_ERROR_CANCEL;
			break;
		}
	}

	return (void *)ret;
}

static inline void clear_all_listener_list(void)
{
	struct event_listener *listener;
	enum event_state next_state;
	Eina_List *l;
	Eina_List *n;

	s_info.event_handler = NULL;
	CLOSE_PIPE(s_info.evt_pipe);

	while (s_info.event_listener_list) {
		EINA_LIST_FOREACH_SAFE(s_info.event_listener_list, l, n, listener) {
			switch (listener->state) {
			case EVENT_STATE_ACTIVATE:
				next_state = EVENT_STATE_ACTIVATED;
				break;
			case EVENT_STATE_ACTIVATED:
				next_state = EVENT_STATE_DEACTIVATE;
				break;
			case EVENT_STATE_DEACTIVATE:
				next_state = EVENT_STATE_DEACTIVATED;
				break;
			case EVENT_STATE_DEACTIVATED:
			default:
				s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
				DbgFree(listener);
				continue;
			}

			if (listener->event_cb(listener->state, &s_info.event_data, listener->cbdata) < 0) {
				if (eina_list_data_find(s_info.event_listener_list, listener)) {
					s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
					DbgFree(listener);
					continue;
				}
			}

			listener->state = next_state;
		}
	}
}

static Eina_Bool event_read_cb(void *data, Ecore_Fd_Handler *handler)
{
	int fd;
	struct event_data *item;
	char event_ch;
	struct event_listener *listener;
	Eina_List *l;
	Eina_List *n;
	enum event_state next_state;
	enum event_state cur_state;
	struct event_data modified_item;

	fd = ecore_main_fd_handler_fd_get(handler);
	if (fd < 0) {
		ErrPrint("Invalid fd\n");
		return ECORE_CALLBACK_CANCEL;
	}

	if (read(fd, &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
		ErrPrint("Unable to read event ch: %s\n", strerror(errno));
		return ECORE_CALLBACK_CANCEL;
	}

	CRITICAL_SECTION_BEGIN(&s_info.event_list_lock);
	item = eina_list_nth(s_info.event_list, 0);
	if (item) {
		s_info.event_list = eina_list_remove(s_info.event_list, item);
	}
	CRITICAL_SECTION_END(&s_info.event_list_lock);

	if (item) {
		EINA_LIST_FOREACH_SAFE(s_info.event_listener_list, l, n, listener) {
			switch (listener->state) {
			case EVENT_STATE_ACTIVATE:
#if defined(_USE_ECORE_TIME_GET)
				if (listener->tv > item->tv) {
					continue;
				}
#else
				if (timercmp(&listener->tv, &item->tv, >)) {
					/* Ignore previous events before activating this listener */
					continue;
				}
#endif

				next_state = EVENT_STATE_ACTIVATED;
				cur_state = listener->state;
				break;
			case EVENT_STATE_DEACTIVATE:
#if defined(_USE_ECORE_TIME_GET)
				if (listener->tv > item->tv) {
					/* Consuming all events occurred while activating this listener */
					cur_state = EVENT_STATE_ACTIVATED;
					next_state = EVENT_STATE_ACTIVATED;
					break;
				}
#else
				if (timercmp(&listener->tv, &item->tv, >)) {
					/* Consuming all events occurred while activating this listener */
					cur_state = EVENT_STATE_ACTIVATED;
					next_state = EVENT_STATE_ACTIVATED;
					break;
				}
#endif

				cur_state = listener->state;
				next_state = EVENT_STATE_DEACTIVATED;
				break;
			case EVENT_STATE_ACTIVATED:
				cur_state = listener->state;
				next_state = listener->state;
				break;
			case EVENT_STATE_DEACTIVATED:
			default:
				/* Remove this from the list */
					/* Check the item again. the listener can be deleted from the callback */
				if (eina_list_data_find(s_info.event_listener_list, listener)) {
					s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
					DbgFree(listener);
				}

				continue;
			}

			memcpy(&modified_item, item, sizeof(modified_item));
			modified_item.x -= listener->x;
			modified_item.y -= listener->y;

			if (listener->event_cb(cur_state, &modified_item, listener->cbdata) < 0) {
				if (eina_list_data_find(s_info.event_listener_list, listener)) {
					s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
					DbgFree(listener);
					continue;
				}
			}

			listener->state = next_state;
		}

		DbgFree(item);
	}

	if (s_info.handle < 0 && !s_info.event_list) {
		/* This callback must has to clear all listeners in this case */
		clear_all_listener_list();

		EINA_LIST_FREE(s_info.reactivate_list, listener) {
			s_info.event_listener_list = eina_list_append(s_info.event_listener_list, listener);
		}

		if (s_info.event_listener_list) {
			if (activate_thread() < 0) {
				EINA_LIST_FREE(s_info.event_listener_list, listener) {
					(void)listener->event_cb(EVENT_STATE_ERROR, NULL, listener->cbdata);
				}
			}
		}

		return ECORE_CALLBACK_CANCEL;
	}

	return ECORE_CALLBACK_RENEW;
}

static int activate_thread(void)
{
	int status;

	s_info.handle = open(INPUT_PATH, O_RDONLY);
	if (s_info.handle < 0) {
		ErrPrint("Unable to access the device: %s\n", strerror(errno));
		return LB_STATUS_ERROR_IO;
	}

	if (fcntl(s_info.handle, F_SETFD, FD_CLOEXEC) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
	}

	if (fcntl(s_info.handle, F_SETFL, O_NONBLOCK) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
	}

	status = pipe2(s_info.evt_pipe, O_NONBLOCK | O_CLOEXEC);
	if (status < 0) {
		ErrPrint("Unable to prepare evt pipe: %s\n", strerror(errno));
		if (close(s_info.handle) < 0) {
			ErrPrint("Failed to close handle: %s\n", strerror(errno));
		}
		s_info.handle = -1;
		return LB_STATUS_ERROR_FAULT;
	}

	status = pipe2(s_info.tcb_pipe, O_NONBLOCK | O_CLOEXEC);
	if (status < 0) {
		ErrPrint("Unable to prepare tcb pipe: %s\n", strerror(errno));
		if (close(s_info.handle) < 0) {
			ErrPrint("Failed to close handle: %s\n", strerror(errno));
		}
		s_info.handle = -1;
		CLOSE_PIPE(s_info.evt_pipe);
		return LB_STATUS_ERROR_FAULT;
	}

	s_info.event_handler = ecore_main_fd_handler_add(s_info.evt_pipe[PIPE_READ], ECORE_FD_READ, event_read_cb, NULL, NULL, NULL);
	if (!s_info.event_handler) {
		if (close(s_info.handle) < 0) {
			ErrPrint("Failed to close handle: %s\n", strerror(errno));
		}
		s_info.handle = -1;

		CLOSE_PIPE(s_info.tcb_pipe);
		CLOSE_PIPE(s_info.evt_pipe);
		return LB_STATUS_ERROR_FAULT;
	}

	status = pthread_create(&s_info.tid, NULL, event_thread_main, NULL);
	if (status != 0) {
		ErrPrint("Failed to initiate the thread: %s\n", strerror(status));
		ecore_main_fd_handler_del(s_info.event_handler);
		s_info.event_handler = NULL;

		if (close(s_info.handle) < 0) {
			ErrPrint("close: %s\n", strerror(errno));
		}
		s_info.handle = -1;

		CLOSE_PIPE(s_info.tcb_pipe);
		CLOSE_PIPE(s_info.evt_pipe);
		return LB_STATUS_ERROR_FAULT;
	}

	DbgPrint("Event handler activated\n");
	return LB_STATUS_SUCCESS;
}

/*!
 * x, y is the starting point.
 */
HAPI int event_activate(int x, int y, int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data)
{
	struct event_listener *listener;
	int ret = LB_STATUS_SUCCESS;

	listener = malloc(sizeof(*listener));
	if (!listener) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return LB_STATUS_ERROR_MEMORY;
	}

#if defined(_USE_ECORE_TIME_GET)
	listener->tv = ecore_time_get();
#else
	if (gettimeofday(&listener->tv, NULL) < 0) {
		ErrPrint("gettimeofday: %s\n", strerror(errno));
		DbgFree(listener);
		return LB_STATUS_ERROR_FAULT;
	}
#endif

	listener->event_cb = event_cb;
	listener->cbdata = data;
	listener->state = EVENT_STATE_ACTIVATE;
	listener->x = x;
	listener->y = y;

	if (s_info.handle < 0) {
		/*!
		 * \note
		 * We don't need to lock to access event_list here.
		 * If the _sinfo.handle is greater than 0, the event_list will not be touched.
		 * But if the s_info.handle is less than 0, it means, there is not thread,
		 * so we can access the event_list without lock.
		 */
		if (s_info.event_list) {
			DbgPrint("Event thread is deactivating now. activating will be delayed\n");
			s_info.reactivate_list = eina_list_append(s_info.reactivate_list, listener);
		} else {
			s_info.event_listener_list = eina_list_append(s_info.event_listener_list, listener);

			if ((ret = activate_thread()) < 0) {
				s_info.event_listener_list = eina_list_remove(s_info.event_listener_list, listener);
				DbgFree(listener);
			}
		}
	} else {
		s_info.event_listener_list = eina_list_append(s_info.event_listener_list, listener);
	}

	return ret;
}

HAPI int event_deactivate(int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data)
{
	int status;
	void *ret;
	char event_ch = EVENT_CH;
	struct event_listener *listener = NULL;
	Eina_List *l;
	int keep_thread = 0;

	EINA_LIST_FOREACH(s_info.event_listener_list, l, listener) {
		if (listener->event_cb == event_cb && listener->cbdata == data) {
			listener->state = EVENT_STATE_DEACTIVATE;
		}

		keep_thread += (listener->state == EVENT_STATE_ACTIVATE || listener->state == EVENT_STATE_ACTIVATED);
	}

	if (!listener) {
		ErrPrint("Listener is not registered\n");
		return LB_STATUS_ERROR_NOT_EXIST;
	}

	if (s_info.handle < 0) {
		ErrPrint("Event handler is not actiavated\n");
		DbgFree(listener);
		return LB_STATUS_SUCCESS;
	}

	if (keep_thread) {
		return LB_STATUS_SUCCESS;
	}

	/* Terminating thread */
	if (write(s_info.tcb_pipe[PIPE_WRITE], &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
		ErrPrint("Unable to write tcb_pipe: %s\n", strerror(errno));
	}

	status = pthread_join(s_info.tid, &ret);
	if (status != 0) {
		ErrPrint("Failed to join a thread: %s\n", strerror(errno));
	} else {
		DbgPrint("Thread returns: %p\n", ret);
	}

	if (close(s_info.handle) < 0) {
		ErrPrint("Unable to release the fd: %s\n", strerror(errno));
	}

	s_info.handle = -1;
	DbgPrint("Event handler deactivated\n");

	CLOSE_PIPE(s_info.tcb_pipe);

	if (!eina_list_count(s_info.event_list)) {
		ecore_main_fd_handler_del(s_info.event_handler);
		clear_all_listener_list();
	}

	s_info.event_data.x = -1;
	s_info.event_data.y = -1;
	s_info.event_data.slot = -1;
	return LB_STATUS_SUCCESS;
}

HAPI int event_is_activated(void)
{
	return s_info.handle >= 0;
}

/* End of a file */
