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
enum tcb_type {
	TCB_CLIENT_TYPE_UNDEFINED = 0x00,
	TCB_CLIENT_TYPE_APP	= 0x01,
	TCB_CLIENT_TYPE_SERVICE	= 0x02,
	TCB_CLIENT_TYPE_UNKNOWN = 0xff,
};

struct tcb;
struct service_context;

extern int tcb_fd(struct tcb *tcb);
extern struct service_context *tcb_svc_ctx(struct tcb *tcb);
extern int tcb_client_type(struct tcb *tcb);
extern int tcb_client_type_set(struct tcb *tcb, enum tcb_type type);

extern struct service_context *service_common_create(const char *addr, int (*service_thread_main)(struct tcb *tcb, struct packet *packet, void *data), void *data);
extern int service_common_destroy(struct service_context *svc_ctx);

extern int service_common_multicast_packet(struct tcb *tcb, struct packet *packet, int type);
extern int service_common_unicast_packet(struct tcb *tcb, struct packet *packet);

/* End of a file */
