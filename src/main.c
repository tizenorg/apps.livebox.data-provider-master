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
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/signalfd.h>
#include <ctype.h>

#include <systemd/sd-daemon.h>

#include <Ecore.h>
#include <glib.h>
#include <glib-object.h>
#include <aul.h>
#include <vconf.h>

#include <packet.h>
#include <dlog.h>

#if defined(HAVE_LIVEBOX)

#include <dynamicbox_service.h>
#include <dynamicbox_conf.h>

#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "buffer_handler.h"
#include "script_handler.h"
#include "package.h"
#include "group.h"
#include "dead_monitor.h"
#include "io.h"
#include "xmonitor.h"
#include "server.h"
#include "event.h"
#include "file_service.h"
#include "utility_service.h"
#endif

#include "conf.h"
#include "setting.h"
#include "util.h"
#include "debug.h"
#include "critical_log.h"
#include "shortcut_service.h"
#include "notification_service.h"
#include "badge_service.h"

#if defined(FLOG)
#define TMP_LOG_FILE "/tmp/live.log"
FILE *__file_log_fp;
#endif

static inline int app_create(void)
{
	int ret;

	if (access(DYNAMICBOX_CONF_LOG_PATH, R_OK | W_OK) != 0) {
		if (mkdir(DYNAMICBOX_CONF_LOG_PATH, 0755) < 0) {
			ErrPrint("Failed to create %s (%s)\n", DYNAMICBOX_CONF_LOG_PATH, strerror(errno));
		}
	}

	/*!
	 * \note
	 * Dead signal handler has to be initialized before
	 * initate package or client (slave and client).
	 *
	 * Because while creating slaves for packages.
	 * It could be crashed before complete the initation stage.
	 *
	 * Then the dead callback should be invoked to handle it properly.
	 *
	 * To enable the dead signal handler,
	 * dead_init should be done before other components are initiated.
	 */
	ret = setting_init();
	if (ret < 0) {
		DbgPrint("Setting initialized: %d\n", ret);
	}

#if defined(HAVE_LIVEBOX)
	ret = client_init();
	if (ret < 0) {
		DbgPrint("Client initialized: %d\n", ret);
	}

	ret = dead_init();
	if (ret < 0) {
		DbgPrint("Dead callback is registered: %d\n", ret);
	}

	ret = group_init();
	if (ret < 0) {
		DbgPrint("group init: %d\n", ret);
	}

	ret = io_init();
	if (ret < 0) {
		DbgPrint("Init I/O: %d\n", ret);
	}

	ret = package_init();
	if (ret < 0) {
		DbgPrint("pkgmgr initialized: %d\n", ret);
	}

	instance_init();

	ret = xmonitor_init();
	if (ret < 0) {
		DbgPrint("XMonitor init is done: %d\n", ret);
	}

	ret = buffer_handler_init();
	if (ret < 0) {
		DbgPrint("Buffer handler init is done: %d\n", ret);
	}

	/*!
	 * \note
	 * After initiate all other sub-systtems,
	 * Enable the server socket.
	 */
	ret = server_init();
	if (ret < 0) {
		DbgPrint("Server initialized: %d\n", ret);
	}

	event_init();

	script_init();

	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_FILE)) {
		file_service_init();
	}

	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_UTILITY)) {
		utility_service_init();
	}
#endif

	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_SHORTCUT)) {
		shortcut_service_init();
	}

	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_NOTIFICATION)) {
		notification_service_init();
	}

	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_BADGE)) {
		badge_service_init();
	}

	return 0;
}

static inline int app_terminate(void)
{
	int ret;

	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_BADGE)) {
		ret = badge_service_fini();
		if (ret < 0) {
			DbgPrint("badge: %d\n", ret);
		}
	}

	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_NOTIFICATION)) {
		ret = notification_service_fini();
		if (ret < 0) {
			DbgPrint("noti: %d\n", ret);
		}
	}

	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_SHORTCUT)) {
		ret = shortcut_service_fini();
		if (ret < 0) {
			DbgPrint("shortcut: %d\n", ret);
		}
	}

#if defined(HAVE_LIVEBOX)
	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_FILE)) {
		ret = file_service_fini();
		if (ret < 0) {
			DbgPrint("Finalize the file service: %d\n", ret);
		}
	}

	ret = server_fini();
	if (ret < 0) {
		DbgPrint("Finalize server: %d\n", ret);
	}

	ret = dead_fini();
	if (ret < 0) {
		DbgPrint("dead signal handler finalized: %d\n", ret);
	}

	if (util_service_is_enabled(DYNAMICBOX_CONF_SERVICE_UTILITY)) {
		ret = utility_service_fini();
		if (ret < 0) {
			DbgPrint("utility: %d\n", ret);
		}
	}

	ret = event_fini();
	if (ret < 0) {
		DbgPrint("event: %d\n", ret);
	}

	ret = setting_fini();
	if (ret < 0) {
		DbgPrint("Finalize setting : %d\n", ret);
	}

	ret = instance_fini();
	if (ret < 0) {
		DbgPrint("Finalizing instances: %d\n", ret);
	}

	ret = package_fini();
	if (ret < 0) {
		DbgPrint("Finalize package info: %d\n", ret);
	}

	ret = script_fini();
	if (ret < 0) {
		DbgPrint("script: %d\n", ret);
	}

	ret = buffer_handler_fini();
	if (ret < 0) {
		DbgPrint("buffer handler: %d\n", ret);
	}

	xmonitor_fini();

	client_fini();

	ret = io_fini();
	if (ret < 0) {
		DbgPrint("IO finalized: %d\n", ret);
	}

	ret = group_fini();
	if (ret < 0) {
		DbgPrint("Group finalized: %d\n", ret);
	}
#endif

	DbgPrint("Terminated\n");
	return 0;
}

static Eina_Bool signal_cb(void *data, Ecore_Fd_Handler *handler)
{
	struct signalfd_siginfo fdsi;
	ssize_t size;
	int fd;

	fd = ecore_main_fd_handler_fd_get(handler);
	if (fd < 0) {
		ErrPrint("Unable to get FD\n");
		ecore_main_fd_handler_del(handler);
		return ECORE_CALLBACK_CANCEL;
	}

	size = read(fd, &fdsi, sizeof(fdsi));
	if (size != sizeof(fdsi)) {
		ErrPrint("Unable to get siginfo: %s\n", strerror(errno));
		ecore_main_fd_handler_del(handler);
		return ECORE_CALLBACK_CANCEL;
	}

	if (fdsi.ssi_signo == SIGTERM) {
		int cfd;

		CRITICAL_LOG("Terminated(SIGTERM)\n");

		cfd = creat("/tmp/.stop.provider", 0644);
		if (cfd < 0 || close(cfd) < 0) {
			ErrPrint("stop.provider: %s\n", strerror(errno));
		}

		vconf_set_bool(VCONFKEY_MASTER_STARTED, 0);
		//exit(0);
		ecore_main_loop_quit();
	} else if (fdsi.ssi_signo == SIGUSR1) {
		/*!
		 * Turn off auto-reactivation
		 * Terminate all slaves
		 */
#if defined(HAVE_LIVEBOX)
		CRITICAL_LOG("USRS1, Deactivate ALL\n");
		slave_deactivate_all(0, 1, 1);
#endif
	} else if (fdsi.ssi_signo == SIGUSR2) {
		/*!
		 * Turn on auto-reactivation
		 * Launch all slaves again
		 */
#if defined(HAVE_LIVEBOX)
		CRITICAL_LOG("USR2, Activate ALL\n");
		slave_activate_all();
#endif
	} else {
		CRITICAL_LOG("Unknown SIG[%d] received\n", fdsi.ssi_signo);
	}

	return ECORE_CALLBACK_RENEW;
}

int main(int argc, char *argv[])
{
	int ret;
	int restart_count = 0;
	sigset_t mask;
	Ecore_Fd_Handler *signal_handler = NULL;

#if defined(FLOG)
	__file_log_fp = fopen(TMP_LOG_FILE, "w+t");
	if (!__file_log_fp) {
		__file_log_fp = fdopen(1, "w+t");
	}
#endif

	/* appcore_agent_terminate */
	if (ecore_init() <= 0) {
		return -EFAULT;
	}

	if (util_screen_init() <= 0) {
		ecore_shutdown();
		return -EFAULT;
	}

	dynamicbox_conf_init();
	dynamicbox_conf_load();

	/*!
	 * How could we care this return values?
	 * Is there any way to print something on the screen?
	 */
	ret = critical_log_init(util_basename(argv[0]));
	if (ret < 0) {
		ErrPrint("Failed to init the critical log\n");
	}

	/*!
	 * \note
	 * Clear old contents files before start the master provider.
	 */
	(void)util_unlink_files(DYNAMICBOX_CONF_ALWAYS_PATH);
	(void)util_unlink_files(DYNAMICBOX_CONF_READER_PATH);
	(void)util_unlink_files(DYNAMICBOX_CONF_IMAGE_PATH);
	(void)util_unlink_files(DYNAMICBOX_CONF_LOG_PATH);

	if (util_free_space(DYNAMICBOX_CONF_IMAGE_PATH) < DYNAMICBOX_CONF_MINIMUM_SPACE) {
		util_remove_emergency_disk();
		util_prepare_emergency_disk();
	}

	util_setup_log_disk();

	sigemptyset(&mask);

	ret = sigaddset(&mask, SIGTERM);
	if (ret < 0) {
		CRITICAL_LOG("Failed to do sigemptyset: %s\n", strerror(errno));
	}

	ret = sigaddset(&mask, SIGUSR1);
	if (ret < 0) {
		CRITICAL_LOG("Failed to do sigemptyset: %s\n", strerror(errno));
	}

	ret = sigaddset(&mask, SIGUSR2);
	if (ret < 0) {
		CRITICAL_LOG("Failed to do sigemptyset: %s\n", strerror(errno));
	}

	ret = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (ret < 0) {
		CRITICAL_LOG("Failed to mask the SIGTERM: %s\n", strerror(errno));
	}

	ret = signalfd(-1, &mask, 0);
	if (ret < 0) {
		CRITICAL_LOG("Failed to initiate the signalfd: %s\n", strerror(errno));
	} else {
		signal_handler = ecore_main_fd_handler_add(ret, ECORE_FD_READ, signal_cb, NULL, NULL, NULL);
		CRITICAL_LOG("Signal handler initiated: %d\n", ret);
	}

	ecore_app_args_set(argc, (const char **)argv);

#if (GLIB_MAJOR_VERSION <= 2 && GLIB_MINOR_VERSION < 36)
	g_type_init();
#endif

	app_create();
	sd_notify(0, "READY=1");

	vconf_get_int(VCONFKEY_MASTER_RESTART_COUNT, &restart_count);
	restart_count++;
	vconf_set_int(VCONFKEY_MASTER_RESTART_COUNT, restart_count);

	vconf_set_bool(VCONFKEY_MASTER_STARTED, 1);
	ecore_main_loop_begin();
	vconf_set_bool(VCONFKEY_MASTER_STARTED, 0);

	app_terminate();

	util_screen_fini();

	if (signal_handler) {
		ecore_main_fd_handler_del(signal_handler);
	}

	ecore_shutdown();
	critical_log_fini();

#if defined(FLOG)
	if (__file_log_fp) {
		fclose(__file_log_fp);
	}
#endif

	dynamicbox_conf_reset();
	return 0;
}

/* End of a file */
