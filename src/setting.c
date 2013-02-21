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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <vconf.h>
#include <dlog.h>
#include <heynoti.h>

#include <Eina.h>

#include "client_life.h"
#include "setting.h"
#include "util.h"
#include "debug.h"
#include "slave_life.h"
#include "critical_log.h"
#include "xmonitor.h"
#include "conf.h"

int errno;

static struct {
	int heyfd;
} s_info = {
	.heyfd = -1,
};

static void lcd_state_cb(keynode_t *node, void *user_data)
{
	if (!node)
		return;

	xmonitor_handle_state_changes();
}

HAPI int setting_is_lcd_off(void)
{
	int state;

	if (vconf_get_int(VCONFKEY_PM_STATE, &state) != 0) {
		ErrPrint("Idle lock state is not valid\n");
		state = VCONFKEY_PM_STATE_NORMAL; /* UNLOCK */
	}

	DbgPrint("State: %d, (%d:lcdoff, %d:sleep)\n", state, VCONFKEY_PM_STATE_LCDOFF, VCONFKEY_PM_STATE_SLEEP);
	return state == VCONFKEY_PM_STATE_LCDOFF || state == VCONFKEY_PM_STATE_SLEEP;
}

static void power_off_cb(void *data)
{
	CRITICAL_LOG("Terminated(heynoti)\n");

	if (creat("/tmp/.stop.provider", 0644) < 0)
		ErrPrint("Failed to create .stop.provider [%s]\n", strerror(errno));

	exit(0);
}

HAPI int setting_init(void)
{
	int ret;

	ret = vconf_notify_key_changed(VCONFKEY_PM_STATE, lcd_state_cb, NULL);
	if (ret < 0)
		ErrPrint("Failed to add vconf for lock state\n");

	s_info.heyfd = heynoti_init();
	if (s_info.heyfd < 0) {
		CRITICAL_LOG("Failed to set poweroff heynoti [%d]\n", s_info.heyfd);
		return 0;
	}

	ret = heynoti_subscribe(s_info.heyfd, "power_off_start", power_off_cb, NULL);
	if (ret < 0)
		CRITICAL_LOG("Failed to subscribe heynoti for power off [%d]\n", ret);

	ret = heynoti_attach_handler(s_info.heyfd);
	if (ret < 0)
		CRITICAL_LOG("Failed to attach heynoti handler [%d]\n", ret);

	return ret;
}

HAPI int setting_fini(void)
{
	int ret;
	ret = vconf_ignore_key_changed(VCONFKEY_PM_STATE, lcd_state_cb);
	return ret;
}

/* End of a file */
