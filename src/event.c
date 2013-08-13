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

int errno;

static struct info {
	pthread_t tid;
	Eina_List *event_list;
	int handle;
	pthread_mutex_t event_list_lock;
	int evt_pipe[PIPE_MAX];
	int tcb_pipe[PIPE_MAX];
	Ecore_Fd_Handler *event_handler;

	int (*event_cb)(enum event_state state, struct event_data *event, void *data);
	void *cbdata;

	enum event_state event_state;
	struct event_data event_data;

	int x;
	int y;
} s_info = {
	.event_list = NULL,
	.handle = -1,
	.event_handler = NULL,

	.event_cb = NULL,
	.cbdata = NULL,

	.event_state = EVENT_STATE_DEACTIVATE,

	.event_data = {
		.x = 0,
		.y = 0,
		.device = -1,
	},
};

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
		case SYN_REPORT:
			if (s_info.event_data.x < 0 || s_info.event_data.y < 0) {
				/* Waiting full event packet */
				break;
			}

			item = malloc(sizeof(*item));
			if (item) {
				char event_ch;

				memcpy(item, &s_info.event_data, sizeof(*item));

				CRITICAL_SECTION_BEGIN(&s_info.event_list_lock);
				s_info.event_list = eina_list_append(s_info.event_list, item);
				CRITICAL_SECTION_END(&s_info.event_list_lock);

				event_ch = EVENT_CH;
				if (write(s_info.evt_pipe[PIPE_WRITE], &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
					ErrPrint("Unable to send an event: %s\n", strerror(errno));
					return LB_STATUS_ERROR_IO;
				}

				/* Take a breathe */
				pthread_yield();
			}
			break;
		case SYN_CONFIG:
			break;
		case SYN_MT_REPORT:
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
			break;
		case ABS_MT_POSITION_X:
			s_info.event_data.x = event->value - s_info.x;
			break;
		case ABS_MT_POSITION_Y:
			s_info.event_data.y = event->value - s_info.y;
			break;
		case ABS_MT_SLOT:
			break;
		case ABS_MT_TRACKING_ID:
			s_info.event_data.device = event->value;
			break;
		case ABS_MT_TOUCH_MAJOR:
			break;
		case ABS_MT_TOUCH_MINOR:
			break;
		case ABS_MT_WIDTH_MAJOR:
			break;
		case ABS_MT_WIDTH_MINOR:
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
	int ret = 0;
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

static Eina_Bool event_read_cb(void *data, Ecore_Fd_Handler *handler)
{
	int fd;
	struct event_data *item;
	char event_ch;

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
	} else {
		ErrPrint("Unable to get event\n");
	}
	CRITICAL_SECTION_END(&s_info.event_list_lock);

	if (item && s_info.event_cb) {
		switch (s_info.event_state) {
		case EVENT_STATE_DEACTIVATE:
			s_info.event_state = EVENT_STATE_ACTIVATE;
			break;
		case EVENT_STATE_ACTIVATE:
			s_info.event_state = EVENT_STATE_ACTIVATED;
			break;
		case EVENT_STATE_ACTIVATED:
		default:
			break;
		}
		s_info.event_cb(s_info.event_state, item, s_info.cbdata);
	}

	free(item);
	return ECORE_CALLBACK_RENEW;
}

/*!
 * x, y is the starting point.
 */
HAPI int event_activate(int x, int y, int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data)
{
	int status;

	if (s_info.handle >= 0) {
		DbgPrint("Already activated\n");
		return LB_STATUS_SUCCESS;
	}

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

	s_info.event_cb = event_cb;
	s_info.cbdata = data;
	s_info.x = x;
	s_info.y = y;

	DbgPrint("Event handler activated\n");
	return LB_STATUS_SUCCESS;
}

HAPI int event_deactivate(void)
{
	int status;
	struct event_data *event;
	void *ret;
	char event_ch = EVENT_CH;

	if (s_info.handle < 0) {
		ErrPrint("Event handler is not actiavated\n");
		return LB_STATUS_SUCCESS;
	}

	if (write(s_info.tcb_pipe[PIPE_WRITE], &event_ch, sizeof(event_ch)) != sizeof(event_ch)) {
		ErrPrint("Unable to write tcb_pipe: %s\n", strerror(errno));
	}

	status = pthread_join(s_info.tid, &ret);
	if (status != 0) {
		ErrPrint("Failed to join a thread: %s\n", strerror(errno));
	} else {
		DbgPrint("Thread returns: %d\n", (int)ret);
	}

	ecore_main_fd_handler_del(s_info.event_handler);
	s_info.event_handler = NULL;

	if (close(s_info.handle) < 0) {
		ErrPrint("Unable to release the fd: %s\n", strerror(errno));
	}

	s_info.handle = -1;
	DbgPrint("Event handler deactivated\n");

	CLOSE_PIPE(s_info.tcb_pipe);
	CLOSE_PIPE(s_info.evt_pipe);

	EINA_LIST_FREE(s_info.event_list, event) {
		if (s_info.event_cb) {
			if (s_info.event_state == EVENT_STATE_DEACTIVATE) {
				s_info.event_state = EVENT_STATE_ACTIVATE;
			} else if (s_info.event_state == EVENT_STATE_ACTIVATE) {
				s_info.event_state = EVENT_STATE_ACTIVATED;
			}
			s_info.event_cb(s_info.event_state, event, s_info.cbdata);
		}
		free(event);
	}

	if (s_info.event_state != EVENT_STATE_DEACTIVATE) {
		s_info.event_state = EVENT_STATE_DEACTIVATE;

		if (s_info.event_cb) {
			s_info.event_cb(s_info.event_state, &s_info.event_data, s_info.cbdata);
		}
	}

	s_info.event_data.x = -1;
	s_info.event_data.y = -1;
	return LB_STATUS_SUCCESS;
}

HAPI int event_is_activated(void)
{
	return s_info.handle >= 0;
}

/* End of a file */
