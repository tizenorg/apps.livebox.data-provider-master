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

struct context_info;

extern int ctx_client_init(void);
extern int ctx_client_fini(void);
extern void ctx_update(void);
extern int ctx_enable_event_handler(struct context_info *info);
extern int ctx_disable_event_handler(struct context_info *info);

/* End of a file */
