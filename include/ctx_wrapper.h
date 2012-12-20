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

extern void ctx_wrapper_enable(void);
extern void ctx_wrapper_disable(void);
extern void *ctx_wrapper_register_callback(struct context_item *item, int (*cb)(struct context_item *item, void *user_data), void *user_data);
extern void *ctx_wrapper_unregister_callback(void *_cbfunc);

/* End of a file */
