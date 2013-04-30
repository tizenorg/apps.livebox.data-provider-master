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

struct parser;

extern struct parser *parser_load(const char *filename);
extern int parser_unload(struct parser *handle);
extern double parser_period(struct parser *handle);
extern int parser_network(struct parser *handle);
extern int parser_timeout(struct parser *handle);
extern const char *parser_auto_launch(struct parser *handle);
extern unsigned int parser_size(struct parser *handle);
extern void parser_get_pdsize(struct parser *handle, unsigned int *width, unsigned int *height);
extern const char *parser_group_str(struct parser *handle);
extern int parser_secured(struct parser *handle);
extern int parser_pinup(struct parser *handler);

extern const char *parser_lb_path(struct parser *handle);
extern const char *parser_lb_group(struct parser *handle);
extern const char *parser_pd_path(struct parser *handle);
extern const char *parser_pd_group(struct parser *handle);

extern const char *parser_abi(struct parser *handle);

extern int parser_text_pd(struct parser *handle);
extern int parser_text_lb(struct parser *handle);
extern int parser_buffer_lb(struct parser *handle);
extern int parser_buffer_pd(struct parser *handle);

extern const char *parser_script(struct parser *handle);

/* End of a file */
