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

struct conf {
	int debug_mode;
	int slave_max_load;
};

extern struct conf g_conf;

#define DELAY_TIME 0.0000001f
#define HAPI __attribute__((visibility("hidden")))

#if !defined(VCONFKEY_MASTER_STARTED)
#define VCONFKEY_MASTER_STARTED	"memory/data-provider-master/started"
#endif

#if !defined(VCONFKEY_MASTER_RESTART_COUNT)
#define VCONFKEY_MASTER_RESTART_COUNT	"memory/private/data-provider-master/restart_count"
#endif

#define CR 13
#define LF 10

/* End of a file */
