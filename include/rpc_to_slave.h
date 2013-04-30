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

extern int rpc_send_new(struct inst_info *inst, void (*ret_cb)(const char *funcname, GVariant *result, void *data), void *data, int skip_need_to_create);
extern int rpc_send_renew(struct inst_info *inst, void (*ret_cb)(const char *funcname, GVariant *result, void *data), void *data);

extern void rpc_send_update_request(const char *pkgname, const char *cluster, const char *category);
/*!
 * \brief
 * \param[in] period if it is negative value, the data provider will use the default period
 */
extern struct inst_info *rpc_send_create_request(struct client_node *client, const char *pkgname, const char *content, const char *cluster, const char *category, double timestamp, double period);
extern void rpc_send_resume_request(void);
extern void rpc_send_pause_request(void);

/* End of a file */
