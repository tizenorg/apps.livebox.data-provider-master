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

struct event_data {
	int x;
	int y;
	int device;
	int slot;
	struct {
		int major;
		int minor;
	} touch;
	struct {
		int major;
		int minor;
	} width;
	int distance;	/* Hovering */
#if defined(_USE_ECORE_TIME_GET)
	double tv;
#else
	struct timeval tv;
#endif
};

enum event_state {
	EVENT_STATE_DEACTIVATED,
	EVENT_STATE_ACTIVATE,
	EVENT_STATE_ACTIVATED,
	EVENT_STATE_DEACTIVATE,
	EVENT_STATE_ERROR
};

extern int event_init(void);
extern int event_fini(void);
extern int event_activate(int x, int y, int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data);
extern int event_deactivate(int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data);
extern int event_is_activated(void);

/* End of a file */
