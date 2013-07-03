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

enum pkgmgr_event_type {
	PKGMGR_EVENT_DOWNLOAD,
	PKGMGR_EVENT_INSTALL,
	PKGMGR_EVENT_UPDATE,
	PKGMGR_EVENT_UNINSTALL,
	PKGMGR_EVENT_RECOVER
};

enum pkgmgr_status {
	PKGMGR_STATUS_START,
	PKGMGR_STATUS_PROCESSING,
	PKGMGR_STATUS_COMMAND,
	PKGMGR_STATUS_END,
	PKGMGR_STATUS_ERROR
};

extern int pkgmgr_init(void);
extern int pkgmgr_fini(void);

extern int pkgmgr_add_event_callback(enum pkgmgr_event_type type, int (*cb)(const char *pkgname, enum pkgmgr_status status, double value, void *data), void *data);

extern void *pkgmgr_del_event_callback(enum pkgmgr_event_type type, int (*cb)(const char *pkgname, enum pkgmgr_status status, double value, void *data), void *data);

/* End of a file */
