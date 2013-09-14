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

#include <Ecore.h>
#include <Ecore_X.h>
#include <Evas.h>
#include <Ecore_Evas.h>
#include <glib.h>
#include <glib-object.h>
#include <aul.h>
#include <vconf.h>

#include <packet.h>
#include <dlog.h>

#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "buffer_handler.h"
#include "script_handler.h"
#include "package.h"
#include "group.h"
#include "dead_monitor.h"
#include "conf.h"
#include "io.h"
#include "xmonitor.h"
#include "setting.h"
#include "server.h"
#include "util.h"
#include "debug.h"
#include "critical_log.h"
#include "event.h"
#include "shortcut_service.h"
#include "notification_service.h"
#include "utility_service.h"
#include "badge_service.h"
#include "file_service.h"

#if defined(FLOG)
FILE *__file_log_fp;
#endif

static inline int app_create(void)
{
	int ret;

	if (access(SLAVE_LOG_PATH, R_OK|W_OK) != 0) {
		if (mkdir(SLAVE_LOG_PATH, 755) < 0) {
			ErrPrint("Failed to create %s (%s)\n", SLAVE_LOG_PATH, strerror(errno));
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
	DbgPrint("Setting initialized: %d\n", ret);

	ret = client_init();
	DbgPrint("Client initialized: %d\n", ret);

	ret = dead_init();
	DbgPrint("Dead callback is registered: %d\n", ret);

	ret = group_init();
	DbgPrint("group init: %d\n", ret);

	ret = io_init();
	DbgPrint("Init I/O: %d\n", ret);

	ret = package_init();
	DbgPrint("pkgmgr initialized: %d\n", ret);

	instance_init();

	ret = xmonitor_init();
	DbgPrint("XMonitor init is done: %d\n", ret);

	ret = buffer_handler_init();
	DbgPrint("Buffer handler init is done: %d\n", ret);

	/*!
	 * \note
	 * After initiate all other sub-systtems,
	 * Enable the server socket.
	 */
	ret = server_init();
	DbgPrint("Server initialized: %d\n", ret);

	event_init();

	shortcut_service_init();
	notification_service_init();
	badge_service_init();
	utility_service_init();
	script_init();

	file_service_init();

	return 0;
}

static inline int app_terminate(void)
{
	int ret;

	ret = file_service_fini();
	DbgPrint("Finalize the file service: %d\n", ret);

	ret = server_fini();
	DbgPrint("Finalize server: %d\n", ret);

	ret = dead_fini();
	DbgPrint("dead signal handler finalized: %d\n", ret);

	ret = utility_service_fini();
	DbgPrint("utility: %d\n", ret);

	ret = badge_service_fini();
	DbgPrint("badge: %d\n", ret);

	ret = notification_service_fini();
	DbgPrint("noti: %d\n", ret);

	ret = shortcut_service_fini();
	DbgPrint("shortcut: %d\n", ret);

	ret = event_fini();
	DbgPrint("event: %d\n", ret);

	ret = setting_fini();
	DbgPrint("Finalize setting : %d\n", ret);

	ret = instance_fini();
	DbgPrint("Finalizing instances: %d\n", ret);

	ret = package_fini();
	DbgPrint("Finalize package info: %d\n", ret);

	ret = script_fini();
	DbgPrint("script: %d\n", ret);

	ret = buffer_handler_fini();
	DbgPrint("buffer handler: %d\n", ret);

	xmonitor_fini();

	client_fini();

	ret = io_fini();
	DbgPrint("IO finalized: %d\n", ret);

	ret = group_fini();
	DbgPrint("Group finalized: %d\n", ret);

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
		return ECORE_CALLBACK_CANCEL;
	}

	size = read(fd, &fdsi, sizeof(fdsi));
	if (size != sizeof(fdsi)) {
		ErrPrint("Unable to get siginfo: %s\n", strerror(errno));
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
		CRITICAL_LOG("USRS1, Deactivate ALL\n");
		slave_deactivate_all(0, 1);
	} else if (fdsi.ssi_signo == SIGUSR2) {
		/*!
		 * Turn on auto-reactivation
		 * Launch all slaves again
		 */
		CRITICAL_LOG("USR2, Activate ALL\n");
		slave_activate_all();
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

	conf_init();
	conf_loader();

	/*!
	 * \note
	 * Clear old contents files before start the master provider.
	 */
	(void)util_unlink_files(ALWAYS_PATH);
	(void)util_unlink_files(READER_PATH);
	(void)util_unlink_files(IMAGE_PATH);
	(void)util_unlink_files(SLAVE_LOG_PATH);

	/*!
	 * How could we care this return values?
	 * Is there any way to print something on the screen?
	 */
	ret = critical_log_init(util_basename(argv[0]));
	if (ret < 0) {
		ErrPrint("Failed to init the critical log\n");
	}

#if defined(FLOG)
	__file_log_fp = fopen("/tmp/live.log", "w+t");
	if (!__file_log_fp) {
		__file_log_fp = fdopen(1, "w+t");
	}
#endif
	/* appcore_agent_terminate */
	if (ecore_init() <= 0) {
		CRITICAL_LOG("Failed to initiate ecore\n");
		critical_log_fini();
		return -EFAULT;
	}

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

	if (ecore_x_init(NULL) <= 0) {
		CRITICAL_LOG("Failed to ecore x init\n");
		ecore_shutdown();
		critical_log_fini();
		return -EFAULT;
	}

	ecore_app_args_set(argc, (const char **)argv);

	if (evas_init() <= 0) {
		CRITICAL_LOG("Failed to init evas return count is below than 0\n");
		ecore_x_shutdown();
		ecore_shutdown();
		critical_log_fini();
		return -EFAULT;
	}

	if (ecore_evas_init() <= 0) {
		CRITICAL_LOG("Failed to init ecore_evas\n");
		evas_shutdown();
		ecore_x_shutdown();
		ecore_shutdown();
		critical_log_fini();
		return -EFAULT;
	}

	g_type_init();

	/*!
 	 * \note
	 * conf_update_size requires ecore_x_init.
	 */
	conf_update_size();

	app_create();

	vconf_get_int(VCONFKEY_MASTER_RESTART_COUNT, &restart_count);
	restart_count++;
	vconf_set_int(VCONFKEY_MASTER_RESTART_COUNT, restart_count);

	vconf_set_bool(VCONFKEY_MASTER_STARTED, 1);
	ecore_main_loop_begin();
	vconf_set_bool(VCONFKEY_MASTER_STARTED, 0);

	app_terminate();

	evas_shutdown();
	ecore_evas_shutdown();

	ecore_x_shutdown();

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

	conf_reset();
	return 0;
}

/* End of a file */
