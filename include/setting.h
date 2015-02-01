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

extern int setting_is_lcd_off(void);
extern int setting_init(void);
extern int setting_fini(void);

enum oom_event_type {
    OOM_TYPE_NORMAL = 0x00,
    OOM_TYPE_LOW = 0x01,
    OOM_ERROR = 0xFF
};

extern int setting_add_oom_event_callback(int (*handler)(enum oom_event_type type, void *data), void *data);
extern int setting_del_oom_event_callback(int (*handler)(enum oom_event_type type, void *data), void *data);
extern enum oom_event_type setting_oom_level(void);

/* End of a file */
