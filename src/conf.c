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

#include <Ecore_X.h>
#include <ctype.h>

#include <dlog.h>

#include "conf.h"
#include "util.h"
#include "debug.h"

HAPI struct conf g_conf = {
	.width = 0,
	.height = 0,

	.base_width = 720,
	.base_height = 1280,

	.minimum_period = 1.0f,

	.default_conf.script = "edje",
	.default_conf.abi = "c",
	.default_conf.pd_group = "disclosure",
	.default_conf.period = -1.0f,

	.launch_key.name = "name",
	.launch_key.secured = "secured",
	.launch_key.abi = "abi",

	.default_packet_time = 0.0001f,

	.empty_content = "",
	.empty_title = "",

	.default_content = "default",
	.default_title = "",

	.minimum_space = 5242880,

	.replace_tag = "/APPID/",

	.slave_ttl = 30.0f,

	.max_log_line = 1000,
	.max_log_file = 3,

	.sqlite_flush_max = 1048576,

	.path = {
		.conf = "/opt/usr/live/%s/etc/%s.conf",
		.image = "/opt/usr/share/live_magazine/",
		.slave_log = "/opt/usr/share/live_magazine/log",
		.script = "/opt/usr/live/%s/res/script/%s.edj",
		.root = "/opt/usr/live/",
		.script_port = "/opt/usr/live/script_port/",
		.db = "/opt/dbspace/.livebox.db",
	},

	.ping_time = 240.0f,
	.slave_max_load = 30,

	.use_sw_backend = 0,
	.provider_method = "pixmap",
	.debug_mode = 0,
	.overwrite_content = 0,
	.com_core_thread = 1,
};

static void conf_update_size(void)
{
	ecore_x_window_size_get(0, &g_conf.width, &g_conf.height);
}

static void use_sw_backend_handler(char *buffer)
{
	g_conf.use_sw_backend = !strcasecmp(buffer, "true");
	DbgPrint("SW Backend: %d\n", g_conf.use_sw_backend);
}

static void provider_method_handler(char *buffer)
{
	g_conf.provider_method = strdup(buffer);
	if (!g_conf.provider_method)
		ErrPrint("Heap: %s\n", strerror(errno));

	DbgPrint("Method: %s\n", g_conf.provider_method);
}

static void debug_mode_handler(char *buffer)
{
	g_conf.debug_mode = !strcasecmp(buffer, "true");
	DbgPrint("Debug mode: %d\n", g_conf.debug_mode);
}

static void overwrite_content_handler(char *buffer)
{
	g_conf.overwrite_content = !strcasecmp(buffer, "true");
	DbgPrint("Overwrite Content: %d\n", g_conf.overwrite_content);
}

static void com_core_thread_handler(char *buffer)
{
	g_conf.com_core_thread = !strcasecmp(buffer, "true");
	DbgPrint("Com core thread: %d\n", g_conf.com_core_thread);
}

static void base_width_handler(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.base_width) != 1)
		ErrPrint("Failed to parse the base_width\n");

	DbgPrint("Base width: %d\n", g_conf.base_width);
}

static void base_height_handler(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.base_height) != 1)
		ErrPrint("Failed to parse the base_height\n");
	DbgPrint("Base height: %d\n", g_conf.base_height);
}

static void minimum_period_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.minimum_period) != 1)
		ErrPrint("Failed to parse the minimum_period\n");
	DbgPrint("Minimum period: %lf\n", g_conf.minimum_period);
}

static void script_handler(char *buffer)
{
	g_conf.default_conf.script = strdup(buffer);
	if (!g_conf.default_conf.script)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("Default script: %s\n", g_conf.default_conf.script);
}

static void default_abi_handler(char *buffer)
{
	g_conf.default_conf.abi = strdup(buffer);
	if (!g_conf.default_conf.abi)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("Default ABI: %s\n", g_conf.default_conf.abi);
}

static void default_group_handler(char *buffer)
{
	g_conf.default_conf.pd_group = strdup(buffer);
	if (!g_conf.default_conf.pd_group)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("Default PD Group: %s\n", g_conf.default_conf.pd_group);
}

static void default_period_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.default_conf.period) != 1)
		ErrPrint("Failed to parse the default_period\n");
	DbgPrint("Default Period: %lf\n", g_conf.default_conf.period);
}

static void default_packet_time_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.default_packet_time) != 1)
		ErrPrint("Failed to parse the default_packet_time\n");
	DbgPrint("Default packet time: %lf\n", g_conf.default_packet_time);
}

static void default_content_handler(char *buffer)
{
	g_conf.default_content = strdup(buffer);
	if (!g_conf.default_content)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("Default content: %s\n", g_conf.default_content);
}

static void default_title_handler(char *buffer)
{
	g_conf.default_title = strdup(buffer);
	if (!g_conf.default_title)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("Default title: %s\n", g_conf.default_title);
}

static void minimum_space_handler(char *buffer)
{
	if (sscanf(buffer, "%lu", &g_conf.minimum_space) != 1)
		ErrPrint("Failed to parse the minimum_space\n");
	DbgPrint("Minimum space: %lu\n", g_conf.minimum_space);
}

static void replace_tag_handler(char *buffer)
{
	g_conf.replace_tag = strdup(buffer);
	if (!g_conf.replace_tag)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("Replace Tag: %s\n", g_conf.replace_tag);
}

static void slave_ttl_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.slave_ttl) != 1)
		ErrPrint("Failed to parse the slave_ttl\n");
	DbgPrint("Slave TTL: %s\n", g_conf.slave_ttl);
}

static void max_log_line_handler(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.max_log_line) != 1)
		ErrPrint("Failed to parse the max_log_line\n");
	DbgPrint("Max log line: %d\n", g_conf.max_log_line);
}

static void max_log_file_handler(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.max_log_file) != 1)
		ErrPrint("Failed to parse the max_log_file\n");
	DbgPrint("Max log file: %d\n", g_conf.max_log_file);
}

static void sqlite_flush_max_handler(char *buffer)
{
	if (sscanf(buffer, "%lu", &g_conf.sqlite_flush_max) != 1)
		ErrPrint("Failed to parse the sqlite_flush_max\n");
	DbgPrint("Flush size: %lu\n", g_conf.sqlite_flush_max);
}

static void db_path_handler(char *buffer)
{
	g_conf.path.db = strdup(buffer);
	if (!g_conf.path.db)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("DB Path: %s\n", g_conf.path.db);
}

static void log_path_handler(char *buffer)
{
	g_conf.path.slave_log = strdup(buffer);
	if (!g_conf.path.slave_log)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("LOG Path: %s\n", g_conf.path.slave_log);
}

static void script_port_path_handler(char *buffer)
{
	g_conf.path.script_port = strdup(buffer);
	if (!g_conf.path.script_port)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("Script Port PATH: %s\n", g_conf.path.script_port);
}

static void share_path_handler(char *buffer)
{
	g_conf.path.image = strdup(buffer);
	if (!g_conf.path.image)
		ErrPrint("Heap: %s\n", strerror(errno));
	DbgPrint("Shared folder: %s\n", g_conf.path.image);
}

static void ping_time_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.ping_time) != 1)
		ErrPrint("Failed to parse the ping_time\n");
	DbgPrint("Default ping time: %lf\n", g_conf.ping_time);
}

static void slave_max_loader(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.slave_max_load) != 1)
		ErrPrint("Failed to parse the slave_max_load\n");
	DbgPrint("Max load: %d\n", g_conf.slave_max_load);
}

HAPI int conf_loader(void)
{
	FILE *fp;
	int c;
	enum state {
		START,
		SPACE,
		TOKEN,
		VALUE,
		ERROR,
		COMMENT,
		END,
	} state;
	int ch_idx;
	int token_idx;
	int buffer_idx;
	int quote;
	int linelen;
	char buffer[256];
	static const struct token_parser {
		const char *name;
		void (*handler)(char *buffer);
	} token_handler[] = {
		{
			.name = "base_width",
			.handler = base_width_handler,
		},
		{
			.name = "base_height",
			.handler = base_height_handler,
		},
		{
			.name = "minimum_period",
			.handler = minimum_period_handler,
		},
		{
			.name = "script",
			.handler = script_handler,
		},
		{
			.name = "default_abi",
			.handler = default_abi_handler,
		},
		{
			.name = "default_group",
			.handler = default_group_handler,
		},
		{
			.name = "default_period",
			.handler = default_period_handler,
		},
		{
			.name = "default_packet_time",
			.handler = default_packet_time_handler,
		},
		{
			.name = "default_content",
			.handler = default_content_handler,
		},
		{
			.name = "default_title",
			.handler = default_title_handler,
		},
		{
			.name = "minimum_space",
			.handler = minimum_space_handler,
		},
		{
			.name = "replace_tag",
			.handler = replace_tag_handler,
		},
		{
			.name = "slave_ttl",
			.handler = slave_ttl_handler,
		},
		{
			.name = "max_log_line",
			.handler = max_log_line_handler,
		},
		{
			.name = "max_log_file",
			.handler = max_log_file_handler,
		},
		{
			.name = "sqilte_flush_max",
			.handler = sqlite_flush_max_handler,
		},
		{
			.name = "db_path",
			.handler = db_path_handler,
		},
		{
			.name = "log_path",
			.handler = log_path_handler,
		},
		{
			.name = "share_path",
			.handler = share_path_handler,
		},
		{
			.name = "script_port_path",
			.handler = script_port_path_handler,
		},
		{
			.name = "ping_interval",
			.handler = ping_time_handler,
		},
		{
			.name = "slave_max_load",
			.handler = slave_max_loader,
		},
		{
			.name = "use_sw_backend",
			.handler = use_sw_backend_handler,
		},
		{
			.name = "provider_method",
			.handler = provider_method_handler,
		},
		{
			.name = "debug_mode",
			.handler = debug_mode_handler,
		},
		{
			.name = "overwrite_content",
			.handler = overwrite_content_handler,
		},
		{
			.name = "com_core_thread",
			.handler = com_core_thread_handler,
		},
		{
			.name = NULL,
			.handler = NULL,
		},
	};

	conf_update_size();

	fp = fopen("/usr/share/data-provider-master/conf.ini", "rt");
	if (!fp) {
		ErrPrint("Error: %s\n", strerror(errno));
		return -EIO;
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
			LOGD("[%s:%d] VALUE state EOF\n", __func__, __LINE__);
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
			if (c == '=')
				state = VALUE;
			else if (!isspace(c))
				state = ERROR;
			break;
		case VALUE:
			if (c == '"') {
				if (quote == 1) {
					buffer[buffer_idx] = '\0';
					state = END;
				} else if (buffer_idx != 0) {
					buffer[buffer_idx++] = c;
					if (buffer_idx >= sizeof(buffer))
						state = ERROR;
				} else {
					quote = 1;
				}
			} else if (isspace(c)) {
				if (buffer_idx == 0) {
					/* Ignore */
				} else if (quote == 1) {
					buffer[buffer_idx++] = c;
					if (buffer_idx >= sizeof(buffer))
						state = ERROR;
				} else {
					buffer[buffer_idx] = '\0';
					ungetc(c, fp);
					state = END;
				}
			} else {
				buffer[buffer_idx++] = c;
				if (buffer_idx >= sizeof(buffer))
					state = ERROR;
			}
			break;
		case TOKEN:
			if (c == '=') {
				if (token_idx < 0)
					state = ERROR;
				else
					state = VALUE;
			} else if (isspace(c)) {
				if (token_idx < 0)
					break;

				if (token_handler[token_idx].name[ch_idx] != '\0')
					state = ERROR;
				else
					state = SPACE;
			} else  {
				if (token_idx < 0) {
					/* Now start to find a token! */
					token_idx = 0;
				}

				if (token_handler[token_idx].name[ch_idx] == c) {
					ch_idx++;
				} else {
					ungetc(c, fp);
					while (ch_idx-- > 0)
						ungetc(token_handler[token_idx].name[ch_idx], fp);

					token_idx++;

					if (token_handler[token_idx].name == NULL)
						state = ERROR;
					else
						ch_idx = 0;
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

				if (token_idx >= 0 && token_handler[token_idx].handler) {
					buffer[buffer_idx] = '\0';
					DbgPrint("BUFFER: [%s]\n", buffer);
					token_handler[token_idx].handler(buffer);
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

	fclose(fp);
	return 0;
}

/* End of a file */
