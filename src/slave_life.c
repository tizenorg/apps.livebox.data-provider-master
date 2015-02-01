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
#include <string.h> /* strerror */
#include <errno.h> /* errno */
#include <unistd.h> /* pid_t */
#include <stdlib.h> /* free */
#include <pthread.h>
#include <malloc.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <Eina.h>
#include <Ecore.h>

#include <aul.h> /* aul_launch_app */
#include <dlog.h>
#include <bundle.h>

#include <packet.h>
#include <dynamicbox_errno.h>
#include <dynamicbox_conf.h>
#include <dynamicbox_cmd_list.h>
#include <dynamicbox_service.h>

#include "critical_log.h"
#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "fault_manager.h"
#include "debug.h"
#include "conf.h"
#include "setting.h"
#include "util.h"
#include "abi.h"
#include "xmonitor.h"

#include "package.h"
#include "instance.h"

#define BUNDLE_SLAVE_SVC_OP_TYPE "__APP_SVC_OP_TYPE__"
#define APP_CONTROL_OPERATION_MAIN "http://tizen.org/appcontrol/operation/main"
#define LOW_PRIORITY	10

int errno;

struct slave_node {
	char *name;
	char *abi;
	char *pkgname;
	int secured;	/* Only A package(dynamicbox) is loaded for security requirements */
	int refcnt;
	int fault_count;
	int critical_fault_count;
	enum slave_state state;
	int network;

	int loaded_instance;
	int loaded_package;

	int reactivate_instances;
	int reactivate_slave;

	pid_t pid;

	enum event_process {
		SLAVE_EVENT_PROCESS_IDLE = 0x00,
		SLAVE_EVENT_PROCESS_ACTIVATE = 0x01,
		SLAVE_EVENT_PROCESS_DEACTIVATE = 0x02,
		SLAVE_EVENT_PROCESS_DELETE = 0x04,
		SLAVE_EVENT_PROCESS_FAULT = 0x08,
		SLAVE_EVENT_PROCESS_PAUSE = 0x10,
		SLAVE_EVENT_PROCESS_RESUME = 0x20
	} in_event_process;
	Eina_List *event_activate_list;
	Eina_List *event_deactivate_list;
	Eina_List *event_delete_list;
	Eina_List *event_fault_list;
	Eina_List *event_pause_list;
	Eina_List *event_resume_list;

	Eina_List *data_list;

	Ecore_Timer *ttl_timer; /*!< Time to live */
	Ecore_Timer *activate_timer; /*!< Waiting hello packet for this time */
	Ecore_Timer *relaunch_timer; /*!< Try to relaunch service app */
	Ecore_Timer *terminate_timer; /*!< Waiting this timer before terminate the service provider */
	int relaunch_count;

	int ctrl_option;

#if defined(_USE_ECORE_TIME_GET)
	double activated_at;
#else
	struct timeval activated_at;
#endif

	char *hw_acceleration;
};

struct event {
	struct slave_node *slave;

	int (*evt_cb)(struct slave_node *, void *);
	void *cbdata;
	int deleted;
};

struct priv_data {
	char *tag;
	void *data;
};

static struct {
	Eina_List *slave_list;
	int deactivate_all_refcnt;
} s_info = {
	.slave_list = NULL,
	.deactivate_all_refcnt = 0,
};

static Eina_Bool terminate_timer_cb(void *data)
{
	struct slave_node *slave = data;
	int ret;

	/*!
	 * \todo
	 * check the return value of the aul_terminate_pid
	 */
	slave->state = SLAVE_REQUEST_TO_TERMINATE;
	slave->terminate_timer = NULL;

	DbgPrint("Terminate slave: %d (%s)\n", slave_pid(slave), slave_name(slave));
	ret = aul_terminate_pid_async(slave->pid);
	if (ret < 0) {
		ErrPrint("Terminate slave(%s) failed. pid %d (%d)\n", slave_name(slave), slave_pid(slave), ret);
		slave = slave_deactivated(slave);
		if (slave == NULL) {
			DbgPrint("Slave is deleted\n");
		}
	}

	return ECORE_CALLBACK_CANCEL;
}

static struct slave_node *slave_deactivate(struct slave_node *slave, int no_timer)
{
	int ret;

	/**
	 * @note
	 * The caller must has to check the slave's state.
	 * If it is not activated, this function must has not to be called.
	 */

	if (slave_pid(slave) <= 0) {
		return slave;
	}

	if (slave->ttl_timer) {
		/**
		 * @note
		 * If there is an activated TTL timer,
		 * It has to be cleared before deactivate the slave.
		 */
		ecore_timer_del(slave->ttl_timer);
		slave->ttl_timer = NULL;
	}

	if ((slave->ctrl_option & PROVIDER_CTRL_MANUAL_TERMINATION) == PROVIDER_CTRL_MANUAL_TERMINATION) {
		/*!
		 * \note
		 * In this case,
		 * The provider requests MANUAL TERMINATION control option.
		 * Master will not send terminate request in this case, the provider should be terminated by itself.
		 */
		DbgPrint("Manual termination is turned on\n");
		slave->state = SLAVE_REQUEST_TO_DISCONNECT;
		(void)slave_rpc_disconnect(slave);
	} else if (slave->terminate_timer) {
		ErrPrint("Terminate timer is already fired (%d)\n", slave->pid);
	} else if (!no_timer && !slave->secured) {
		DbgPrint("Fire the terminate timer: %d\n", slave->pid);
		slave->terminate_timer = ecore_timer_add(DYNAMICBOX_CONF_SLAVE_ACTIVATE_TIME, terminate_timer_cb, slave);
		if (!slave->terminate_timer) {
			/*!
			 * \note
			 * Normally, Call the terminate_timer_cb directly from here
			 * But in this case. if the aul_terminate_pid failed, Call the slave_deactivated function directly.
			 * Then the "slave" pointer can be changed. To trace it, Copy the body of terminate_timer_cb to here.
			 */
			ErrPrint("Failed to add a new timer for terminating\n");
			DbgPrint("Terminate slave: %d (%s)\n", slave_pid(slave), slave_name(slave));
			/*!
			 * \todo
			 * check the return value of the aul_terminate_pid
			 */
			slave->state = SLAVE_REQUEST_TO_TERMINATE;

			ret = aul_terminate_pid_async(slave->pid);
			if (ret < 0) {
				ErrPrint("Terminate slave(%s) failed. pid %d (%d)\n", slave_name(slave), slave_pid(slave), ret);
				slave = slave_deactivated(slave);
			}
		}
	} else {
		/*!
		 * \todo
		 * check the return value of the aul_terminate_pid
		 */
		slave->state = SLAVE_REQUEST_TO_TERMINATE;

		DbgPrint("Terminate slave: %d (%s)\n", slave_pid(slave), slave_name(slave));
		ret = aul_terminate_pid_async(slave->pid);
		if (ret < 0) {
			ErrPrint("Terminate slave(%s) failed. pid %d (%d)\n", slave_name(slave), slave_pid(slave), ret);
			slave = slave_deactivated(slave);
		}
	}

	return slave;
}

static Eina_Bool slave_ttl_cb(void *data)
{
	struct pkg_info *info;
	struct slave_node *slave = (struct slave_node *)data;
	Eina_List *l;
	Eina_List *pkg_list;

	pkg_list = (Eina_List *)package_list();
	EINA_LIST_FOREACH(pkg_list, l, info) {
		if (package_slave(info) == slave) {
			struct inst_info *inst;
			Eina_List *inst_list;
			Eina_List *n;

			inst_list = (Eina_List *)package_instance_list(info);
			EINA_LIST_FOREACH(inst_list, n, inst) {
				if (instance_visible_state(inst) == DBOX_SHOW) {
					DbgPrint("Instance is in show, give more ttl to %d for %s\n", slave_pid(slave), instance_id(inst));
					return ECORE_CALLBACK_RENEW;
				}
			}
		}
	}

	/*!
	 * \note
	 * ttl_timer must has to be set to NULL before deactivate the slave
	 * It will be used for making decision of the expired TTL timer or the fault of a dynamicbox.
	 */
	slave->ttl_timer = NULL;

	if (slave_is_activated(slave)) {
		slave_set_reactivation(slave, 0);
		slave_set_reactivate_instances(slave, 1);

		slave = slave_deactivate(slave, 1);
		if (!slave) {
			DbgPrint("Slave is deleted\n");
		}
	}

	/*! To recover all instances state it is activated again */
	return ECORE_CALLBACK_CANCEL;
}

static inline int xmonitor_pause_cb(void *data)
{
	slave_pause(data);
	return DBOX_STATUS_ERROR_NONE;
}

static inline int xmonitor_resume_cb(void *data)
{
	slave_resume(data);
	return DBOX_STATUS_ERROR_NONE;
}

static inline struct slave_node *create_slave_node(const char *name, int is_secured, const char *abi, const char *pkgname, int network, const char *hw_acceleration)
{
	struct slave_node *slave;

	slave = calloc(1, sizeof(*slave));
	if (!slave) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	slave->name = strdup(name);
	if (!slave->name) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(slave);
		return NULL;
	}

	slave->abi = strdup(abi);
	if (!slave->abi) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(slave->name);
		DbgFree(slave);
		return NULL;
	}

	slave->pkgname = strdup(pkgname);
	if (!slave->pkgname) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(slave->abi);
		DbgFree(slave->name);
		DbgFree(slave);
		return NULL;
	}

	if (hw_acceleration) {
		slave->hw_acceleration = strdup(hw_acceleration);
		if (!slave->hw_acceleration) {
			ErrPrint("Heap: %s\n", strerror(errno));
			DbgFree(slave->pkgname);
			DbgFree(slave->abi);
			DbgFree(slave->name);
			DbgFree(slave);
			return NULL;
		}
	}

	slave->secured = is_secured;
	slave->pid = (pid_t)-1;
	slave->state = SLAVE_TERMINATED;
	slave->network = network;
	slave->relaunch_count = DYNAMICBOX_CONF_SLAVE_RELAUNCH_COUNT;

	xmonitor_add_event_callback(XMONITOR_PAUSED, xmonitor_pause_cb, slave);
	xmonitor_add_event_callback(XMONITOR_RESUMED, xmonitor_resume_cb, slave);

	s_info.slave_list = eina_list_append(s_info.slave_list, slave);
	return slave;
}

static inline void invoke_delete_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;

	slave->in_event_process |= SLAVE_EVENT_PROCESS_DELETE;
	EINA_LIST_FOREACH_SAFE(slave->event_delete_list, l, n, event) {
		if (event->deleted || event->evt_cb(event->slave, event->cbdata) < 0 || event->deleted) {
			slave->event_delete_list = eina_list_remove(slave->event_delete_list, event);
			DbgFree(event);
		}
	}
	slave->in_event_process &= ~SLAVE_EVENT_PROCESS_DELETE;
}

static inline void destroy_slave_node(struct slave_node *slave)
{
	struct event *event;
	struct priv_data *priv;

	if (slave_pid(slave) != (pid_t)-1) {
		ErrPrint("Slave is not deactivated\n");
		return;
	}

	xmonitor_del_event_callback(XMONITOR_PAUSED, xmonitor_pause_cb, slave);
	xmonitor_del_event_callback(XMONITOR_RESUMED, xmonitor_resume_cb, slave);

	invoke_delete_cb(slave);
	slave_rpc_fini(slave); /*!< Finalize the RPC after handling all delete callbacks */

	EINA_LIST_FREE(slave->event_delete_list, event) {
		DbgFree(event);
	}

	EINA_LIST_FREE(slave->event_activate_list, event) {
		DbgFree(event);
	}

	EINA_LIST_FREE(slave->event_deactivate_list, event) {
		DbgFree(event);
	}

	EINA_LIST_FREE(slave->event_fault_list, event) {
		DbgFree(event);
	}

	EINA_LIST_FREE(slave->data_list, priv) {
		DbgFree(priv->tag);
		DbgFree(priv);
	}

	s_info.slave_list = eina_list_remove(s_info.slave_list, slave);

	if (slave->ttl_timer) {
		ecore_timer_del(slave->ttl_timer);
	}

	if (slave->activate_timer) {
		ecore_timer_del(slave->activate_timer);
	}

	if (slave->relaunch_timer) {
		ecore_timer_del(slave->relaunch_timer);
	}

	DbgFree(slave->abi);
	DbgFree(slave->name);
	DbgFree(slave->pkgname);
	DbgFree(slave->hw_acceleration);
	DbgFree(slave);
	return;
}

static inline struct slave_node *find_slave(const char *name)
{
	struct slave_node *slave;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (!strcmp(slave->name, name)) {
			return slave;
		}
	}

	return NULL;
}

HAPI int slave_expired_ttl(struct slave_node *slave)
{
	if (!slave) {
		return 0;
	}

	if (!slave->secured && !(DBOX_IS_INHOUSE(slave_abi(slave)) && DYNAMICBOX_CONF_SLAVE_LIMIT_TO_TTL)) {
		return 0;
	}

	return !!slave->ttl_timer;
}

HAPI struct slave_node *slave_ref(struct slave_node *slave)
{
	if (!slave) {
		return NULL;
	}

	slave->refcnt++;
	return slave;
}

HAPI struct slave_node *slave_unref(struct slave_node *slave)
{
	if (!slave) {
		return NULL;
	}

	if (slave->refcnt == 0) {
		ErrPrint("Slave refcnt is not valid\n");
		return NULL;
	}

	slave->refcnt--;
	if (slave->refcnt == 0) {
		destroy_slave_node(slave);
		slave = NULL;
	}

	return slave;
}

HAPI const int const slave_refcnt(struct slave_node *slave)
{
	return slave->refcnt;
}

HAPI struct slave_node *slave_create(const char *name, int is_secured, const char *abi, const char *pkgname, int network, const char *hw_acceleration)
{
	struct slave_node *slave;

	slave = find_slave(name);
	if (slave) {
		if (slave->secured != is_secured) {
			ErrPrint("Exists slave and creating slave's security flag is not matched\n");
		}
		return slave;
	}

	slave = create_slave_node(name, is_secured, abi, pkgname, network, hw_acceleration);
	if (!slave) {
		return NULL;
	}

	slave_ref(slave);
	slave_rpc_init(slave);

	return slave;
}

/*!
 * \note
 * Before destroying slave object,
 * you should check the RPC(slave_async_XXX) state and Private data field (slave_set_data)
 */
HAPI void slave_destroy(struct slave_node *slave)
{
	slave_unref(slave);
}

static inline struct slave_node *invoke_fault_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;

	slave_ref(slave);
	slave->in_event_process |= SLAVE_EVENT_PROCESS_FAULT;
	EINA_LIST_FOREACH_SAFE(slave->event_fault_list, l, n, event) {
		if (event->deleted || event->evt_cb(event->slave, event->cbdata) < 0 || event->deleted) {
			slave->event_fault_list = eina_list_remove(slave->event_fault_list, event);
			DbgFree(event);
		}
	}
	slave->in_event_process &= ~SLAVE_EVENT_PROCESS_FAULT;
	slave = slave_unref(slave);

	return slave;
}

static inline void invoke_activate_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;

	slave->in_event_process |= SLAVE_EVENT_PROCESS_ACTIVATE;
	EINA_LIST_FOREACH_SAFE(slave->event_activate_list, l, n, event) {
		if (event->deleted || event->evt_cb(event->slave, event->cbdata) < 0 || event->deleted) {
			slave->event_activate_list = eina_list_remove(slave->event_activate_list, event);
			DbgFree(event);
		}
	}
	slave->in_event_process &= ~SLAVE_EVENT_PROCESS_ACTIVATE;
}

static Eina_Bool activate_timer_cb(void *data)
{
	struct slave_node *slave = data;

	if (slave->relaunch_timer) {
		ecore_timer_del(slave->relaunch_timer);
		slave->relaunch_timer = NULL;
	}

	slave->fault_count++;

	if (invoke_fault_cb(slave) == NULL) {
		ErrPrint("Slave is deleted while processing fault handler\n");
		return ECORE_CALLBACK_CANCEL;
	}

	slave_set_reactivation(slave, 0);
	slave_set_reactivate_instances(slave, 0);

	slave->activate_timer = NULL;
	if (slave_pid(slave) > 0) {
		int ret;
		DbgPrint("Try to terminate PID: %d\n", slave_pid(slave));
		ret = aul_terminate_pid_async(slave_pid(slave));
		if (ret < 0) {
			ErrPrint("Terminate failed, pid %d (reason: %d)\n", slave_pid(slave), ret);
		}
	}

	CRITICAL_LOG("Slave is not activated in %lf sec (slave: %s)\n", DYNAMICBOX_CONF_SLAVE_ACTIVATE_TIME, slave_name(slave));
	slave = slave_deactivated(slave);
	return ECORE_CALLBACK_CANCEL;
}

static inline void invoke_slave_fault_handler(struct slave_node *slave)
{
	slave->fault_count++;
	if (invoke_fault_cb(slave) == NULL) {
		ErrPrint("Slave is deleted while processing fault handler\n");
		return;
	}

	slave_set_reactivation(slave, 0);
	slave_set_reactivate_instances(slave, 0);

	if (slave_pid(slave) > 0) {
		if ((slave->ctrl_option & PROVIDER_CTRL_MANUAL_TERMINATION) == PROVIDER_CTRL_MANUAL_TERMINATION) {
			DbgPrint("Manual termination is turned on\n");
			(void)slave_rpc_disconnect(slave);
		} else {
			int ret;
			DbgPrint("Try to terminate PID: %d\n", slave_pid(slave));
			ret = aul_terminate_pid_async(slave_pid(slave));
			if (ret < 0) {
				ErrPrint("Terminate failed, pid %d (reason: %d)\n", slave_pid(slave), ret);
			}
		}
	}

	slave->state = SLAVE_TERMINATED;
}

static Eina_Bool relaunch_timer_cb(void *data)
{
	struct slave_node *slave = data;
	int ret = ECORE_CALLBACK_CANCEL;

	if (!slave->activate_timer) {
		ErrPrint("Activate timer is not valid\n");
		slave->relaunch_timer = NULL;

		invoke_slave_fault_handler(slave);
	} else if (!slave->relaunch_count) {
		ErrPrint("Relaunch count is exhahausted\n");
		ecore_timer_del(slave->activate_timer);
		slave->activate_timer = NULL;

		slave->relaunch_timer = NULL;
		invoke_slave_fault_handler(slave);
	} else {
		bundle *param;

		param = bundle_create();
		if (!param) {
			ErrPrint("Failed to create a bundle\n");

			ecore_timer_del(slave->activate_timer);
			slave->activate_timer = NULL;

			slave->relaunch_timer = NULL;

			invoke_slave_fault_handler(slave);
		} else {
			bundle_add(param, BUNDLE_SLAVE_SVC_OP_TYPE, APP_CONTROL_OPERATION_MAIN);
			bundle_add(param, DYNAMICBOX_CONF_BUNDLE_SLAVE_NAME, slave_name(slave));
			bundle_add(param, DYNAMICBOX_CONF_BUNDLE_SLAVE_SECURED, ((DBOX_IS_INHOUSE(slave_abi(slave)) && DYNAMICBOX_CONF_SLAVE_LIMIT_TO_TTL) || slave->secured) ? "true" : "false");
			bundle_add(param, DYNAMICBOX_CONF_BUNDLE_SLAVE_ABI, slave->abi);
			bundle_add(param, DYNAMICBOX_CONF_BUNDLE_SLAVE_HW_ACCELERATION, slave->hw_acceleration);

			slave->pid = (pid_t)aul_launch_app(slave_pkgname(slave), param);

			bundle_free(param);

			switch (slave->pid) {
			case AUL_R_EHIDDENFORGUEST:	/**< App hidden for guest mode */
			case AUL_R_ENOLAUNCHPAD:	/**< no launchpad */
			case AUL_R_EILLACC:		/**< Illegal Access */
			case AUL_R_EINVAL:		/**< Invalid argument */
			case AUL_R_ENOINIT:		/**< AUL handler NOT initialized */
			case AUL_R_ERROR:		/**< General error */
				CRITICAL_LOG("Failed to launch a new slave %s (%d)\n", slave_name(slave), slave->pid);
				slave->pid = (pid_t)-1;
				ecore_timer_del(slave->activate_timer);
				slave->activate_timer = NULL;

				slave->relaunch_timer = NULL;

				invoke_slave_fault_handler(slave);
				/* Waiting app-launch result */
				break;
			case AUL_R_ETIMEOUT:		/**< Timeout */
			case AUL_R_ECOMM:		/**< Comunication Error */
			case AUL_R_ETERMINATING:	/**< application terminating */
			case AUL_R_ECANCELED:		/**< Operation canceled */
				slave->relaunch_count--;

				CRITICAL_LOG("Try relaunch again %s (%d), %d\n", slave_name(slave), slave->pid, slave->relaunch_count);
				slave->pid = (pid_t)-1;
				ret = ECORE_CALLBACK_RENEW;
				ecore_timer_reset(slave->activate_timer);
				/* Try again after a few secs later */
				break;
			case AUL_R_LOCAL:		/**< Launch by himself */
			case AUL_R_OK:			/**< General success */
			default:
				DbgPrint("Slave %s is launched with %d as %s\n", slave_pkgname(slave), slave->pid, slave_name(slave));
				slave->relaunch_timer = NULL;
				ecore_timer_reset(slave->activate_timer);
				break;
			}
		}

	}

	return ret;
}

HAPI int slave_activate(struct slave_node *slave)
{
	/**
	 * @todo
	 * If a slave is deactivated by OOM killer,
	 * We should not activate it again from here.
	 * Instead of this, it should be reactivated by user event or oom event(normal)
	 */

	/**
	 * @note
	 * This check code can be replaced with the slave->state check code
	 * If the slave data has the PID, it means, it is activated
	 * Even if it is in the termiating sequence, it will have the PID
	 * until it is terminated at last.
	 * So we can use this simple code for checking the slave's last state.
	 * whether it is alive or not.
	 */
	if (slave_pid(slave) != (pid_t)-1) {
		if (slave->terminate_timer) {
			DbgPrint("Clear terminate timer. to reuse (%d)\n", slave->pid);
			ecore_timer_del(slave->terminate_timer);
			slave->terminate_timer = NULL;
		} else if (slave_state(slave) == SLAVE_REQUEST_TO_TERMINATE || slave_state(slave) == SLAVE_REQUEST_TO_DISCONNECT) {
			slave_set_reactivation(slave, 1);
		}
		return DBOX_STATUS_ERROR_ALREADY;
	} else if (slave_state(slave) == SLAVE_REQUEST_TO_LAUNCH) {
		DbgPrint("Slave is already launched: but the AUL is timed out\n");
		return DBOX_STATUS_ERROR_ALREADY;
	}

	if (DYNAMICBOX_CONF_DEBUG_MODE || g_conf.debug_mode) {
		DbgPrint("Debug Mode enabled. name[%s] secured[%d] abi[%s]\n", slave_name(slave), slave->secured, slave->abi);
	} else {
		bundle *param;

		slave->relaunch_count = DYNAMICBOX_CONF_SLAVE_RELAUNCH_COUNT;

		param = bundle_create();
		if (!param) {
			ErrPrint("Failed to create a bundle\n");
			return DBOX_STATUS_ERROR_FAULT;
		}

		bundle_add(param, BUNDLE_SLAVE_SVC_OP_TYPE, APP_CONTROL_OPERATION_MAIN);
		bundle_add(param, DYNAMICBOX_CONF_BUNDLE_SLAVE_NAME, slave_name(slave));
		bundle_add(param, DYNAMICBOX_CONF_BUNDLE_SLAVE_SECURED, ((DBOX_IS_INHOUSE(slave_abi(slave)) && DYNAMICBOX_CONF_SLAVE_LIMIT_TO_TTL) || slave->secured) ? "true" : "false");
		bundle_add(param, DYNAMICBOX_CONF_BUNDLE_SLAVE_ABI, slave->abi);
		bundle_add(param, DYNAMICBOX_CONF_BUNDLE_SLAVE_HW_ACCELERATION, slave->hw_acceleration);

		slave->pid = (pid_t)aul_launch_app(slave_pkgname(slave), param);

		bundle_free(param);

		switch (slave->pid) {
		case AUL_R_EHIDDENFORGUEST:	/**< App hidden for guest mode */
		case AUL_R_ENOLAUNCHPAD:	/**< no launchpad */
		case AUL_R_EILLACC:		/**< Illegal Access */
		case AUL_R_EINVAL:		/**< Invalid argument */
		case AUL_R_ENOINIT:		/**< AUL handler NOT initialized */
		case AUL_R_ERROR:		/**< General error */
			CRITICAL_LOG("Failed to launch a new slave %s (%d)\n", slave_name(slave), slave->pid);
			slave->pid = (pid_t)-1;
			/* Waiting app-launch result */
			break;
		case AUL_R_ECOMM:		/**< Comunication Error */
		case AUL_R_ETERMINATING:	/**< application terminating */
		case AUL_R_ECANCELED:		/**< Operation canceled */
		case AUL_R_ETIMEOUT:		/**< Timeout */
			CRITICAL_LOG("Try relaunch this soon %s (%d)\n", slave_name(slave), slave->pid);
			slave->relaunch_timer = ecore_timer_add(DYNAMICBOX_CONF_SLAVE_RELAUNCH_TIME, relaunch_timer_cb, slave);
			if (!slave->relaunch_timer) {
				CRITICAL_LOG("Failed to register a relaunch timer (%s)\n", slave_name(slave));
				slave->pid = (pid_t)-1;
				return DBOX_STATUS_ERROR_FAULT;
			}
			/* Try again after a few secs later */
			break;
		case AUL_R_LOCAL:		/**< Launch by himself */
		case AUL_R_OK:			/**< General success */
		default:
			DbgPrint("Slave %s is launched with %d as %s\n", slave_pkgname(slave), slave->pid, slave_name(slave));
			break;
		}

		slave->activate_timer = ecore_timer_add(DYNAMICBOX_CONF_SLAVE_ACTIVATE_TIME, activate_timer_cb, slave);
		if (!slave->activate_timer) {
			ErrPrint("Failed to register an activate timer\n");
		}
	}

	slave->state = SLAVE_REQUEST_TO_LAUNCH;
	/*!
	 * \note
	 * Increase the refcnt of a slave,
	 * To prevent from making an orphan(slave).
	 */
	(void)slave_ref(slave);

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int slave_give_more_ttl(struct slave_node *slave)
{
	double delay;

	if (!(DBOX_IS_INHOUSE(slave_abi(slave)) && DYNAMICBOX_CONF_SLAVE_LIMIT_TO_TTL) && (!slave->secured || !slave->ttl_timer)) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	delay = DYNAMICBOX_CONF_SLAVE_TTL - ecore_timer_pending_get(slave->ttl_timer);
	ecore_timer_delay(slave->ttl_timer, delay);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int slave_freeze_ttl(struct slave_node *slave)
{
	if (!(DBOX_IS_INHOUSE(slave_abi(slave)) && DYNAMICBOX_CONF_SLAVE_LIMIT_TO_TTL) && (!slave->secured || !slave->ttl_timer)) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	ecore_timer_freeze(slave->ttl_timer);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int slave_thaw_ttl(struct slave_node *slave)
{
	double delay;

	if (!(DBOX_IS_INHOUSE(slave_abi(slave)) && DYNAMICBOX_CONF_SLAVE_LIMIT_TO_TTL) && (!slave->secured || !slave->ttl_timer)) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	ecore_timer_thaw(slave->ttl_timer);

	delay = DYNAMICBOX_CONF_SLAVE_TTL - ecore_timer_pending_get(slave->ttl_timer);
	ecore_timer_delay(slave->ttl_timer, delay);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI int slave_activated(struct slave_node *slave)
{
	slave->state = SLAVE_RESUMED;

	if (xmonitor_is_paused()) {
		slave_pause(slave);
	}

	if (((DBOX_IS_INHOUSE(slave_abi(slave)) && DYNAMICBOX_CONF_SLAVE_LIMIT_TO_TTL) || slave->secured == 1) && DYNAMICBOX_CONF_SLAVE_TTL > 0.0f) {
		DbgPrint("Slave deactivation timer is added (%s - %lf)\n", slave_name(slave), DYNAMICBOX_CONF_SLAVE_TTL);
		slave->ttl_timer = ecore_timer_add(DYNAMICBOX_CONF_SLAVE_TTL, slave_ttl_cb, slave);
		if (!slave->ttl_timer) {
			ErrPrint("Failed to create a TTL timer\n");
		}
	}

	invoke_activate_cb(slave);

	slave_set_reactivation(slave, 0);
	slave_set_reactivate_instances(slave, 0);

#if defined(_USE_ECORE_TIME_GET)
	slave->activated_at = ecore_time_get();
#else
	if (gettimeofday(&slave->activated_at, NULL) < 0) {
		ErrPrint("Failed to get time of day: %s\n", strerror(errno));
		slave->activated_at.tv_sec = 0;
		slave->activated_at.tv_usec = 0;
	}
#endif

	if (slave->activate_timer) {
		ecore_timer_del(slave->activate_timer);
		slave->activate_timer = NULL;
	}

	if (slave->relaunch_timer) {
		ecore_timer_del(slave->relaunch_timer);
		slave->relaunch_timer = NULL;
	}

	slave_set_priority(slave, LOW_PRIORITY);
	return DBOX_STATUS_ERROR_NONE;
}

static inline int invoke_deactivate_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;
	int ret;
	int reactivate = 0;

	slave->in_event_process |= SLAVE_EVENT_PROCESS_DEACTIVATE;

	EINA_LIST_FOREACH_SAFE(slave->event_deactivate_list, l, n, event) {
		if (event->deleted) {
			slave->event_deactivate_list = eina_list_remove(slave->event_deactivate_list, event);
			DbgFree(event);
			continue;
		}

		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0 || event->deleted) {
			slave->event_deactivate_list = eina_list_remove(slave->event_deactivate_list, event);
			DbgFree(event);
		}

		if (ret == SLAVE_NEED_TO_REACTIVATE) {
			reactivate++;
		}
	}

	slave->in_event_process &= ~SLAVE_EVENT_PROCESS_DEACTIVATE;

	return reactivate;
}

HAPI struct slave_node *slave_deactivated(struct slave_node *slave)
{
	int reactivate;

	slave->pid = (pid_t)-1;
	slave->state = SLAVE_TERMINATED;

	if (slave->ttl_timer) {
		ecore_timer_del(slave->ttl_timer);
		slave->ttl_timer = NULL;
	}

	if (slave->activate_timer) {
		ecore_timer_del(slave->activate_timer);
		slave->activate_timer = NULL;
	}

	if (slave->relaunch_timer) {
		ecore_timer_del(slave->relaunch_timer);
		slave->relaunch_timer = NULL;
	}

	if (slave->terminate_timer) {
		ecore_timer_del(slave->terminate_timer);
		slave->terminate_timer = NULL;
	}

	/**
	 * @note
	 * FOR SAFETY
	 * If the deactivated event callback is called for package.c
	 * It can delete the instance if it has fault information
	 * then it also try to delete the slave object again.
	 * To prevent from unexpected slave object deletetion while handling callback,
	 * increase the refcnt of slave
	 * when it get back from callback, try to decrease the refcnt of slave
	 * At that time, we can delete slave safely.
	 */
	slave_ref(slave);
	reactivate = invoke_deactivate_cb(slave);
	slave = slave_unref(slave);
	if (!slave) {
		ErrPrint("Slave object is deleted\n");
		return slave;
	}

	slave = slave_unref(slave);
	if (!slave) {
		DbgPrint("SLAVE object is destroyed\n");
		return slave;
	}

	if ((slave->ctrl_option & PROVIDER_CTRL_MANUAL_REACTIVATION) == PROVIDER_CTRL_MANUAL_REACTIVATION) {
		/*!
		 * \note
		 * In this case, the provider(Slave) should be reactivated by itself or user.
		 * The master will not reactivate it automatically.
		 */
		DbgPrint("Manual reactivate option is turned on\n");
	} else if (reactivate && slave_need_to_reactivate(slave) && setting_oom_level() == OOM_TYPE_NORMAL) {
		int ret;

		DbgPrint("Need to reactivate a slave\n");
		ret = slave_activate(slave);
		if (ret < 0 && ret != DBOX_STATUS_ERROR_ALREADY) {
			ErrPrint("Failed to reactivate a slave\n");
		}
	} else if (slave_loaded_instance(slave) == 0) {
		/*!
		 * \note
		 * If a slave has no more instances,
		 * Destroy it
		 */
		slave = slave_unref(slave);
	}

	return slave;
}

HAPI struct slave_node *slave_deactivated_by_fault(struct slave_node *slave)
{
	int ret;
	int reactivate = 1;
	int reactivate_instances = 1;
	int max_load;

	if (g_conf.slave_max_load < 0) {
		max_load = DYNAMICBOX_CONF_SLAVE_MAX_LOAD;
	} else {
		max_load = g_conf.slave_max_load;
	}

	if (!slave_is_activated(slave)) {
		DbgPrint("Deactivating in progress\n");
		if (slave_loaded_instance(slave) == 0) {
			slave = slave_unref(slave);
		}

		return slave;
	}

	slave->fault_count++;

	(void)fault_check_pkgs(slave);

	if (slave_pid(slave) > 0) {
		DbgPrint("Try to terminate PID: %d\n", slave_pid(slave));
		ret = aul_terminate_pid_async(slave_pid(slave));
		if (ret < 0) {
			ErrPrint("Terminate failed, pid %d\n", slave_pid(slave));
		}
	}

#if defined(_USE_ECORE_TIME_GET)
	double faulted_at;

	faulted_at = ecore_time_get();
	if (faulted_at - slave->activated_at < DYNAMICBOX_CONF_MINIMUM_REACTIVATION_TIME) {
		slave->critical_fault_count++;

		if (!slave_loaded_instance(slave) || slave->critical_fault_count >= max_load) {
			ErrPrint("Reactivation time is too fast and frequently occurred - Stop to auto reactivation\n");
			reactivate = 0;
			reactivate_instances = 0;
			slave->critical_fault_count = 0;
			/*!
			 * \note
			 * Fault callback can access the slave information.
			 */
			if (invoke_fault_cb(slave) == NULL) {
				ErrPrint("Slave is deleted while processing fault handler\n");
				return NULL;
			}
		} else {
			slave->critical_fault_count = 0;
		}
	}
#else
	struct timeval faulted_at;

	if (gettimeofday(&faulted_at, NULL) == 0) {
		struct timeval rtv;

		timersub(&faulted_at, &slave->activated_at, &rtv);
		if (rtv.tv_sec < DYNAMICBOX_CONF_MINIMUM_REACTIVATION_TIME) {
			slave->critical_fault_count++;
			if (!slave_loaded_instance(slave) || slave->critical_fault_count >= max_load) {
				ErrPrint("Reactivation time is too fast and frequently occurred - Stop to auto reactivation\n");
				reactivate = 0;
				reactivate_instances = 0;
				slave->critical_fault_count = 0;
				/*!
				 * \note
				 * Fault callback can access the slave information.
				 */
				if (invoke_fault_cb(slave) == NULL) {
					ErrPrint("Slave is deleted while processing fault handler\n");
					return NULL;
				}
			}
		} else {
			slave->critical_fault_count = 0;
		}
	} else {
		ErrPrint("Failed to get time of day: %s\n", strerror(errno));
	}
#endif

	slave_set_reactivation(slave, reactivate);
	slave_set_reactivate_instances(slave, reactivate_instances);

	slave = slave_deactivated(slave);
	return slave;
}

HAPI const int const slave_is_activated(struct slave_node *slave)
{
	switch (slave->state) {
	case SLAVE_REQUEST_TO_TERMINATE:
	case SLAVE_TERMINATED:
		return 0;
	case SLAVE_REQUEST_TO_DISCONNECT:
		/* This case should be treated as an activated state.
		 * To send the last request to the provider.
		 */
	case SLAVE_REQUEST_TO_LAUNCH:
		/* Not yet launched. but the slave incurred an unexpected error */
	case SLAVE_REQUEST_TO_PAUSE:
	case SLAVE_REQUEST_TO_RESUME:
	case SLAVE_PAUSED:
	case SLAVE_RESUMED:
		return 1;
	default:
		return slave->pid != (pid_t)-1;
	}

	/* Could not be reach to here */
	return 0;
}

HAPI int slave_event_callback_add(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data)
{
	struct event *ev;

	ev = calloc(1, sizeof(*ev));
	if (!ev) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	ev->slave = slave;
	ev->cbdata = data;
	ev->evt_cb = cb;

	/*!
	 * \note
	 * Use the eina_list_prepend API.
	 * To keep the sequence of a callback invocation.
	 *
	 * Here is an example sequence.
	 *
	 * slave_event_callback_add(CALLBACK_01);
	 * slave_event_callback_add(CALLBACK_02);
	 * slave_event_callback_add(CALLBACK_03);
	 *
	 * Then the invoke_event_callback function will call the CALLBACKS as below sequence
	 *
	 * invoke_CALLBACK_03
	 * invoke_CALLBACK_02
	 * invoke_CALLBACK_01
	 */

	switch (event) {
	case SLAVE_EVENT_ACTIVATE:
		slave->event_activate_list = eina_list_prepend(slave->event_activate_list, ev);
		break;
	case SLAVE_EVENT_DELETE:
		slave->event_delete_list = eina_list_prepend(slave->event_delete_list, ev);
		break;
	case SLAVE_EVENT_DEACTIVATE:
		slave->event_deactivate_list = eina_list_prepend(slave->event_deactivate_list, ev);
		break;
	case SLAVE_EVENT_PAUSE:
		slave->event_pause_list = eina_list_prepend(slave->event_pause_list, ev);
		break;
	case SLAVE_EVENT_RESUME:
		slave->event_resume_list = eina_list_prepend(slave->event_resume_list, ev);
		break;
	case SLAVE_EVENT_FAULT:
		slave->event_fault_list = eina_list_prepend(slave->event_fault_list, ev);
		break;
	default:
		DbgFree(ev);
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int slave_event_callback_del(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data)
{
	struct event *ev;
	Eina_List *l;
	Eina_List *n;

	switch (event) {
	case SLAVE_EVENT_DEACTIVATE:
		EINA_LIST_FOREACH_SAFE(slave->event_deactivate_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				if (slave->in_event_process & SLAVE_EVENT_PROCESS_DEACTIVATE) {
					ev->deleted = 1;
				} else {
					slave->event_deactivate_list = eina_list_remove(slave->event_deactivate_list, ev);
					DbgFree(ev);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;
	case SLAVE_EVENT_DELETE:
		EINA_LIST_FOREACH_SAFE(slave->event_delete_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				if (slave->in_event_process & SLAVE_EVENT_PROCESS_DELETE) {
					ev->deleted = 1;
				} else {
					slave->event_delete_list = eina_list_remove(slave->event_delete_list, ev);
					DbgFree(ev);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;
	case SLAVE_EVENT_ACTIVATE:
		EINA_LIST_FOREACH_SAFE(slave->event_activate_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				if (slave->in_event_process & SLAVE_EVENT_PROCESS_ACTIVATE) {
					ev->deleted = 1;
				} else {
					slave->event_activate_list = eina_list_remove(slave->event_activate_list, ev);
					DbgFree(ev);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;
	case SLAVE_EVENT_PAUSE:
		EINA_LIST_FOREACH_SAFE(slave->event_pause_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				if (slave->in_event_process & SLAVE_EVENT_PROCESS_PAUSE) {
					ev->deleted = 1;
				} else {
					slave->event_pause_list = eina_list_remove(slave->event_pause_list, ev);
					DbgFree(ev);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;
	case SLAVE_EVENT_RESUME:
		EINA_LIST_FOREACH_SAFE(slave->event_resume_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				if (slave->in_event_process & SLAVE_EVENT_PROCESS_RESUME) {
					ev->deleted = 1;
				} else {
					slave->event_resume_list = eina_list_remove(slave->event_resume_list, ev);
					DbgFree(ev);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;
	case SLAVE_EVENT_FAULT:
		EINA_LIST_FOREACH_SAFE(slave->event_fault_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				if (slave->in_event_process & SLAVE_EVENT_PROCESS_FAULT) {
					ev->deleted = 1;
				} else {
					slave->event_fault_list = eina_list_remove(slave->event_fault_list, ev);
					DbgFree(ev);
				}
				return DBOX_STATUS_ERROR_NONE;
			}
		}
		break;
	default:
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	return DBOX_STATUS_ERROR_NOT_EXIST;
}

HAPI int slave_set_data(struct slave_node *slave, const char *tag, void *data)
{
	struct priv_data *priv;

	priv = calloc(1, sizeof(*priv));
	if (!priv) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	priv->tag = strdup(tag);
	if (!priv->tag) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(priv);
		return DBOX_STATUS_ERROR_OUT_OF_MEMORY;
	}

	priv->data = data;
	slave->data_list = eina_list_append(slave->data_list, priv);
	return DBOX_STATUS_ERROR_NONE;
}

HAPI void *slave_del_data(struct slave_node *slave, const char *tag)
{
	struct priv_data *priv;
	void *data;
	Eina_List *l;
	Eina_List *n;

	EINA_LIST_FOREACH_SAFE(slave->data_list, l, n, priv) {
		if (!strcmp(priv->tag, tag)) {
			slave->data_list = eina_list_remove(slave->data_list, priv);

			data = priv->data;
			DbgFree(priv->tag);
			DbgFree(priv);
			return data;
		}
	}

	return NULL;
}

HAPI void *slave_data(struct slave_node *slave, const char *tag)
{
	struct priv_data *priv;
	Eina_List *l;

	EINA_LIST_FOREACH(slave->data_list, l, priv) {
		if (!strcmp(priv->tag, tag)) {
			return priv->data;
		}
	}

	return NULL;
}

HAPI struct slave_node *slave_find_by_pid(pid_t pid)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave_pid(slave) == pid) {
			return slave;
		}
	}

	return NULL;
}

HAPI struct slave_node *slave_find_by_name(const char *name)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (!strcmp(slave_name(slave), name)) {
			return slave;
		}
	}

	return NULL;
}

HAPI struct slave_node *slave_find_available(const char *slave_pkgname, const char *abi, int secured, int network, const char *hw_acceleration)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave->secured != secured) {
			continue;
		}

		if ((slave->state == SLAVE_REQUEST_TO_TERMINATE || slave->state == SLAVE_REQUEST_TO_DISCONNECT) && slave->loaded_instance == 0) {
			/*!
			 * \note
			 * If a slave is in request_to_terminate state,
			 * and the slave object has no more intances,
			 * the slave object will be deleted soon.
			 * so we cannot reuse it.
			 *
			 * This object is not usable.
			 */
			continue;
		}

		if (strcasecmp(slave->abi, abi)) {
			continue;
		}

		if (strcasecmp(slave->pkgname, slave_pkgname)) {
			continue;
		}

		if (slave->hw_acceleration != hw_acceleration) {
			if (!slave->hw_acceleration || !hw_acceleration || strcasecmp(slave->hw_acceleration, hw_acceleration)) {
				continue;
			}
		}

		if (slave->secured) {
			if (slave->loaded_package == 0) {
				DbgPrint("Found secured slave - has no instances (%s)\n", slave_name(slave));
				return slave;
			}
		} else if (slave->network == network) {
			DbgPrint("slave[%s] loaded_package[%d] net: [%d]\n", slave_name(slave), slave->loaded_package, slave->network);
			if (!strcasecmp(abi, DYNAMICBOX_CONF_DEFAULT_ABI)) {
				int max_load;
				if (g_conf.slave_max_load < 0) {
					max_load = DYNAMICBOX_CONF_SLAVE_MAX_LOAD;
				} else {
					max_load = g_conf.slave_max_load;
				}

				if (slave->loaded_package < max_load) {
					return slave;
				}
			} else {
				return slave;
			}
		}
	}

	return NULL;
}

HAPI struct slave_node *slave_find_by_pkgname(const char *pkgname)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (!strcmp(slave_pkgname(slave), pkgname)) {
			if (slave_pid(slave) == (pid_t)-1) {
				return slave;
			}
		}
	}

	return NULL;
}

HAPI struct slave_node *slave_find_by_rpc_handle(int handle)
{
	Eina_List *l;
	struct slave_node *slave;

	if (handle <= 0) {
		ErrPrint("Invalid RPC handle: %d\n", handle);
		return NULL;
	}

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave_rpc_handle(slave) == handle) {
			return slave;
		}
	}

	/* Not found */
	return NULL;
}

HAPI char *slave_package_name(const char *abi, const char *lbid)
{
	char *s_pkgname;
	const char *tmp;

	tmp = abi_find_slave(abi);
	if (!tmp) {
		ErrPrint("Failed to find a proper pkgname of a slave\n");
		return NULL;
	}

	s_pkgname = util_replace_string(tmp, DYNAMICBOX_CONF_REPLACE_TAG_APPID, lbid);
	if (!s_pkgname) {
		DbgPrint("Failed to get replaced string\n");
		s_pkgname = strdup(tmp);
		if (!s_pkgname) {
			ErrPrint("Heap: %s\n", strerror(errno));
			return NULL;
		}
	}

	return s_pkgname;
}

HAPI void slave_load_package(struct slave_node *slave)
{
	slave->loaded_package++;
}

HAPI void slave_unload_package(struct slave_node *slave)
{
	if (!slave || slave->loaded_package == 0) {
		ErrPrint("Slave loaded package is not correct\n");
		return;
	}

	slave->loaded_package--;
}

HAPI void slave_load_instance(struct slave_node *slave)
{
	slave->loaded_instance++;
	DbgPrint("Instance: (%d)%d\n", slave_pid(slave), slave->loaded_instance);
}

HAPI int const slave_loaded_instance(struct slave_node *slave)
{
	return slave->loaded_instance;
}

HAPI int const slave_loaded_package(struct slave_node *slave)
{
	return slave->loaded_package;
}

HAPI struct slave_node *slave_unload_instance(struct slave_node *slave)
{
	if (!slave || slave->loaded_instance == 0) {
		ErrPrint("Slave loaded instance is not correct\n");
		return slave;
	}

	slave->loaded_instance--;
	DbgPrint("Instance: (%d)%d\n", slave_pid(slave), slave->loaded_instance);
	if (slave->loaded_instance == 0 && slave_is_activated(slave)) {
		slave_set_reactivation(slave, 0);
		slave_set_reactivate_instances(slave, 0);

		slave = slave_deactivate(slave, 0);
	}

	return slave;
}

HAPI const int const slave_is_secured(const struct slave_node *slave)
{
	return slave->secured;
}

HAPI const char * const slave_name(const struct slave_node *slave)
{
	return slave->name;
}

HAPI const char * const slave_abi(const struct slave_node *slave)
{
	return slave->abi;
}

HAPI const pid_t const slave_pid(const struct slave_node *slave)
{
	return slave->pid;
}

HAPI int slave_set_pid(struct slave_node *slave, pid_t pid)
{
	if (!slave) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	DbgPrint("Slave PID is updated to %d from %d\n", pid, slave_pid(slave));

	slave->pid = pid;
	return DBOX_STATUS_ERROR_NONE;
}

static inline void invoke_resumed_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;

	slave->in_event_process |= SLAVE_EVENT_PROCESS_RESUME;
	EINA_LIST_FOREACH_SAFE(slave->event_resume_list, l, n, event) {
		if (event->deleted || event->evt_cb(event->slave, event->cbdata) < 0 || event->deleted) {
			slave->event_resume_list = eina_list_remove(slave->event_resume_list, event);
			DbgFree(event);
		}
	}
	slave->in_event_process &= ~SLAVE_EVENT_PROCESS_RESUME;
}

static void resume_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	int ret;

	if (slave->state == SLAVE_REQUEST_TO_TERMINATE || slave->state == SLAVE_REQUEST_TO_DISCONNECT) {
		DbgPrint("Slave is terminating now. ignore resume result\n");
		return;
	}

	if (!packet) {
		ErrPrint("Failed to change the state of the slave\n");
		slave->state = SLAVE_PAUSED;
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid parameter\n");
		return;
	}

	if (ret == 0) {
		slave->state = SLAVE_RESUMED;
		slave_rpc_ping_thaw(slave);
		invoke_resumed_cb(slave);
	}
}

static inline void invoke_paused_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;

	slave->in_event_process |= SLAVE_EVENT_PROCESS_PAUSE;
	EINA_LIST_FOREACH_SAFE(slave->event_pause_list, l, n, event) {
		if (event->deleted || event->evt_cb(event->slave, event->cbdata) < 0 || event->deleted) {
			slave->event_pause_list = eina_list_remove(slave->event_pause_list, event);
			DbgFree(event);
		}
	}
	slave->in_event_process &= ~SLAVE_EVENT_PROCESS_PAUSE;
}

static void pause_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	int ret;

	if (slave->state == SLAVE_REQUEST_TO_TERMINATE || slave->state == SLAVE_REQUEST_TO_DISCONNECT) {
		DbgPrint("Slave is terminating now. ignore pause result\n");
		return;
	}

	if (!packet) {
		ErrPrint("Failed to change the state of the slave\n");
		slave->state = SLAVE_RESUMED;
		return;
	}

	if (packet_get(packet, "i", &ret) != 1) {
		ErrPrint("Invalid parameter\n");
		return;
	}

	if (ret == 0) {
		slave->state = SLAVE_PAUSED;
		slave_rpc_ping_freeze(slave);
		invoke_paused_cb(slave);
	}
}

HAPI int slave_resume(struct slave_node *slave)
{
	double timestamp;
	struct packet *packet;
	unsigned int cmd = CMD_RESUME;

	switch (slave->state) {
	case SLAVE_REQUEST_TO_DISCONNECT:
	case SLAVE_REQUEST_TO_LAUNCH:
	case SLAVE_REQUEST_TO_TERMINATE:
	case SLAVE_TERMINATED:
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	case SLAVE_RESUMED:
	case SLAVE_REQUEST_TO_RESUME:
		return DBOX_STATUS_ERROR_NONE;
	default:
		break;
	}

	timestamp = util_timestamp();

	packet = packet_create((const char *)&cmd, "d", timestamp);
	if (!packet) {
		ErrPrint("Failed to prepare param\n");
		return DBOX_STATUS_ERROR_FAULT;
	}

	slave->state = SLAVE_REQUEST_TO_RESUME;
	return slave_rpc_async_request(slave, NULL, packet, resume_cb, NULL, 0);
}

HAPI int slave_pause(struct slave_node *slave)
{
	double timestamp;
	struct packet *packet;
	unsigned int cmd = CMD_PAUSE;

	switch (slave->state) {
	case SLAVE_REQUEST_TO_DISCONNECT:
	case SLAVE_REQUEST_TO_LAUNCH:
	case SLAVE_REQUEST_TO_TERMINATE:
	case SLAVE_TERMINATED:
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	case SLAVE_PAUSED:
	case SLAVE_REQUEST_TO_PAUSE:
		return DBOX_STATUS_ERROR_NONE;
	default:
		break;
	}

	timestamp = util_timestamp();

	packet = packet_create((const char *)&cmd, "d", timestamp);
	if (!packet) {
		ErrPrint("Failed to prepare param\n");
		return DBOX_STATUS_ERROR_FAULT;
	}

	slave->state = SLAVE_REQUEST_TO_PAUSE;
	return slave_rpc_async_request(slave, NULL, packet, pause_cb, NULL, 0);
}

HAPI const char *slave_pkgname(const struct slave_node *slave)
{
	return slave ? slave->pkgname : NULL;
}

HAPI enum slave_state slave_state(const struct slave_node *slave)
{
	return slave ? slave->state : SLAVE_ERROR;
}

HAPI const char *slave_state_string(const struct slave_node *slave)
{
	switch (slave->state) {
	case SLAVE_REQUEST_TO_DISCONNECT:
		return "RequestToDisconnect";
	case SLAVE_REQUEST_TO_LAUNCH:
		return "RequestToLaunch";
	case SLAVE_REQUEST_TO_TERMINATE:
		return "RequestToTerminate";
	case SLAVE_TERMINATED:
		return "Terminated";
	case SLAVE_REQUEST_TO_PAUSE:
		return "RequestToPause";
	case SLAVE_REQUEST_TO_RESUME:
		return "RequestToResume";
	case SLAVE_PAUSED:
		return "Paused";
	case SLAVE_RESUMED:
		return "Resumed";
	case SLAVE_ERROR:
		return "Error";
	default:
		break;
	}

	return "Unknown";
}

HAPI const void *slave_list(void)
{
	return s_info.slave_list;
}

HAPI int const slave_fault_count(const struct slave_node *slave)
{
	return slave->fault_count;
}

HAPI double const slave_ttl(const struct slave_node *slave)
{
	if (!slave->ttl_timer) {
		return 0.0f;
	}

	return ecore_timer_pending_get(slave->ttl_timer);
}

HAPI void slave_set_reactivate_instances(struct slave_node *slave, int reactivate)
{
	slave->reactivate_instances = reactivate;
}

HAPI int slave_need_to_reactivate_instances(struct slave_node *slave)
{
	return slave->reactivate_instances;
}

HAPI void slave_set_reactivation(struct slave_node *slave, int flag)
{
	slave->reactivate_slave = flag;
}

HAPI int slave_need_to_reactivate(struct slave_node *slave)
{
	return slave->reactivate_slave;
}

HAPI int slave_network(const struct slave_node *slave)
{
	return slave->network;
}

HAPI void slave_set_network(struct slave_node *slave, int network)
{
	slave->network = network;
}

HAPI int slave_deactivate_all(int reactivate, int reactivate_instances, int no_timer)
{
	Eina_List *l;
	Eina_List *n;
	struct slave_node *slave;
	int cnt = 0;

	s_info.deactivate_all_refcnt++;
	if (s_info.deactivate_all_refcnt > 1) {
		return 0;
	}
	DbgPrint("Deactivate all\n");

	EINA_LIST_FOREACH_SAFE(s_info.slave_list, l, n, slave) {
		if (slave_is_activated(slave)) {
			slave_set_reactivate_instances(slave, reactivate_instances);
			slave_set_reactivation(slave, reactivate);

			if (!slave_deactivate(slave, no_timer)) {
				s_info.slave_list = eina_list_remove(s_info.slave_list, slave);
			}
		}

		cnt++;
	}

	return cnt;
}

HAPI int slave_activate_all(void)
{
	Eina_List *l;
	struct slave_node *slave;
	int cnt = 0;

	s_info.deactivate_all_refcnt--;
	if (s_info.deactivate_all_refcnt > 0) {
		return 0;
	}
	DbgPrint("Activate all\n");

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		slave_activate(slave);
		cnt++;
	}

	return cnt;
}

HAPI void slave_set_control_option(struct slave_node *slave, int ctrl)
{
	slave->ctrl_option = ctrl;
}

HAPI int slave_control_option(struct slave_node *slave)
{
	return slave->ctrl_option;
}

HAPI int slave_set_priority(struct slave_node *slave, int priority)
{
	pid_t pid;

	if (!slave) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	pid = slave_pid(slave);
	if (pid < 0) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	if (setpriority(PRIO_PROCESS, slave_pid(slave), priority) < 0) {
		ErrPrint("setpriority: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_FAULT;
	}

	return DBOX_STATUS_ERROR_NONE;
}

HAPI int slave_priority(struct slave_node *slave)
{
	pid_t pid;
	int priority;

	if (!slave) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	pid = slave_pid(slave);
	if (pid < 0) {
		return DBOX_STATUS_ERROR_INVALID_PARAMETER;
	}

	priority = getpriority(PRIO_PROCESS, pid);
	if (priority < 0) {
		ErrPrint("getpriority: %s\n", strerror(errno));
		return DBOX_STATUS_ERROR_FAULT;
	}

	return priority;
}

/* End of a file */
