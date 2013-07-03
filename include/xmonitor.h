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

enum xmonitor_event {
	XMONITOR_PAUSED,
	XMONITOR_RESUMED,

	XMONITOR_ERROR = 0xFFFFFFFF /* To specify the size of this enum */
};

extern int xmonitor_init(void);
extern void xmonitor_fini(void);
extern int xmonitor_update_state(int pid);
extern int xmonitor_add_event_callback(enum xmonitor_event event, int (*cb)(void *user_data), void *user_data);
extern int xmonitor_del_event_callback(enum xmonitor_event event, int (*cb)(void *user_data), void *user_data);
extern int xmonitor_is_paused(void);
extern void xmonitor_handle_state_changes(void);
extern int xmonitor_resume(struct client_node *client);
extern int xmonitor_pause(struct client_node *client);

/* End of a file */
