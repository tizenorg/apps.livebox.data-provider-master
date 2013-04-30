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

/*!
 */
extern int client_rpc_async_request(struct client_node *client, struct packet *packet);
extern int client_rpc_handle(struct client_node *client);

/*!
 */
extern int client_rpc_init(struct client_node *client, int handle);
extern int client_rpc_fini(struct client_node *client);

/* End of a file */
