/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.0 (the "License");
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

/*!
 * Managing the reference counter of a slave
 */

struct slave_node;

enum slave_event {
	SLAVE_EVENT_ACTIVATE,
	SLAVE_EVENT_DEACTIVATE, /* deactivate callback, can return REACTIVATE, DEFAULT */
	SLAVE_EVENT_DELETE,
	SLAVE_EVENT_FAULT, /* Critical fault */

	SLAVE_EVENT_PAUSE,
	SLAVE_EVENT_RESUME,

	SLAVE_NEED_TO_REACTIVATE,
};

enum slave_state {
	/*!
	 * Launch the slave but not yet receives "hello" packet
	 */
	SLAVE_REQUEST_TO_LAUNCH,

	/*!
	 * \note
	 * Terminate the slave but not yet receives dead signal
	 */
	SLAVE_REQUEST_TO_TERMINATE,

	/*!
	 * \note
	 * No slave process exists, just slave object created
	 */
	SLAVE_TERMINATED,

	/*!
	 * \note
	 * State change request is sent,
	 */
	SLAVE_REQUEST_TO_PAUSE,
	SLAVE_REQUEST_TO_RESUME,

	/*!
	 * \note
	 * SLAVE_ACTIVATED = { SLAVE_PAUSED, SLAVE_RESUMED }
	 */
	SLAVE_PAUSED,
	SLAVE_RESUMED,

	SLAVE_ERROR = 0xFF, /* Explicitly define the size of this enum type */
};

extern struct slave_node *slave_ref(struct slave_node *slave);
extern struct slave_node *slave_unref(struct slave_node *slave);
extern const int const slave_refcnt(struct slave_node *slave);

/*!
 * \brief
 * Create a new slave object or destroy it
 *
 * \param[in] name
 * \param[in] is_secured
 * \param[in] abi
 * \param[in] pkgname
 * \param[in] period
 * \return slave_node
 */
extern struct slave_node *slave_create(const char *name, int is_secured, const char *abi, const char *pkgname, int network);

/*!
 * \brief
 * \param[in] slave
 * \return void
 */
extern void slave_destroy(struct slave_node *slave);

/*!
 * \brief
 * Launch or terminate a slave
 * \param[in] slave
 * \return int
 */
extern int slave_activate(struct slave_node *slave);

/*!
 * \brief After this function call, the slave object can be deleted
 * \param[in] slave
 */
extern struct slave_node *slave_deactivate(struct slave_node *slave) __attribute__((warn_unused_result));

/*!
 * To check the slave's activation state
 */
extern const int const slave_is_activated(struct slave_node *slave);
extern int slave_activated(struct slave_node *slave);

extern int slave_give_more_ttl(struct slave_node *slave);
extern int slave_freeze_ttl(struct slave_node *slave);
extern int slave_thaw_ttl(struct slave_node *slave);
extern int slave_expired_ttl(struct slave_node *slave);

/*!
 * \NOTE
 * To mangage the unexpected termination of a slave
 * After this function call, the slave object can be deleted
 */
extern struct slave_node *slave_deactivated_by_fault(struct slave_node *slave) __attribute__((warn_unused_result));

/*!
 * \NOTE
 * After this function, the slave object can be deleted
 */
extern struct slave_node *slave_deactivated(struct slave_node *slave) __attribute__((warn_unused_result));

extern int slave_event_callback_add(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data);
extern int slave_event_callback_del(struct slave_node *slave, enum slave_event event, int (*cb)(struct slave_node *, void *), void *data);

extern int slave_set_data(struct slave_node *slave, const char *tag, void *data);
extern void *slave_del_data(struct slave_node *slave, const char *tag);
extern void *slave_data(struct slave_node *slave, const char *tag);

extern struct slave_node *slave_find_by_pid(pid_t pid);
extern struct slave_node *slave_find_by_name(const char *name);
extern struct slave_node *slave_find_by_pkgname(const char *pkgname);
extern struct slave_node *slave_find_by_rpc_handle(int handle);

extern void slave_dead_handler(struct slave_node *slave);
extern const int const slave_is_secured(const struct slave_node *slave);
extern const char * const slave_name(const struct slave_node *slave);
extern const pid_t const slave_pid(const struct slave_node *slave);
extern const char * const slave_abi(const struct slave_node *slave);
extern int slave_set_pid(struct slave_node *slave, pid_t pid);

/*!
 * \note
 * Used for making decision of destroying a slave or not
 * Used for balancing load of the slave.
 */
extern void slave_load_package(struct slave_node *slave);
extern void slave_unload_package(struct slave_node *slave);
extern int const slave_loaded_package(struct slave_node *slave);
extern struct slave_node *slave_find_available(const char *abi, int secured, int network);

extern double const slave_ttl(const struct slave_node *slave);

/*!
 * \note
 * Used for making decision of activating a slave or not
 */
extern void slave_load_instance(struct slave_node *slave);

/*!
 * \NOTE
 * After this function call, the slave object can be deleted.
 */
extern struct slave_node *slave_unload_instance(struct slave_node *slave) __attribute__((warn_unused_result));

extern int const slave_loaded_instance(struct slave_node *slave);

extern int slave_resume(struct slave_node *slave);
extern int slave_pause(struct slave_node *slave);

extern const char *slave_pkgname(const struct slave_node *slave);
extern enum slave_state slave_state(const struct slave_node *slave);
extern const char *slave_state_string(const struct slave_node *slave);

extern const void *slave_list(void);
extern int const slave_fault_count(const struct slave_node *slave);

extern int slave_need_to_reactivate_instances(struct slave_node *slave);
extern void slave_set_reactivate_instances(struct slave_node *slave, int reactivate);

extern void slave_set_reactivation(struct slave_node *slave, int flag);
extern int slave_need_to_reactivate(struct slave_node *slave);

extern int slave_network(const struct slave_node *slave);
extern void slave_set_network(struct slave_node *slave, int network);

/* End of a file */
