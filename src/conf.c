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

#include <Ecore_X.h>
#include <ctype.h>

#include <dlog.h>
#include <livebox-errno.h>

#include "conf.h"
#include "util.h"
#include "debug.h"

static const char *CONF_DEFAULT_PATH_CONF = "/opt/usr/live/%s/etc/%s.conf";
static const char *CONF_DEFAULT_PATH_IMAGE = "/opt/usr/share/live_magazine/";
static const char *CONF_DEFAULT_PATH_LOG = "/opt/usr/share/live_magazine/log";
static const char *CONF_DEFAULT_PATH_READER = "/opt/usr/share/live_magazine/reader";
static const char *CONF_DEFAULT_PATH_ALWAYS = "/opt/usr/share/live_magazine/always";
static const char *CONF_DEFAULT_PATH_SCRIPT = "/opt/usr/live/%s/res/script/%s.edj";
static const char *CONF_DEFAULT_PATH_ROOT = "/opt/usr/live/";
static const char *CONF_DEFAULT_PATH_SCRIPT_PORT = "/usr/share/data-provider-master/plugin-script/";
static const char *CONF_DEFAULT_PATH_DB = "/opt/dbspace/.livebox.db";
static const char *CONF_DEFAULT_PATH_INPUT = "/dev/input/event1";
static const char *CONF_DEFAULT_SCRIPT_TYPE = "edje";
static const char *CONF_DEFAULT_ABI = "c";
static const char *CONF_DEFAULT_PD_GROUP = "disclosure";
static const char *CONF_DEFAULT_LAUNCH_BUNDLE_NAME = "name";
static const char *CONF_DEFAULT_LAUNCH_BUNDLE_SECURED = "secured";
static const char *CONF_DEFAULT_LAUNCH_BUNDLE_ABI = "abi";
static const char *CONF_DEFAULT_CONTENT = "default";
static const char *CONF_DEFAULT_TITLE = "";
static const char *CONF_DEFAULT_EMPTY_CONTENT = "";
static const char *CONF_DEFAULT_EMPTY_TITLE = "";
static const char *CONF_DEFAULT_REPLACE_TAG = "/APPID/";
static const char *CONF_DEFAULT_PROVIDER_METHOD = "pixmap";
static const int CONF_DEFAULT_WIDTH = 0;
static const int CONF_DEFAULT_HEIGHT = 0;
static const int CONF_DEFAULT_BASE_WIDTH = 720;
static const int CONF_DEFAULT_BASE_HEIGHT = 1280;
static const double CONF_DEFAULT_MINIMUM_PERIOD = 1.0f;
static const double CONF_DEFAULT_PERIOD = -1.0f;
static const double CONF_DEFAULT_PACKET_TIME = 0.0001f;
static const unsigned long CONF_DEFAULT_MINIMUM_SPACE = 5242880;
static const double CONF_DEFAULT_SLAVE_TTL = 30.0f;
static const double CONF_DEFAULT_SLAVE_ACTIVATE_TIME = 30.0f;
static const double CONF_DEFAULT_SLAVE_RELAUNCH_TIME = 3.0f;
static const int CONF_DEFAULT_SLAVE_RELAUNCH_COUNT = 3;
static const int CONF_DEFAULT_MAX_LOG_LINE = 1000;
static const int CONF_DEFAULT_MAX_LOG_FILE = 3;
static const int CONF_DEFAULT_SQLITE_FLUSH_MAX = 1048576;
static const double CONF_DEFAULT_PING_TIME = 240.0f;
static const int CONF_DEFAULT_SLAVE_MAX_LOAD = 30;
static const int CONF_DEFAULT_USE_SW_BACKEND = 0;
static const int CONF_DEFAULT_DEBUG_MODE = 0;
static const int CONF_DEFAULT_OVERWRITE_CONTENT = 0;
static const int CONF_DEFAULT_COM_CORE_THREAD = 1;
static const int CONF_DEFAULT_USE_XMONITOR = 0;
static const double CONF_DEFAULT_SCALE_WIDTH_FACTOR = 1.0f;
static const double CONF_DEFAULT_SCALE_HEIGHT_FACTOR = 1.0f;
static const double CONF_DEFAULT_PD_REQUEST_TIMEOUT = 5.0f;

HAPI struct conf g_conf;

HAPI void conf_update_size(void)
{
	ecore_x_window_size_get(0, &g_conf.width, &g_conf.height);

	g_conf.scale_width_factor = (double)g_conf.width / (double)BASE_W;
	g_conf.scale_height_factor = (double)g_conf.height / (double)BASE_H;
}

static void use_xmonitor(char *buffer)
{
	g_conf.use_xmonitor = !strcasecmp(buffer, "true");
}

static void use_sw_backend_handler(char *buffer)
{
	g_conf.use_sw_backend = !strcasecmp(buffer, "true");
}

static void provider_method_handler(char *buffer)
{
	g_conf.provider_method = strdup(buffer);
	if (!g_conf.provider_method) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void debug_mode_handler(char *buffer)
{
	g_conf.debug_mode = !strcasecmp(buffer, "true");
}

static void overwrite_content_handler(char *buffer)
{
	g_conf.overwrite_content = !strcasecmp(buffer, "true");
}

static void com_core_thread_handler(char *buffer)
{
	g_conf.com_core_thread = !strcasecmp(buffer, "true");
}

static void base_width_handler(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.base_width) != 1) {
		ErrPrint("Failed to parse the base_width\n");
	}
}

static void base_height_handler(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.base_height) != 1) {
		ErrPrint("Failed to parse the base_height\n");
	}
}

static void minimum_period_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.minimum_period) != 1) {
		ErrPrint("Failed to parse the minimum_period\n");
	}
	DbgPrint("Minimum period: %lf\n", g_conf.minimum_period);
}

static void script_handler(char *buffer)
{
	g_conf.default_conf.script = strdup(buffer);
	if (!g_conf.default_conf.script) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void default_abi_handler(char *buffer)
{
	g_conf.default_conf.abi = strdup(buffer);
	if (!g_conf.default_conf.abi) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void default_group_handler(char *buffer)
{
	g_conf.default_conf.pd_group = strdup(buffer);
	if (!g_conf.default_conf.pd_group) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void default_period_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.default_conf.period) != 1) {
		ErrPrint("Failed to parse the default_period\n");
	}
	DbgPrint("Default Period: %lf\n", g_conf.default_conf.period);
}

static void default_packet_time_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.default_packet_time) != 1) {
		ErrPrint("Failed to parse the default_packet_time\n");
	}
	DbgPrint("Default packet time: %lf\n", g_conf.default_packet_time);
}

static void default_content_handler(char *buffer)
{
	g_conf.default_content = strdup(buffer);
	if (!g_conf.default_content) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void default_title_handler(char *buffer)
{
	g_conf.default_title = strdup(buffer);
	if (!g_conf.default_title) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void minimum_space_handler(char *buffer)
{
	if (sscanf(buffer, "%lu", &g_conf.minimum_space) != 1) {
		ErrPrint("Failed to parse the minimum_space\n");
	}
}

static void replace_tag_handler(char *buffer)
{
	g_conf.replace_tag = strdup(buffer);
	if (!g_conf.replace_tag) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void slave_ttl_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.slave_ttl) != 1) {
		ErrPrint("Failed to parse the slave_ttl\n");
	}
	DbgPrint("Slave TTL: %lf\n", g_conf.slave_ttl);
}

static void slave_activate_time_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.slave_activate_time) != 1) {
		ErrPrint("Failed to parse the slave_activate_time\n");
	}
	DbgPrint("Slave activate time: %lf\n", g_conf.slave_activate_time);
}

static void slave_relaunch_time_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.slave_relaunch_time) != 1) {
		ErrPrint("Failed to parse the slave_activate_time\n");
	}
	DbgPrint("Slave relaunch time: %lf\n", g_conf.slave_relaunch_time);
}

static void slave_relaunch_count_handler(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.slave_relaunch_count) != 1) {
		ErrPrint("Failed to parse the max_log_line\n");
	}
}

static void max_log_line_handler(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.max_log_line) != 1) {
		ErrPrint("Failed to parse the max_log_line\n");
	}
}

static void max_log_file_handler(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.max_log_file) != 1) {
		ErrPrint("Failed to parse the max_log_file\n");
	}
}

static void sqlite_flush_max_handler(char *buffer)
{
	if (sscanf(buffer, "%lu", &g_conf.sqlite_flush_max) != 1) {
		ErrPrint("Failed to parse the sqlite_flush_max\n");
	}
}

static void db_path_handler(char *buffer)
{
	g_conf.path.db = strdup(buffer);
	if (!g_conf.path.db) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void reader_path_handler(char *buffer)
{
	g_conf.path.reader = strdup(buffer);
	if (!g_conf.path.reader) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void always_path_handler(char *buffer)
{
	g_conf.path.always = strdup(buffer);
	if (!g_conf.path.always) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void log_path_handler(char *buffer)
{
	g_conf.path.slave_log = strdup(buffer);
	if (!g_conf.path.slave_log) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void script_port_path_handler(char *buffer)
{
	g_conf.path.script_port = strdup(buffer);
	if (!g_conf.path.script_port) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void share_path_handler(char *buffer)
{
	g_conf.path.image = strdup(buffer);
	if (!g_conf.path.image) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void input_path_handler(char *buffer)
{
	g_conf.path.input = strdup(buffer);
	if (!g_conf.path.input) {
		ErrPrint("Heap: %s\n", strerror(errno));
	}
}

static void ping_time_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.ping_time) != 1) {
		ErrPrint("Failed to parse the ping_time\n");
	}
	DbgPrint("Default ping time: %lf\n", g_conf.ping_time);
}

static void slave_max_loader(char *buffer)
{
	if (sscanf(buffer, "%d", &g_conf.slave_max_load) != 1) {
		ErrPrint("Failed to parse the slave_max_load\n");
	}
}

static void pd_request_timeout_handler(char *buffer)
{
	if (sscanf(buffer, "%lf", &g_conf.pd_request_timeout) != 1) {
		ErrPrint("Failed to parse the request_timeout\n");
	}
	DbgPrint("Default PD request timeout: %lf\n", g_conf.pd_request_timeout);
}

HAPI void conf_init(void)
{
	g_conf.width = CONF_DEFAULT_WIDTH;
	g_conf.height = CONF_DEFAULT_HEIGHT;
	g_conf.base_width = CONF_DEFAULT_BASE_WIDTH;
	g_conf.base_height = CONF_DEFAULT_BASE_HEIGHT;
	g_conf.minimum_period = CONF_DEFAULT_MINIMUM_PERIOD;
	g_conf.default_conf.period = CONF_DEFAULT_PERIOD;
	g_conf.minimum_space = CONF_DEFAULT_MINIMUM_SPACE;
	g_conf.default_packet_time = CONF_DEFAULT_PACKET_TIME;
	g_conf.slave_ttl = CONF_DEFAULT_SLAVE_TTL;
	g_conf.slave_activate_time = CONF_DEFAULT_SLAVE_ACTIVATE_TIME;
	g_conf.slave_relaunch_time = CONF_DEFAULT_SLAVE_RELAUNCH_TIME;
	g_conf.slave_relaunch_count = CONF_DEFAULT_SLAVE_RELAUNCH_COUNT;
	g_conf.max_log_line = CONF_DEFAULT_MAX_LOG_LINE;
	g_conf.max_log_file = CONF_DEFAULT_MAX_LOG_FILE;
	g_conf.sqlite_flush_max = CONF_DEFAULT_SQLITE_FLUSH_MAX;
	g_conf.ping_time = CONF_DEFAULT_PING_TIME;
	g_conf.slave_max_load = CONF_DEFAULT_SLAVE_MAX_LOAD;
	g_conf.use_sw_backend = CONF_DEFAULT_USE_SW_BACKEND;
	g_conf.debug_mode = CONF_DEFAULT_DEBUG_MODE;
	g_conf.overwrite_content = CONF_DEFAULT_OVERWRITE_CONTENT;
	g_conf.com_core_thread = CONF_DEFAULT_COM_CORE_THREAD;
	g_conf.use_xmonitor = CONF_DEFAULT_USE_XMONITOR;
	g_conf.scale_width_factor = CONF_DEFAULT_SCALE_WIDTH_FACTOR;
	g_conf.scale_height_factor = CONF_DEFAULT_SCALE_HEIGHT_FACTOR;
	g_conf.pd_request_timeout = CONF_DEFAULT_PD_REQUEST_TIMEOUT;
	g_conf.default_conf.script = (char *)CONF_DEFAULT_SCRIPT_TYPE;
	g_conf.default_conf.abi = (char *)CONF_DEFAULT_ABI;
	g_conf.default_conf.pd_group = (char *)CONF_DEFAULT_PD_GROUP;
	g_conf.launch_key.name = (char *)CONF_DEFAULT_LAUNCH_BUNDLE_NAME;
	g_conf.launch_key.secured = (char *)CONF_DEFAULT_LAUNCH_BUNDLE_SECURED;
	g_conf.launch_key.abi = (char *)CONF_DEFAULT_LAUNCH_BUNDLE_ABI;
	g_conf.empty_content = (char *)CONF_DEFAULT_EMPTY_CONTENT;
	g_conf.empty_title = (char *)CONF_DEFAULT_EMPTY_TITLE;
	g_conf.default_content = (char *)CONF_DEFAULT_CONTENT;
	g_conf.default_title = (char *)CONF_DEFAULT_TITLE;
	g_conf.replace_tag = (char *)CONF_DEFAULT_REPLACE_TAG;
	g_conf.path.conf = (char *)CONF_DEFAULT_PATH_CONF;
	g_conf.path.image = (char *)CONF_DEFAULT_PATH_IMAGE;
	g_conf.path.slave_log = (char *)CONF_DEFAULT_PATH_LOG;
	g_conf.path.reader = (char *)CONF_DEFAULT_PATH_READER;
	g_conf.path.always = (char *)CONF_DEFAULT_PATH_ALWAYS;
	g_conf.path.script = (char *)CONF_DEFAULT_PATH_SCRIPT;
	g_conf.path.root = (char *)CONF_DEFAULT_PATH_ROOT;
	g_conf.path.script_port = (char *)CONF_DEFAULT_PATH_SCRIPT_PORT;
	g_conf.path.db = (char *)CONF_DEFAULT_PATH_DB;
	g_conf.path.input = (char *)CONF_DEFAULT_PATH_INPUT;
	g_conf.provider_method = (char *)CONF_DEFAULT_PROVIDER_METHOD;
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
		END
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
			.name = "slave_activate_time",
			.handler = slave_activate_time_handler,
		},
		{
			.name = "slave_relaunch_time",
			.handler = slave_relaunch_time_handler,
		},
		{
			.name = "slave_relaunch_count",
			.handler = slave_relaunch_count_handler,
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
			.name = "reader_path",
			.handler = reader_path_handler,
		},
		{
			.name = "always_path",
			.handler = always_path_handler,
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
			.name = "use_xmonitor",
			.handler = use_xmonitor,
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
			.name = "input",
			.handler = input_path_handler,
		},
		{
			.name = "pd_request_timeout",
			.handler = pd_request_timeout_handler,
		},
		{
			.name = NULL,
			.handler = NULL,
		},
	};

	fp = fopen(DEFAULT_MASTER_CONF, "rt");
	if (!fp) {
		ErrPrint("Error: %s\n", strerror(errno));
		return LB_STATUS_ERROR_IO;
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
			DbgPrint("[%s:%d] VALUE state EOF\n", __func__, __LINE__);
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
					while (ch_idx-- > 0)
						ungetc(token_handler[token_idx].name[ch_idx], fp);

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

				if (token_idx >= 0 && token_handler[token_idx].handler) {
					buffer[buffer_idx] = '\0';
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

	if (fclose(fp) != 0) {
		ErrPrint("fclose: %s\n", strerror(errno));
	}
	return LB_STATUS_SUCCESS;
}

HAPI void conf_reset(void)
{
	g_conf.width = CONF_DEFAULT_WIDTH;
	g_conf.height = CONF_DEFAULT_HEIGHT;
	g_conf.base_width = CONF_DEFAULT_BASE_WIDTH;
	g_conf.base_height = CONF_DEFAULT_BASE_HEIGHT;
	g_conf.minimum_period = CONF_DEFAULT_MINIMUM_PERIOD;
	g_conf.default_conf.period = CONF_DEFAULT_PERIOD;
	g_conf.minimum_space = CONF_DEFAULT_MINIMUM_SPACE;
	g_conf.default_packet_time = CONF_DEFAULT_PACKET_TIME;
	g_conf.slave_ttl = CONF_DEFAULT_SLAVE_TTL;
	g_conf.slave_activate_time = CONF_DEFAULT_SLAVE_ACTIVATE_TIME;
	g_conf.slave_relaunch_time = CONF_DEFAULT_SLAVE_RELAUNCH_TIME;
	g_conf.slave_relaunch_count = CONF_DEFAULT_SLAVE_RELAUNCH_COUNT;
	g_conf.max_log_line = CONF_DEFAULT_MAX_LOG_LINE;
	g_conf.max_log_file = CONF_DEFAULT_MAX_LOG_FILE;
	g_conf.sqlite_flush_max = CONF_DEFAULT_SQLITE_FLUSH_MAX;
	g_conf.ping_time = CONF_DEFAULT_PING_TIME;
	g_conf.slave_max_load = CONF_DEFAULT_SLAVE_MAX_LOAD;
	g_conf.use_sw_backend = CONF_DEFAULT_USE_SW_BACKEND;
	g_conf.debug_mode = CONF_DEFAULT_DEBUG_MODE;
	g_conf.overwrite_content = CONF_DEFAULT_OVERWRITE_CONTENT;
	g_conf.com_core_thread = CONF_DEFAULT_COM_CORE_THREAD;
	g_conf.use_xmonitor = CONF_DEFAULT_USE_XMONITOR;
	g_conf.scale_width_factor = CONF_DEFAULT_SCALE_WIDTH_FACTOR;
	g_conf.scale_height_factor = CONF_DEFAULT_SCALE_HEIGHT_FACTOR;
	g_conf.pd_request_timeout = CONF_DEFAULT_PD_REQUEST_TIMEOUT;

	if (g_conf.default_conf.script != CONF_DEFAULT_SCRIPT_TYPE) {
		DbgFree(g_conf.default_conf.script);
		g_conf.default_conf.script = (char *)CONF_DEFAULT_SCRIPT_TYPE;
	}

	if (g_conf.default_conf.abi != CONF_DEFAULT_ABI) {
		DbgFree(g_conf.default_conf.abi);
		g_conf.default_conf.abi = (char *)CONF_DEFAULT_ABI;
	}

	if (g_conf.default_conf.pd_group != CONF_DEFAULT_PD_GROUP) {
		DbgFree(g_conf.default_conf.pd_group);
		g_conf.default_conf.pd_group = (char *)CONF_DEFAULT_PD_GROUP;
	}

	if (g_conf.launch_key.name != CONF_DEFAULT_LAUNCH_BUNDLE_NAME) {
		DbgFree(g_conf.launch_key.name);
		g_conf.launch_key.name = (char *)CONF_DEFAULT_LAUNCH_BUNDLE_NAME;
	}

	if (g_conf.launch_key.secured != CONF_DEFAULT_LAUNCH_BUNDLE_SECURED) {
		DbgFree(g_conf.launch_key.secured);
		g_conf.launch_key.secured = (char *)CONF_DEFAULT_LAUNCH_BUNDLE_SECURED;
	}

	if (g_conf.launch_key.abi != CONF_DEFAULT_LAUNCH_BUNDLE_ABI) {
		DbgFree(g_conf.launch_key.abi);
		g_conf.launch_key.abi = (char *)CONF_DEFAULT_LAUNCH_BUNDLE_ABI;
	}

	if (g_conf.empty_content != CONF_DEFAULT_EMPTY_CONTENT) {
		DbgFree(g_conf.empty_content);
		g_conf.empty_content = (char *)CONF_DEFAULT_EMPTY_CONTENT;
	}

	if (g_conf.empty_title != CONF_DEFAULT_EMPTY_TITLE) {
		DbgFree(g_conf.empty_title);
		g_conf.empty_title = (char *)CONF_DEFAULT_EMPTY_TITLE;
	}

	if (g_conf.default_content != CONF_DEFAULT_CONTENT) {
		DbgFree(g_conf.default_content);
		g_conf.default_content = (char *)CONF_DEFAULT_CONTENT;
	}

	if (g_conf.default_title != CONF_DEFAULT_TITLE) {
		DbgFree(g_conf.default_title);
		g_conf.default_title = (char *)CONF_DEFAULT_TITLE;
	}

	if (g_conf.replace_tag != CONF_DEFAULT_REPLACE_TAG) {
		DbgFree(g_conf.replace_tag);
		g_conf.replace_tag = (char *)CONF_DEFAULT_REPLACE_TAG;
	}

	if (g_conf.path.conf != CONF_DEFAULT_PATH_CONF) {
		DbgFree(g_conf.path.conf);
		g_conf.path.conf = (char *)CONF_DEFAULT_PATH_CONF;
	}

	if (g_conf.path.image != CONF_DEFAULT_PATH_IMAGE) {
		DbgFree(g_conf.path.image);
		g_conf.path.image = (char *)CONF_DEFAULT_PATH_IMAGE;
	}

	if (g_conf.path.slave_log != CONF_DEFAULT_PATH_LOG) {
		DbgFree(g_conf.path.slave_log);
		g_conf.path.slave_log = (char *)CONF_DEFAULT_PATH_LOG;
	}

	if (g_conf.path.reader != CONF_DEFAULT_PATH_READER) {
		DbgFree(g_conf.path.reader);
		g_conf.path.reader = (char *)CONF_DEFAULT_PATH_READER;
	}

	if (g_conf.path.always != CONF_DEFAULT_PATH_ALWAYS) {
		DbgFree(g_conf.path.always);
		g_conf.path.always = (char *)CONF_DEFAULT_PATH_ALWAYS;
	}

	if (g_conf.path.script != CONF_DEFAULT_PATH_SCRIPT) {
		DbgFree(g_conf.path.script);
		g_conf.path.script = (char *)CONF_DEFAULT_PATH_SCRIPT;
	}

	if (g_conf.path.root != CONF_DEFAULT_PATH_ROOT) {
		DbgFree(g_conf.path.root);
		g_conf.path.root = (char *)CONF_DEFAULT_PATH_ROOT;
	}

	if (g_conf.path.script_port != CONF_DEFAULT_PATH_SCRIPT_PORT) {
		DbgFree(g_conf.path.script_port);
		g_conf.path.script_port = (char *)CONF_DEFAULT_PATH_SCRIPT_PORT;
	}

	if (g_conf.path.db != CONF_DEFAULT_PATH_DB) {
		DbgFree(g_conf.path.db);
		g_conf.path.db = (char *)CONF_DEFAULT_PATH_DB;
	}

	if (g_conf.path.input != CONF_DEFAULT_PATH_INPUT) {
		DbgFree(g_conf.path.input);
		g_conf.path.input = (char *)CONF_DEFAULT_PATH_INPUT;
	}

	if (g_conf.provider_method != CONF_DEFAULT_PROVIDER_METHOD) {
		DbgFree(g_conf.provider_method);
		g_conf.provider_method = (char *)CONF_DEFAULT_PROVIDER_METHOD;
	}
}

/* End of a file */
