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
#include <string.h>
#include <stdlib.h> /* free */

#include <gio/gio.h>

#include <Eina.h>
#include <packet.h>
#include <dlog.h>
#include <dynamicbox_errno.h>
#include <dynamicbox_service.h>
#include <dynamicbox_conf.h>
#include <dynamicbox_cmd_list.h>

#include "util.h"
#include "debug.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "instance.h"
#include "client_rpc.h"
#include "package.h"
#include "conf.h"
#include "critical_log.h"

static struct info {
	Eina_List *call_list;
	int fault_mark_count;
} s_info = {
	.call_list = NULL,
	.fault_mark_count = 0,
};

struct fault_info {
	struct slave_node *slave;
	double timestamp;
	char *pkgname;
	char *filename;
	char *func;
};

HAPI int const fault_is_occured(void)
{
	return s_info.fault_mark_count;
}

static void clear_log_file(struct slave_node *slave)
{
	char filename[BUFSIZ];
	int ret;

	ret = snprintf(filename, sizeof(filename) - 1, "%s/slave.%d", DYNAMICBOX_CONF_LOG_PATH, slave_pid(slave));
	if (ret == sizeof(filename) - 1) {
		filename[sizeof(filename) - 1] = '\0';
		ErrPrint("filename buffer is overflowed\n");
	}

	if (unlink(filename) < 0) {
		ErrPrint("unlink: %s\n", strerror(errno));
	}
}

static char *check_log_file(struct slave_node *slave)
{
	char libexec[BUFSIZ];
	char *ptr;
	FILE *fp;
	char filename[BUFSIZ];

	snprintf(filename, sizeof(filename), "%s/slave.%d", DYNAMICBOX_CONF_LOG_PATH, slave_pid(slave));
	fp = fopen(filename, "rt");
	if (!fp) {
		ErrPrint("No log file found [%s]\n", strerror(errno));
		return NULL;
	}

	ptr = fgets(libexec, sizeof(libexec), fp);
	if (fclose(fp) != 0) {
		ErrPrint("fclose: %s\n", strerror(errno));
	}

	if (ptr != libexec) {
		ErrPrint("Invalid log\n");
		return NULL;
	}

	if (unlink(filename) < 0) {
		ErrPrint("Failed to unlink %s\n", filename);
	}

	ptr = dynamicbox_service_dbox_id_by_libexec(libexec);
	if (!ptr) {
		ErrPrint("Failed to find the faulted package\n");
	}

	DbgPrint("Faulted package: %s\n", ptr);
	return ptr;
}

HAPI void fault_unicast_info(struct client_node *client, const char *pkgname, const char *filename, const char *func)
{
	struct packet *packet;
	unsigned int cmd = CMD_FAULT_PACKAGE;

	if (!client || !pkgname || !filename || !func) {
		return;
	}

	packet = packet_create_noack((const char *)&cmd, "sss", pkgname, filename, func);
	if (!packet) {
		return;
	}

	client_rpc_async_request(client, packet);
}

HAPI void fault_broadcast_info(const char *pkgname, const char *filename, const char *func)
{
	struct packet *packet;
	unsigned int cmd = CMD_FAULT_PACKAGE;

	packet = packet_create_noack((const char *)&cmd, "sss", pkgname, filename, func);
	if (!packet) {
		ErrPrint("Failed to create a param\n");
		return;
	}

	client_broadcast(NULL, packet);
}

static inline void dump_fault_info(const char *name, pid_t pid, const char *pkgname, const char *filename, const char *funcname)
{
	CRITICAL_LOG("Slavename: %s[%d]\n"	\
			"Package: %s\n"		\
			"Filename: %s\n"	\
			"Funcname: %s\n", name, pid, pkgname, filename, funcname);
}

HAPI int fault_info_set(struct slave_node *slave, const char *pkgname, const char *id, const char *func)
{
	struct pkg_info *pkg;
	int ret;

	pkg = package_find(pkgname);
	if (!pkg) {
		return DBOX_STATUS_ERROR_NOT_EXIST;
	}

	ret = package_set_fault_info(pkg, util_timestamp(), id, func);
	if (ret < 0) {
		return DBOX_STATUS_ERROR_FAULT;
	}

	dump_fault_info(slave_name(slave), slave_pid(slave), pkgname, id, func);
	ErrPrint("Set fault %s(%d)\n", !ret ? "Success" : "Failed", ret);
	fault_broadcast_info(pkgname, id, func);

	/*!
	 * \note
	 * Update statistics
	 */
	s_info.fault_mark_count++;
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int fault_check_pkgs(struct slave_node *slave)
{
	struct fault_info *info;
	struct pkg_info *pkg;
	const char *pkgname;
	Eina_List *l;
	Eina_List *n;
	int checked;

	/*!
	 * \note
	 * First step.
	 * Check the log file
	 */
	pkgname = (const char *)check_log_file(slave);
	if (pkgname) {
		pkg = package_find(pkgname);
		if (pkg) {
			(void)package_set_fault_info(pkg, util_timestamp(), NULL, NULL);
			dump_fault_info(slave_name(slave), slave_pid(slave), pkgname, "", "");
			fault_broadcast_info(pkgname, "", "");
			DbgFree((char *)pkgname);

			s_info.fault_mark_count = 0;
			clear_log_file(slave);
			EINA_LIST_REVERSE_FOREACH_SAFE(s_info.call_list, l, n, info) {
				if (info->slave != slave) {
					continue;
				}

				s_info.call_list = eina_list_remove_list(s_info.call_list, l);

				DbgFree(info->pkgname);
				DbgFree(info->filename);
				DbgFree(info->func);
				DbgFree(info);
			}
			return 0;
		}
		DbgFree((char *)pkgname);
	}

	/*!
	 * \note
	 * Second step.
	 * Is it secured slave?
	 */
	pkgname = package_find_by_secured_slave(slave);
	if (pkgname) {
		pkg = package_find(pkgname);
		if (pkg) {
			(void)package_set_fault_info(pkg, util_timestamp(), NULL, NULL);
			dump_fault_info(slave_name(slave), slave_pid(slave), pkgname, "", "");
			fault_broadcast_info(pkgname, "", "");

			s_info.fault_mark_count = 0;
			clear_log_file(slave);
			EINA_LIST_REVERSE_FOREACH_SAFE(s_info.call_list, l, n, info) {
				if (info->slave != slave) {
					continue;
				}

				s_info.call_list = eina_list_remove_list(s_info.call_list, l);

				DbgFree(info->pkgname);
				DbgFree(info->filename);
				DbgFree(info->func);
				DbgFree(info);
			}
			return 0;
		}
	}

	/*!
	 * \note
	 * At last, check the pair of function call and return mark
	 */
	checked = 0;
	EINA_LIST_REVERSE_FOREACH_SAFE(s_info.call_list, l, n, info) {
		if (info->slave == slave) {
			const char *filename;
			const char *func;

			pkg = package_find(info->pkgname);
			if (!pkg) {
				ErrPrint("Failed to find a package %s\n", info->pkgname);
				continue;
			}

			filename = info->filename ? info->filename : "";
			func = info->func ? info->func : "";

			if (!checked) {
				(void)package_set_fault_info(pkg, info->timestamp, info->filename, info->func);
				fault_broadcast_info(info->pkgname, info->filename, info->func);
			} else {
				DbgPrint("Treated as a false log\n");
				dump_fault_info(
						slave_name(info->slave), slave_pid(info->slave), info->pkgname, filename, func);
			}

			s_info.call_list = eina_list_remove_list(s_info.call_list, l);

			DbgFree(info->pkgname);
			DbgFree(info->filename);
			DbgFree(info->func);
			DbgFree(info);
			checked = 1;
		}
	}

	s_info.fault_mark_count = 0;
	clear_log_file(slave);
	return 0;
}

HAPI int fault_func_call(struct slave_node *slave, const char *pkgname, const char *filename, const char *func)
{
	struct fault_info *info;

	info = malloc(sizeof(*info));
	if (!info) {
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	info->slave = slave;

	info->pkgname = strdup(pkgname);
	if (!info->pkgname) {
		DbgFree(info);
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	info->filename = strdup(filename);
	if (!info->filename) {
		DbgFree(info->pkgname);
		DbgFree(info);
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	info->func = strdup(func);
	if (!info->func) {
		DbgFree(info->filename);
		DbgFree(info->pkgname);
		DbgFree(info);
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	info->timestamp = util_timestamp();

	s_info.call_list = eina_list_append(s_info.call_list, info);

	s_info.fault_mark_count++;
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int fault_func_ret(struct slave_node *slave, const char *pkgname, const char *filename, const char *func)
{
	struct fault_info *info;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.call_list, l, info) {
		if (info->slave != slave) {
			continue;
		}

		if (strcmp(info->pkgname, pkgname)) {
			continue;
		}

		if (strcmp(info->filename, filename)) {
			continue;
		}

		if (strcmp(info->func, func)) {
			continue;
		}

		s_info.call_list = eina_list_remove_list(s_info.call_list, l);
		DbgFree(info->filename);
		DbgFree(info->pkgname);
		DbgFree(info->func);
		DbgFree(info);

		s_info.fault_mark_count--;
		return 0;
	} 

	return DBOX_STATUS_ERROR_NOT_EXIST;
}

/* End of a file */
