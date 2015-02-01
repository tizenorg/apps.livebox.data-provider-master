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

#include <stdio.h>
#include <stdlib.h> /* strtod */
#include <errno.h>
#include <ctype.h> /* isspace */

#include <Eina.h>
#include <dlog.h>

#include <dynamicbox_service.h>
#include <dynamicbox_errno.h>
#include <dynamicbox_conf.h>

#include "util.h"
#include "debug.h"
#include "conf.h"
#include "parser.h"

static Eina_List *s_list;
int errno;

#if __x86__64__
#define RETURN_TYPE long long
#else
#define RETURN_TYPE int
#endif

struct parser {
	char *filename;
	double period;
	int timeout;
	int network;
	char *auto_launch;
	unsigned int size;
	unsigned int gbar_width;
	unsigned int gbar_height;
	char *group;
	int secured;

	char *gbar_path;
	char *gbar_group;

	char *dbox_path;
	char *dbox_group;
	int pinup;
	int text_gbar;
	int text_lb;
	int buffer_gbar;
	int buffer_lb;

	char *abi;

	char *script;
};

HAPI double parser_period(struct parser *handle)
{
	return handle->period;
}

HAPI int parser_network(struct parser *handle)
{
	return handle->network;
}

HAPI int parser_timeout(struct parser *handle)
{
	return handle->timeout;
}

HAPI const char *parser_auto_launch(struct parser *handle)
{
	return handle->auto_launch;
}

HAPI const char *parser_script(struct parser *handle)
{
	return handle->script;
}

HAPI const char *parser_abi(struct parser *handle)
{
	return handle->abi;
}

HAPI unsigned int parser_size(struct parser *handle)
{
	return handle->size;
}

HAPI const char *parser_dbox_path(struct parser *handle)
{
	return handle->dbox_path;
}

HAPI const char *parser_dbox_group(struct parser *handle)
{
	return handle->dbox_group;
}

HAPI const char *parser_gbar_path(struct parser *handle)
{
	return handle->gbar_path;
}

HAPI const char *parser_gbar_group(struct parser *handle)
{
	return handle->gbar_group;
}

HAPI const char *parser_group_str(struct parser *handle)
{
	return handle->group;
}

HAPI int parser_secured(struct parser *handle)
{
	return handle->secured;
}

HAPI void parser_get_gbar_size(struct parser *handle, unsigned int *width, unsigned int *height)
{
	*width = handle->gbar_width;
	*height = handle->gbar_height;
}

HAPI int parser_pinup(struct parser *handle)
{
	return handle->pinup;
}

HAPI int parser_text_dbox(struct parser *handle)
{
	return handle->text_lb;
}

HAPI int parser_text_gbar(struct parser *handle)
{
	return handle->text_gbar;
}

HAPI int parser_buffer_dbox(struct parser *handle)
{
	return handle->buffer_lb;
}

HAPI int parser_buffer_gbar(struct parser *handle)
{
	return handle->buffer_gbar;
}

HAPI RETURN_TYPE parser_find(const char *pkgname)
{
	Eina_List *l;
	struct parser *item;
	char *filename;
	int len;
	int ret;

	len = strlen(pkgname) * 2 + strlen(DYNAMICBOX_CONF_CONF_PATH);

	filename = malloc(len);
	if (!filename) {
		return (RETURN_TYPE)0;
	}

	ret = snprintf(filename, len, DYNAMICBOX_CONF_CONF_PATH, pkgname, pkgname);
	if (ret < 0) {
		DbgFree(filename);
		return (RETURN_TYPE)0;
	}

	DbgPrint("Conf file %s for package %s\n", filename, pkgname);

	EINA_LIST_FOREACH(s_list, l, item) {
		if (!strcmp(item->filename, filename)) {
			DbgFree(filename);
			return (RETURN_TYPE)item;
		}
	}

	DbgFree(filename);
	return (RETURN_TYPE)0;
}

static inline int parse_size(const char *buffer, unsigned int *size)
{
	register int i;
	int w;
	int h;

	enum {
		START,
		WIDTH,
		DELIM,
		HEIGHT,
		ERROR,
		STOP,
		END
	} state;

	*size = 0;
	state = START;
	i = 0;
	w = 0;
	h = 0;

	while (state != ERROR && state != END) {
		switch (state) {
		case START:
			switch (buffer[i]) {
			case '1'...'9':
				state = WIDTH;
				break;
			case ' ':
			case '\t':
			case ';':
				i++;
				break;
			case '\0':
				state = END;
				break;
			default:
				state = ERROR;
				break;
			}
			break;
		case WIDTH:
			switch (buffer[i]) {
			case '0'...'9':
				w = (w * 10) + (buffer[i] - '0');
				i++;
				break;
			case 'x':
				state = DELIM;
				i++;
				break;
			default:
				state = ERROR;
				break;
			}

			break;
		case DELIM:
			switch (buffer[i]) {
			case '1'...'9':
				state = HEIGHT;
				break;
			case ' ':
			case '\t':
				i++;
				break;
			default:
				state = ERROR;
				break;
			}
			break;
		case HEIGHT:
			switch (buffer[i]) {
			case '0'...'9':
				h = (h * 10) + (buffer[i] - '0');
				i++;
				break;
			case ';':
			case '\0':
				state = STOP;
				break;
			default:
				state = ERROR;
				break;
			}
			break;
		case STOP:
			if (w == 1 && h == 1) {
				*size |= DBOX_SIZE_TYPE_1x1;
			} else if (w == 2 && h == 1) {
				*size |= DBOX_SIZE_TYPE_2x1;
			} else if (w == 2 && h == 2) {
				*size |= DBOX_SIZE_TYPE_2x2;
			} else if (w == 4 && h == 1) {
				*size |= DBOX_SIZE_TYPE_4x1;
			} else if (w == 4 && h == 2) {
				*size |= DBOX_SIZE_TYPE_4x2;
			} else if (w == 4 && h == 3) {
				*size |= DBOX_SIZE_TYPE_4x3;
			} else if (w == 4 && h == 4) {
				*size |= DBOX_SIZE_TYPE_4x4;
			} else if (w == 21 && h == 21) {
				*size |= DBOX_SIZE_TYPE_EASY_1x1;
			} else if (w == 23 && h == 21) {
				*size |= DBOX_SIZE_TYPE_EASY_3x1;
			} else if (w == 23 && h == 23) {
				*size |= DBOX_SIZE_TYPE_EASY_3x3;
			} else {
				ErrPrint("Invalid size type: %dx%d\n", w, h);
			}

			if (buffer[i] == ';') {
				state = START;
			} else if (buffer[i] == '\0') {
				state = END;
			}

			w = 0;
			h = 0;
			break;
		default:
			return -1;
		}
	}

	return *size ? 0 : -1;
}

/*!
 * \note
 * This will change the value of "buffer"
 */
static inline const char *rtrim(char *buffer)
{
	int len;

	len = strlen(buffer);
	while (len > 0 && isspace(buffer[len - 1])) {
		len--;
	}

	if (len <= 0) {
		return NULL;
	}

	buffer[len] = '\0';

	return buffer;
}

/*!
 * \note
 * This will change the value of "buffer"
 */
static inline char *dup_rtrim(char *buffer)
{
	char *ret;

	if (!rtrim(buffer)) {
		return NULL;
	}

	ret = strdup(buffer);
	if (!ret) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	return ret;
}

static void period_handler(struct parser *item, char *buffer)
{
	char *tmp = NULL;

	if (!rtrim(buffer)) {
		return;
	}

	item->period = strtod(buffer, &tmp);
}

static void timeout_handler(struct parser *item, char *buffer)
{
	if (!rtrim(buffer)) {
		return;
	}

	item->timeout = atoi(buffer);
}

static void network_handler(struct parser *item, char *buffer)
{
	if (!rtrim(buffer)) {
		return;
	}

	item->network = !!atoi(buffer);
}

static void auto_launch_handler(struct parser *item, char *buffer)
{
	if (!rtrim(buffer)) {
		return;
	}

	item->auto_launch = strdup(buffer);
	if (!item->auto_launch) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return;
	}
}

static void size_handler(struct parser *item, char *buffer)
{
	if (parse_size(buffer, &item->size) == -1) {
		ErrPrint("Failed to get size\n");
		item->size = 0x00000001;
	}
}

static void gbar_size_handler(struct parser *item, char *buffer)
{
	if (sscanf(buffer, "%ux%u", &item->gbar_width, &item->gbar_height) != 2) {
		ErrPrint("parse pd size\n");
	}
}

static void text_dbox_handler(struct parser *item, char *buffer)
{
	if (!rtrim(buffer)) {
		return;
	}

	item->text_lb = !!atoi(buffer);
}

static void abi_handler(struct parser *item, char *buffer)
{
	item->abi = dup_rtrim(buffer);
}

static void script_handler(struct parser *item, char *buffer)
{
	item->script = dup_rtrim(buffer);
}

static void buffer_gbar_handler(struct parser *item, char *buffer)
{
	if (!rtrim(buffer)) {
		return;
	}

	item->buffer_gbar = !!atoi(buffer);
}

static void buffer_dbox_handler(struct parser *item, char *buffer)
{
	if (!rtrim(buffer)) {
		return;
	}

	item->buffer_lb = !!atoi(buffer);
}

static void text_gbar_handler(struct parser *item, char *buffer)
{
	if (!rtrim(buffer)) {
		return;
	}

	item->text_gbar = !!atoi(buffer);
}

static void pinup_handler(struct parser *item, char *buffer)
{
	if (!rtrim(buffer)) {
		return;
	}

	item->pinup = !!atoi(buffer);
}

static void dbox_path_handler(struct parser *item, char *buffer)
{
	if (item->dbox_path) {
		DbgFree(item->dbox_path);
	}

	item->dbox_path = dup_rtrim(buffer);
	if (!item->dbox_path) {
		ErrPrint("Error: %s\n", strerror(errno));
	}
}

static void group_handler(struct parser *item, char *buffer)
{
	if (item->group) {
		DbgFree(item->group);
	}

	item->group = dup_rtrim(buffer);
	if (!item->group) {
		ErrPrint("Error: %s\n", strerror(errno));
	}
}

static void secured_handler(struct parser *item, char *buffer)
{
	if (!rtrim(buffer)) {
		return;
	}

	item->secured = !!atoi(buffer);
}

static void dbox_group_handler(struct parser *item, char *buffer)
{
	if (item->dbox_group) {
		DbgFree(item->dbox_group);
	}

	item->dbox_group = dup_rtrim(buffer);
	if (!item->dbox_group) {
		ErrPrint("Error: %s\n", strerror(errno));
	}
}

static void gbar_path_handler(struct parser *item, char *buffer)
{
	if (item->gbar_path) {
		DbgFree(item->gbar_path);
	}

	item->gbar_path = dup_rtrim(buffer);
	if (!item->gbar_path) {
		ErrPrint("Error: %s\n", strerror(errno));
	}
}

static void gbar_group_handler(struct parser *item, char *buffer)
{
	if (item->gbar_group) {
		DbgFree(item->gbar_group);
	}

	item->gbar_group = dup_rtrim(buffer);
	if (!item->gbar_group) {
		ErrPrint("Error: %s\n", strerror(errno));
	}
}

HAPI struct parser *parser_load(const char *pkgname)
{
	struct parser *item;
	FILE *fp;
	int c;
	enum state {
		START,
		SPACE,
		TOKEN,
		VALUE,
		ERROR,
		COMMENT,
		END
	} state;
	int ch_idx;
	int token_idx;
	int buffer_idx;
	int quote;
	int len;
	int linelen;
	char buffer[256];
	static const struct token_parser {
		const char *name;
		void (*handler)(struct parser *, char *buffer);
	} token_handler[] = {
		{
			.name = "period",
			.handler = period_handler,
		},
		{
			.name = "timeout",
			.handler = timeout_handler,
		},
		{
			.name = "network",
			.handler = network_handler,
		},
		{
			.name = "auto_launch",
			.handler = auto_launch_handler,
		},
		{
			.name = "size",
			.handler = size_handler,
		},
		{
			.name = "group",
			.handler = group_handler,
		},
		{
			.name = "secured",
			.handler = secured_handler,
		},
		{
			.name = "dynamicbox_path",
			.handler = dbox_path_handler,
		},
		{
			.name = "dynamicbox_group",
			.handler = dbox_group_handler,
		},
		{
			.name = "gbar_path",
			.handler = gbar_path_handler,
		},
		{
			.name = "gbar_group",
			.handler = gbar_group_handler,
		},
		{
			.name = "gbar_size",
			.handler = gbar_size_handler,
		},
		{
			.name = "pinup",
			.handler = pinup_handler,
		},
		{
			.name = "text_dynamicbox",
			.handler = text_dbox_handler,
		},
		{
			.name = "text_gbar",
			.handler = text_gbar_handler,
		},
		{
			.name = "buffer_dynamicbox",
			.handler = buffer_dbox_handler,
		},
		{
			.name = "buffer_gbar",
			.handler = buffer_gbar_handler,
		},
		{
			.name = "script",
			.handler = script_handler,
		},
		{
			.name = "abi",
			.handler = abi_handler,
		},
		{
			.name = NULL,
			.handler = NULL,
		},
	};
	int ret;

	item = calloc(1, sizeof(*item));
	if (!item) {
		return 0;
	}

	/* live-, .conf */
	len = strlen(DYNAMICBOX_CONF_CONF_PATH) + strlen(pkgname) * 2;
	item->filename = malloc(len);
	if (!item->filename) {
		ErrPrint("Error: %s\n", strerror(errno));
		DbgFree(item);
		return 0;
	}

	ret = snprintf(item->filename, len, DYNAMICBOX_CONF_CONF_PATH, pkgname, pkgname);
	if (ret < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		DbgFree(item->filename);
		DbgFree(item);
		return 0;
	}

	item->dbox_path = NULL;
	item->dbox_group = NULL;
	item->gbar_width = 0;
	item->gbar_height = 0;
	item->auto_launch = NULL;
	item->size = 0x00000001;
	item->group = NULL;
	item->secured = 0;
	item->pinup = 0;

	fp = fopen(item->filename, "rt");
	if (!fp) {
		DbgFree(item->filename);
		DbgFree(item);
		return 0;
	}

	state = START;
	ch_idx = 0;
	token_idx = -1;
	buffer_idx = 0;
	quote = 0;
	linelen = 0;
	do {
		c = getc(fp);
		if ((c == EOF) && (state == VALUE)) {
			state = END;
		}

		switch (state) {
		case COMMENT:
			if (c == CR || c == LF || c == EOF) {
				buffer[buffer_idx] = '\0';

				state = START;
				token_idx = -1;
				ch_idx = 0;
				buffer_idx = 0;
				linelen = -1; /* Will be ZERO by follwing increment code */
				quote = 0;
			} else {
				buffer[buffer_idx++] = c;
				if (buffer_idx == (sizeof(buffer) - 1)) {
					buffer[buffer_idx] = '\0';
					buffer_idx = 0;
				}
			}
			break;
		case START:
			if (linelen == 0 && c == '#') {
				state = COMMENT;
			} else if (isspace(c)) {
				/* Ignore empty space */
			} else {
				state = TOKEN;
				ungetc(c, fp);
			}
			break;
		case SPACE:
			if (c == '=') {
				state = VALUE;
			} else if (!isspace(c)) {
				state = ERROR;
			}
			break;
		case VALUE:
			if (c == '"') {
				if (quote == 1) {
					buffer[buffer_idx] = '\0';
					state = END;
				} else if (buffer_idx != 0) {
					buffer[buffer_idx++] = c;
					if (buffer_idx >= sizeof(buffer)) {
						state = ERROR;
					}
				} else {
					quote = 1;
				}
			} else if (isspace(c)) {
				if (buffer_idx == 0) {
					/* Ignore */
				} else if (quote == 1) {
					buffer[buffer_idx++] = c;
					if (buffer_idx >= sizeof(buffer)) {
						state = ERROR;
					}
				} else {
					buffer[buffer_idx] = '\0';
					ungetc(c, fp);
					state = END;
				}
			} else {
				buffer[buffer_idx++] = c;
				if (buffer_idx >= sizeof(buffer)) {
					state = ERROR;
				}
			}
			break;
		case TOKEN:
			if (c == '=') {
				if (token_idx < 0) {
					state = ERROR;
				} else {
					state = VALUE;
				}
			} else if (isspace(c)) {
				if (token_idx < 0) {
					break;
				}

				if (token_handler[token_idx].name[ch_idx] != '\0') {
					state = ERROR;
				} else {
					state = SPACE;
				}
			} else  {
				if (token_idx < 0) {
					/* Now start to find a token! */
					token_idx = 0;
				}

				if (token_handler[token_idx].name[ch_idx] == c) {
					ch_idx++;
				} else {
					ungetc(c, fp);
					while (ch_idx-- > 0) {
						ungetc(token_handler[token_idx].name[ch_idx], fp);
					}

					token_idx++;

					if (token_handler[token_idx].name == NULL) {
						state = ERROR;
					} else {
						ch_idx = 0;
					}
				}
			}
			break;
		case ERROR:
			if (c == CR || c == LF || c == EOF) {
				state = START;
				token_idx = -1;
				buffer_idx = 0;
				ch_idx = 0;
				linelen = -1;
				quote = 0;
			}
			break;
		case END:
			if (c == LF || c == CR || c == EOF) {
				state = START;

				/*!
				 * \NOTE
				 * Make the string terminator
				 */
				buffer[buffer_idx] = '\0';

				if (token_idx >= 0 && token_handler[token_idx].handler) {
					token_handler[token_idx].handler(item, buffer);
				}

				token_idx = -1;
				ch_idx = 0;
				buffer_idx = 0;
				linelen = -1;
				quote = 0;
				/* Finish */
			} else if (isspace(c)) {
				/* ignore */
			} else {
				state = ERROR;
			}
			break;
		default:
			/* ?? */
			break;
		}

		linelen++;
	} while (c != EOF);

	if (fclose(fp) != 0) {
		ErrPrint("fclose: %s\n", strerror(errno));
	}

	s_list = eina_list_append(s_list, item);
	return item;
}

HAPI int parser_unload(struct parser *item)
{
	s_list = eina_list_remove(s_list, item);

	DbgFree(item->auto_launch);
	DbgFree(item->abi);
	DbgFree(item->script);
	DbgFree(item->group);
	DbgFree(item->gbar_group);
	DbgFree(item->gbar_path);
	DbgFree(item->dbox_group);
	DbgFree(item->dbox_path);
	DbgFree(item->filename);
	DbgFree(item);
	return DBOX_STATUS_ERROR_NONE;
}

/* End of a file */
