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
#include <string.h> /* strerror */
#include <errno.h> /* errno */
#include <unistd.h> /* pid_t */
#include <stdlib.h> /* free */
#include <pthread.h>
#include <malloc.h>
#include <sys/time.h>

#include <Eina.h>
#include <Ecore.h>

#include <aul.h> /* aul_launch_app */
#include <dlog.h>
#include <bundle.h>

#include <packet.h>

#include "slave_life.h"
#include "slave_rpc.h"
#include "client_life.h"
#include "fault_manager.h"
#include "ctx_client.h"
#include "debug.h"
#include "conf.h"
#include "setting.h"
#include "util.h"
#include "abi.h"
#include "xmonitor.h"

int errno;

struct slave_node {
	char *name;
	char *abi;
	char *pkgname;
	int secured;	/* Only A package(livebox) is loaded for security requirements */
	int refcnt;
	int fault_count;
	int critical_fault_count;
	enum slave_state state;

	int loaded_instance;
	int loaded_package;

	int reactivate_instances;
	int reactivate_slave;

	pid_t pid;

	Eina_List *event_activate_list;
	Eina_List *event_deactivate_list;
	Eina_List *event_delete_list;
	Eina_List *event_fault_list;
	Eina_List *event_pause_list;
	Eina_List *event_resume_list;

	Eina_List *data_list;

	Ecore_Timer *ttl_timer; /* Time to live */

	struct timeval activated_at;
};

struct event {
	struct slave_node *slave;

	int (*evt_cb)(struct slave_node *, void *);
	void *cbdata;
};

struct priv_data {
	char *tag;
	void *data;
};

static struct {
	Eina_List *slave_list;
} s_info = {
	.slave_list = NULL,
};

static Eina_Bool slave_ttl_cb(void *data)
{
	struct slave_node *slave = (struct slave_node *)data;

	/*!
	 * \note
	 * ttl_timer must has to be set to NULL before deactivate the slave
	 * It will be used for making decision of the expired TTL timer or the fault of a livebox.
	 */
	slave->ttl_timer = NULL;

	slave_set_reactivation(slave, 0);
	slave_set_reactivate_instances(slave, 1);

	slave = slave_deactivate(slave);
	DbgPrint("Slave is deactivated(%p)\n", slave);

	/*! To recover all instances state it is activated again */
	return ECORE_CALLBACK_CANCEL;
}

static inline int xmonitor_pause_cb(void *data)
{
	slave_pause(data);
	return 0;
}

static inline int xmonitor_resume_cb(void *data)
{
	slave_resume(data);
	return 0;
}

static inline struct slave_node *create_slave_node(const char *name, int is_secured, const char *abi, const char *pkgname)
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

	slave->secured = is_secured;
	slave->pid = (pid_t)-1;
	slave->state = SLAVE_TERMINATED;

	xmonitor_add_event_callback(XMONITOR_PAUSED, xmonitor_pause_cb, slave);
	xmonitor_add_event_callback(XMONITOR_RESUMED, xmonitor_resume_cb, slave);

	s_info.slave_list = eina_list_append(s_info.slave_list, slave);
	DbgPrint("slave data is created %p\n", slave);
	return slave;
}

static inline void invoke_delete_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;
	int ret;

	EINA_LIST_FOREACH_SAFE(slave->event_delete_list, l, n, event) {
		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0) {
			if (eina_list_data_find(slave->event_delete_list, event)) {
				slave->event_delete_list = eina_list_remove(slave->event_delete_list, event);
				DbgFree(event);
			}
		}
	}
}

static inline void destroy_slave_node(struct slave_node *slave)
{
	struct event *event;
	struct priv_data *priv;

	if (slave->pid != (pid_t)-1) {
		ErrPrint("Slave is not deactivated\n");
		return;
	}

	DbgPrint("Slave data is destroyed %p\n", slave);

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

	if (slave->ttl_timer)
		ecore_timer_del(slave->ttl_timer);

	DbgFree(slave->abi);
	DbgFree(slave->name);
	DbgFree(slave->pkgname);
	DbgFree(slave);
	return;
}

static inline struct slave_node *find_slave(const char *name)
{
	struct slave_node *slave;
	Eina_List *l;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (!strcmp(slave->name, name))
			return slave;
	}
	
	return NULL;
}

HAPI int slave_expired_ttl(struct slave_node *slave)
{
	if (!slave)
		return 0;

	if (!slave->secured)
		return 0;

	return !!slave->ttl_timer;
}

HAPI struct slave_node *slave_ref(struct slave_node *slave)
{
	if (!slave)
		return NULL;

	slave->refcnt++;
	return slave;
}

HAPI struct slave_node *slave_unref(struct slave_node *slave)
{
	if (!slave)
		return NULL;

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

HAPI struct slave_node *slave_create(const char *name, int is_secured, const char *abi, const char *pkgname)
{
	struct slave_node *slave;

	slave = find_slave(name);
	if (slave) {
		if (slave->secured != is_secured)
			ErrPrint("Exists slave and creating slave's security flag is not matched\n");
		return slave;
	}

	slave = create_slave_node(name, is_secured, abi, pkgname);
	if (!slave)
		return NULL;

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

static inline void invoke_fault_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;
	int ret;

	EINA_LIST_FOREACH_SAFE(slave->event_fault_list, l, n, event) {
		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0) {
			if (eina_list_data_find(slave->event_fault_list, event)) {
				slave->event_fault_list = eina_list_remove(slave->event_fault_list, event);
				DbgFree(event);
			}
		}
	}
}

static inline void invoke_activate_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;
	int ret;

	EINA_LIST_FOREACH_SAFE(slave->event_activate_list, l, n, event) {
		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0) {
			if (eina_list_data_find(slave->event_activate_list, event)) {
				slave->event_activate_list = eina_list_remove(slave->event_activate_list, event);
				DbgFree(event);
			}
		}
	}
}

HAPI int slave_activate(struct slave_node *slave)
{
	bundle *param;

	/*!
	 * \note
	 * This check code can replace the slave->state check code
	 * If the slave data has the PID, it means, it is activated
	 * Even if it is in the termiating sequence, it will have the PID
	 * before terminated at last.
	 * So we can use this simple code for checking the slave's last state.
	 * about it is alive? or not.
	 */
	if (slave->pid != (pid_t)-1)
		return -EALREADY;

	param = bundle_create();
	if (!param) {
		ErrPrint("Failed to create a bundle\n");
		return -EFAULT;
	}

	bundle_add(param, BUNDLE_SLAVE_NAME, slave->name);
	bundle_add(param, BUNDLE_SLAVE_SECURED, slave->secured ? "true" : "false");
	bundle_add(param, BUNDLE_SLAVE_ABI, slave->abi);
	DbgPrint("Launch the slave package: %s\n", slave->pkgname);
	slave->pid = (pid_t)aul_launch_app(slave->pkgname, param);
	bundle_free(param);

	if (slave->pid < 0) {
		ErrPrint("Failed to launch a new slave %s (%d)\n", slave->name, slave->pid);
		slave->pid = (pid_t)-1;
		return -EFAULT;
	}
	DbgPrint("Slave launched %d for %s\n", slave->pid, slave->name);

	slave->state = SLAVE_REQUEST_TO_LAUNCH;
	/*!
	 * \note
	 * Increase the refcnt of a slave,
	 * To prevent from making an orphan(slave).
	 */
	slave_ref(slave);

	return 0;
}

HAPI int slave_give_more_ttl(struct slave_node *slave)
{
	double delay;

	if (!slave->secured || !slave->ttl_timer)
		return -EINVAL;

	delay = SLAVE_TTL - ecore_timer_pending_get(slave->ttl_timer);
	ecore_timer_delay(slave->ttl_timer, delay);
	return 0;
}

HAPI int slave_freeze_ttl(struct slave_node *slave)
{
	if (!slave->secured || !slave->ttl_timer)
		return -EINVAL;

	ecore_timer_freeze(slave->ttl_timer);
	return 0;
}

HAPI int slave_thaw_ttl(struct slave_node *slave)
{
	double delay;

	if (!slave->secured || !slave->ttl_timer)
		return -EINVAL;

	ecore_timer_thaw(slave->ttl_timer);

	delay = SLAVE_TTL - ecore_timer_pending_get(slave->ttl_timer);
	ecore_timer_delay(slave->ttl_timer, delay);
	return 0;
}

HAPI int slave_activated(struct slave_node *slave)
{
	slave->state = SLAVE_RESUMED;

	if (xmonitor_is_paused())
		slave_pause(slave);

	if (slave->secured == 1) {
		DbgPrint("Slave deactivation timer is added (%s - %lf)\n", slave->name, SLAVE_TTL);
		slave->ttl_timer = ecore_timer_add(SLAVE_TTL, slave_ttl_cb, slave);
		if (!slave->ttl_timer)
			ErrPrint("Failed to create a TTL timer\n");
	}

	invoke_activate_cb(slave);

	slave_set_reactivation(slave, 0);
	slave_set_reactivate_instances(slave, 0);

	if (gettimeofday(&slave->activated_at, NULL) < 0)
		ErrPrint("Failed to get time of day: %s\n", strerror(errno));
	return 0;
}

static inline int invoke_deactivate_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;
	int ret;
	int reactivate = 0;

	EINA_LIST_FOREACH_SAFE(slave->event_deactivate_list, l, n, event) {
		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0) {
			if (eina_list_data_find(slave->event_deactivate_list, event)) {
				slave->event_deactivate_list = eina_list_remove(slave->event_deactivate_list, event);
				DbgFree(event);
			}
		} else if (ret == SLAVE_NEED_TO_REACTIVATE) {
			reactivate++;
		}
	}

	return reactivate;
}

HAPI struct slave_node *slave_deactivate(struct slave_node *slave)
{
	int ret;

	if (!slave_is_activated(slave)) {
		ErrPrint("Slave is already deactivated\n");
		if (slave_loaded_instance(slave) == 0) {
			/*!
			 * \note
			 * If a slave has no more instances,
			 * Destroy it
			 */
			slave = slave_unref(slave);
		}
		return slave;
	}

	DbgPrint("Deactivate a slave: %d\n", slave->pid);
	/*!
	 * \todo
	 * check the return value of the aul_terminate_pid
	 */
	slave->state = SLAVE_REQUEST_TO_TERMINATE;

	DbgPrint("Terminate PID: %d\n", slave->pid);
	if (slave->pid > 0) {
		ret = aul_terminate_pid(slave->pid);
		if (ret < 0) {
			ErrPrint("Terminate failed. pid %d (%d)\n", slave->pid, ret);
			slave = slave_deactivated(slave);
		}
	}

	return slave;
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

	reactivate = invoke_deactivate_cb(slave);

	slave = slave_unref(slave);
	if (!slave) {
		DbgPrint("SLAVE object is destroyed\n");
		return slave;
	}

	if (reactivate && slave_need_to_reactivate(slave)) {
		int ret;

		DbgPrint("Need to reactivate a slave\n");
		ret = slave_activate(slave);
		if (ret < 0 && ret != -EALREADY)
			ErrPrint("Failed to reactivate a slave\n");
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
	struct timeval faulted_at;
	int reactivate = 1;
	int reactivate_instances = 1;

	if (!slave_is_activated(slave)) {
		DbgPrint("Deactivating in progress\n");
		if (slave_loaded_instance(slave) == 0)
			slave = slave_unref(slave);

		return slave;
	}

	slave->fault_count++;

	(void)fault_check_pkgs(slave);

	if (slave->pid > 0) {
		DbgPrint("Try to terminate PID: %d\n", slave->pid);
		ret = aul_terminate_pid(slave->pid);
		if (ret < 0) {
			ErrPrint("Terminate failed, pid %d\n", slave->pid);
		}
	}

	if (gettimeofday(&faulted_at, NULL) == 0) {
		struct timeval rtv;

		timersub(&faulted_at, &slave->activated_at, &rtv);
		if (rtv.tv_sec < MINIMUM_REACTIVATION_TIME) {
			slave->critical_fault_count++;
			if (!slave_loaded_instance(slave) || slave->critical_fault_count >= SLAVE_MAX_LOAD) {
				ErrPrint("Reactivation time is too fast and frequently occurred - Stop to auto reactivation\n");
				reactivate = 0;
				reactivate_instances = 0;
				slave->critical_fault_count = 0;
				/*!
				 * \note
				 * Fault callback can access the slave information.
				 */
				invoke_fault_cb(slave);
			}
		} else {
			slave->critical_fault_count = 0;
		}
	} else {
		ErrPrint("Failed to get time of day: %s\n", strerror(errno));
	}

	slave_set_reactivation(slave, reactivate);
	slave_set_reactivate_instances(slave, reactivate_instances);

	slave = slave_deactivated(slave);
	return slave;
}

HAPI const int const slave_is_activated(struct slave_node *slave)
{
	switch (slave->state) {
	case SLAVE_REQUEST_TO_LAUNCH:
	case SLAVE_REQUEST_TO_TERMINATE:
	case SLAVE_TERMINATED:
		return 0;
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
		return -ENOMEM;
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
		return -EINVAL;
	}

	return 0;
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
				slave->event_deactivate_list = eina_list_remove(slave->event_deactivate_list, ev);
				DbgFree(ev);
				return 0;
			}
		}
		break;
	case SLAVE_EVENT_DELETE:
		EINA_LIST_FOREACH_SAFE(slave->event_delete_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				slave->event_delete_list = eina_list_remove(slave->event_delete_list, ev);
				DbgFree(ev);
				return 0;
			}
		}
		break;
	case SLAVE_EVENT_ACTIVATE:
		EINA_LIST_FOREACH_SAFE(slave->event_activate_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				slave->event_activate_list = eina_list_remove(slave->event_activate_list, ev);
				DbgFree(ev);
				return 0;
			}
		}
		break;
	case SLAVE_EVENT_PAUSE:
		EINA_LIST_FOREACH_SAFE(slave->event_pause_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				slave->event_pause_list = eina_list_remove(slave->event_pause_list, ev);
				DbgFree(ev);
				return 0;
			}
		}
		break;
	case SLAVE_EVENT_RESUME:
		EINA_LIST_FOREACH_SAFE(slave->event_resume_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				slave->event_resume_list = eina_list_remove(slave->event_resume_list, ev);
				DbgFree(ev);
				return 0;
			}
		}
		break;
	case SLAVE_EVENT_FAULT:
		EINA_LIST_FOREACH_SAFE(slave->event_fault_list, l, n, ev) {
			if (ev->evt_cb == cb && ev->cbdata == data) {
				slave->event_fault_list = eina_list_remove(slave->event_fault_list, ev);
				DbgFree(ev);
				return 0;
			}
		}
		break;
	default:
		return -EINVAL;
	}

	return -ENOENT;
}

HAPI int slave_set_data(struct slave_node *slave, const char *tag, void *data)
{
	struct priv_data *priv;

	priv = calloc(1, sizeof(*priv));
	if (!priv) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return -ENOMEM;
	}

	priv->tag = strdup(tag);
	if (!priv->tag) {
		ErrPrint("Heap: %s\n", strerror(errno));
		DbgFree(priv);
		return -ENOMEM;
	}

	priv->data = data;
	slave->data_list = eina_list_append(slave->data_list, priv);
	return 0;
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
		if (!strcmp(priv->tag, tag))
			return priv->data;
	}

	return NULL;
}

HAPI struct slave_node *slave_find_by_pid(pid_t pid)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave->pid == pid)
			return slave;
	}

	return NULL;
}

HAPI struct slave_node *slave_find_by_name(const char *name)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (!strcmp(slave->name, name))
			return slave;
	}

	return NULL;
}

HAPI struct slave_node *slave_find_available(const char *abi, int secured)
{
	Eina_List *l;
	struct slave_node *slave;

	EINA_LIST_FOREACH(s_info.slave_list, l, slave) {
		if (slave->secured != secured)
			continue;

		if (slave->state == SLAVE_REQUEST_TO_TERMINATE && slave->loaded_instance == 0) {
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

		if (strcasecmp(slave->abi, abi))
			continue;

		if (slave->secured) {
			DbgPrint("Found secured slave - has no instances (%s)\n", slave_name(slave));
			if (slave->loaded_package == 0)
				return slave;
		} else {
			DbgPrint("slave[%s] %d\n", slave_name(slave), slave->loaded_package);
			if (!strcasecmp(abi, DEFAULT_ABI)) {
				if (slave->loaded_package < SLAVE_MAX_LOAD)
					return slave;
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
		if (!strcmp(slave->pkgname, pkgname)) {
			if (slave->pid == (pid_t)-1) {
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
		if (slave_rpc_handle(slave) == handle)
			return slave;
	}

	/* Not found */
	return NULL;
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
	DbgPrint("Instance: (%d)%d\n", slave->pid, slave->loaded_instance);
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
	DbgPrint("Instance: (%d)%d\n", slave->pid, slave->loaded_instance);
	if (slave->loaded_instance == 0 && slave_is_activated(slave)) {
		slave_set_reactivation(slave, 0);
		slave_set_reactivate_instances(slave, 0);

		slave = slave_deactivate(slave);
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
	if (!slave)
		return -EINVAL;

	DbgPrint("Slave PID is updated to %d from %d\n", pid, slave->pid);

	slave->pid = pid;
	return 0;
}

static inline void invoke_resumed_cb(struct slave_node *slave)
{
	Eina_List *l;
	Eina_List *n;
	struct event *event;
	int ret;

	EINA_LIST_FOREACH_SAFE(slave->event_resume_list, l, n, event) {
		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0) {
			if (eina_list_data_find(slave->event_resume_list, event)) {
				slave->event_resume_list = eina_list_remove(slave->event_resume_list, event);
				DbgFree(event);
			}
		}
	}
}

static void resume_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	int ret;

	if (slave->state == SLAVE_REQUEST_TO_TERMINATE) {
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
	int ret;

	EINA_LIST_FOREACH_SAFE(slave->event_pause_list, l, n, event) {
		ret = event->evt_cb(event->slave, event->cbdata);
		if (ret < 0) {
			if (eina_list_data_find(slave->event_pause_list, event)) {
				slave->event_pause_list = eina_list_remove(slave->event_pause_list, event);
				DbgFree(event);
			}
		}
	}
}

static void pause_cb(struct slave_node *slave, const struct packet *packet, void *data)
{
	int ret;

	if (slave->state == SLAVE_REQUEST_TO_TERMINATE) {
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

	switch (slave->state) {
	case SLAVE_REQUEST_TO_LAUNCH:
	case SLAVE_REQUEST_TO_TERMINATE:
	case SLAVE_TERMINATED:
		return -EINVAL;
	case SLAVE_RESUMED:
	case SLAVE_REQUEST_TO_RESUME:
		return 0;
	default:
		break;
	}

	timestamp = util_timestamp();

	packet = packet_create("resume", "d", timestamp);
	if (!packet) {
		ErrPrint("Failed to prepare param\n");
		return -EFAULT;
	}

	slave->state = SLAVE_REQUEST_TO_RESUME;
	return slave_rpc_async_request(slave, NULL, packet, resume_cb, NULL, 0);
}

HAPI int slave_pause(struct slave_node *slave)
{
	double timestamp;
	struct packet *packet;

	switch (slave->state) {
	case SLAVE_REQUEST_TO_LAUNCH:
	case SLAVE_REQUEST_TO_TERMINATE:
	case SLAVE_TERMINATED:
		return -EINVAL;
	case SLAVE_PAUSED:
	case SLAVE_REQUEST_TO_PAUSE:
		return 0;
	default:
		break;
	}

	timestamp = util_timestamp();

	packet = packet_create("pause", "d", timestamp);
	if (!packet) {
		ErrPrint("Failed to prepare param\n");
		return -EFAULT;
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
	case SLAVE_REQUEST_TO_LAUNCH:
		return "Request to launch";
	case SLAVE_REQUEST_TO_TERMINATE:
		return "Request to terminate";
	case SLAVE_TERMINATED:
		return "Terminated";
	case SLAVE_REQUEST_TO_PAUSE:
		return "Request to pause";
	case SLAVE_REQUEST_TO_RESUME:
		return "Request to resume";
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
	if (!slave->ttl_timer)
		return 0.0f;

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

/* End of a file */
