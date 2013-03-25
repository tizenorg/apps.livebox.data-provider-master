/*
 * Copyright 2013  Samsung Electronics Co., Ltd
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

/*!
 * \note
 * An instance has three states.
 * ACTIVATED, DEACTIVATED, DESTROYED
 *
 * When the master is launched and someone requiers to create this instance,
 * The master just allocate a heap for new instance.
 * We defined this as "DEACTIVATED" state.
 *
 * After master successfully allocate heap for an instance,
 * It will send a load request to a specified slave
 * (The slave will be specified when a package informaion is
 * prepared via configuration file of each livebox packages.)
 * We defined this as "REQUEST_TO_ACTIVATE" state.
 *
 * After the slave create a new instance, it will sends back
 * "created" event to the master.
 * Then the master will change the state of the instance to
 * "ACTIVATED".
 *
 * Sometimes, slaves can meet some unexpected problems then
 * it will tries to clear all problems and tries to keep them in safe env.
 * To do this, master or slave can be terminated.
 * In this case, the master has to find the fault module(crashed livebox)
 * and prevent it from loading at the slave.
 * And it will send requests for re-creating all other normal liveboxes.
 * We defined this as "REQUEST_TO_REACTIVATE".
 *
 * After slave is launched again(recovered from fault situation), it will
 * receives "re-create" event from the master, then it will create all
 * instances of requested liveboxes.
 *
 * When the master receives "created" event from the slaves,
 * It will change the instance's state to "ACTIVATED"
 * But now, the master will not send "created" event to the clients.
 *
 * Because the clients don't want to know the re-created liveboxes.
 * They just want to know about fault liveboxes to display deactivated
 * message.
 *
 * Sometimes the master can send requests to the slave to unload instances.
 * We defined this as "REQUEST_TO_DEACTIVATE".
 *
 * After the slave successfully destroy instances,
 * The master will change the instance's state to "DEACTIVATED"
 * It is same state with the first time when it is created in the master.
 *
 * Sometimes, the instances can be deleted permanently from the master and slave.
 * We called this "destorying an instance".
 * So we defined its states as "DESTROYED".
 * It can make confusing us, the "DESTROYED" means like the instance is already deleted from the
 * heap,. 
 * Yes, it is right. But the instance cannot be deleted directly.
 * Because some callbacks still reference it to complete its job.
 * So the instance needs to keep this DESTROYED state for a while
 * until all callbacks are done for their remained jobs.
 *
 * To unload the instance from the slave,
 * The master should send a request to the slave,
 * And the master should keep the instance until it receives "deleted" event from the slave.
 * We defined this state as "REQUEST_TO_DESTROY".
 * 
 * After master receives "deleted" event from the slave,
 * It will change the state of an master to "DESTROYED"
 *
 * There is one more event to change the state of an instance to "DESTROYED".
 * In case of system created livebox, it could be destroyed itself.
 * So the slave will send "deleted" event to the master directly.
 * Even if the master doesn't requests to delete it.
 *
 * In this case, the master will change the state of an instance to
 * "DESTROYED" state. but it will wait to delete it from the heap until
 * reference count of an instance reaches to ZERO.
 */

enum instance_event {
	INSTANCE_EVENT_DESTROY,
	INSTNACE_EVENT_UNKNOWN,
};

enum instance_state {
	INST_INIT = 0x0, /*!< Only keeps in the master */

	/*!
	 */
	INST_ACTIVATED, /*!< This instance is loaded to the slave */
	INST_REQUEST_TO_ACTIVATE, /*!< Sent a request to a slave to load this */
	INST_REQUEST_TO_REACTIVATE, /*!< Sent a request to a slave to load this without "created" event for clients(viewer) */

	/*!
	 */
	INST_DESTROYED, /*!< Instance is unloaded and also it requires to be deleted from the master */
	INST_REQUEST_TO_DESTROY, /*!< Sent a request to a slave, when the master receives deleted event, the master will delete this */
};

enum livebox_visible_state { /*!< Must be sync'd with livebox-viewer */
	LB_SHOW = 0x00, /*!< Livebox is showed. Default state */
	LB_HIDE = 0x01, /*!< Livebox is hide, with no update event, but keep update timer */

	LB_HIDE_WITH_PAUSE = 0x02, /*!< Livebix is hide, it needs to be paused (with freezed update timer) */

	LB_VISIBLE_ERROR = 0xFFFFFFFF, /* To enlarge the size of this enumeration type */
};

#define IS_PD	1
#define IS_LB	0

struct inst_info;
struct pkg_info;
struct script_handle;
struct client_node;

extern struct inst_info *instance_create(struct client_node *client, double timestamp, const char *pkgname, const char *content, const char *cluster, const char *category, double period, int width, int height);
extern int instance_destroy(struct inst_info *inst);

extern struct inst_info * instance_ref(struct inst_info *inst);
extern struct inst_info * instance_unref(struct inst_info *inst);

extern int instance_state_reset(struct inst_info *inst);
extern int instance_destroyed(struct inst_info *inst);

extern int instance_reactivate(struct inst_info *inst);
extern int instance_activate(struct inst_info *inst);

extern int instance_recover_state(struct inst_info *inst);
extern int instance_need_slave(struct inst_info *inst);

extern void instance_set_lb_info(struct inst_info *inst, int w, int h, double priority, const char *content, const char *title);
extern void instance_set_pd_info(struct inst_info *inst, int w, int h);

extern void instance_pd_updated(const char *pkgname, const char *id, const char *descfile);
extern void instance_lb_updated(const char *pkgname, const char *id);
extern void instance_lb_updated_by_instance(struct inst_info *inst);
extern void instance_pd_updated_by_instance(struct inst_info *inst, const char *descfile);

extern int instance_client_pd_destroyed(struct inst_info *inst, int status);
extern int instance_client_pd_created(struct inst_info *inst, int status);

extern int instance_set_pinup(struct inst_info *inst, int pinup);
extern int instance_resize(struct inst_info *inst, int w, int h);
extern int instance_set_period(struct inst_info *inst, double period);
extern int instance_clicked(struct inst_info *inst, const char *event, double timestamp, double x, double y);
extern int instance_text_signal_emit(struct inst_info *inst, const char *emission, const char *source, double sx, double sy, double ex, double ey);
extern int instance_signal_emit(struct inst_info *inst, const char *emission, const char *source, double sx, double sy, double ex, double ey, double x, double y, int down);
extern int instance_change_group(struct inst_info *inst, const char *cluster, const char *category);
extern int instance_set_visible_state(struct inst_info *inst, enum livebox_visible_state state);
extern enum livebox_visible_state instance_visible_state(struct inst_info *inst);

/*!
 * \note
 * getter
 */
extern const double const instance_timestamp(const struct inst_info *inst);
extern struct pkg_info * const instance_package(const struct inst_info *inst);
extern struct script_info * const instance_lb_script(const struct inst_info *inst);
extern struct script_info * const instance_pd_script(const struct inst_info *inst);
extern struct buffer_info * const instance_pd_buffer(const struct inst_info *inst);
extern struct buffer_info * const instance_lb_buffer(const struct inst_info *inst);
extern const char * const instance_id(const struct inst_info *inst);
extern const char * const instance_content(const struct inst_info *inst);
extern const char * const instance_category(const struct inst_info *inst);
extern const char * const instance_cluster(const struct inst_info *inst);
extern const char * const instance_title(const struct inst_info *inst);
extern const char * const instance_auto_launch(const struct inst_info *inst);
extern const int const instance_priority(const struct inst_info *inst);
extern const struct client_node * const instance_client(const struct inst_info *inst);
extern const double const instance_period(const struct inst_info *inst);
extern const int const instance_timeout(const struct inst_info *inst);
extern const int const instance_lb_width(const struct inst_info *inst);
extern const int const instance_lb_height(const struct inst_info *inst);
extern const int const instance_pd_width(const struct inst_info *inst);
extern const int const instance_pd_height(const struct inst_info *inst);
extern const enum instance_state const instance_state(const struct inst_info *inst);

/*!
 * event
 */
extern int instance_unicast_created_event(struct inst_info *inst, struct client_node *client);
extern int instance_unicast_deleted_event(struct inst_info *inst, struct client_node *client);

extern int instance_create_lb_buffer(struct inst_info *inst);
extern int instance_create_pd_buffer(struct inst_info *inst);

extern void instance_slave_set_pd_pos(struct inst_info *inst, double x, double y);
extern void instance_slave_get_pd_pos(struct inst_info *inst, double *x, double *y);

extern int instance_slave_open_pd(struct inst_info *inst, struct client_node *client);
extern int instance_slave_close_pd(struct inst_info *inst, struct client_node *client);

extern int instance_freeze_updator(struct inst_info *inst);
extern int instance_thaw_updator(struct inst_info *inst);

/*!
 * Multiple viewer
 */
extern int instance_add_client(struct inst_info *inst, struct client_node *client);
extern int instance_del_client(struct inst_info *inst, struct client_node *client);
extern int instance_has_client(struct inst_info *inst, struct client_node *client);
extern void *instance_client_list(struct inst_info *inst);

extern void instance_init(void);
extern void instance_fini(void);

extern int instance_event_callback_add(struct inst_info *inst, enum instance_event type, int (*event_cb)(struct inst_info *inst, void *data), void *data);
extern int instance_event_callback_del(struct inst_info *inst, enum instance_event type, int (*event_cb)(struct inst_info *inst, void *data));

/* End of a file */
