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

struct script_info;
struct fb_info;

extern struct script_info *script_handler_create(struct inst_info *inst, const char *file, const char *group, int w, int h);
extern int script_handler_destroy(struct script_info *info);
extern struct fb_info *script_handler_fb(struct script_info *info);
extern void *script_handler_evas(struct script_info *info);
extern int script_handler_parse_desc(struct inst_info *inst, const char *descfile, int is_pd);
extern int script_handler_unload(struct script_info *info, int is_pd);
extern int script_handler_load(struct script_info *info, int is_pd);
extern int script_handler_is_loaded(struct script_info *info);
extern int script_handler_feed_event(struct script_info *info, int event, double timestamp);

extern int script_init(void);
extern int script_fini(void);

extern int script_signal_emit(Evas *e, const char *part, const char *signal, double sx, double sy, double ex, double ey);
extern int script_handler_update_pointer(struct script_info *inst, int x, int y, int down);
extern int script_handler_resize(struct script_info *info, int w, int h);

/* End of a file */
