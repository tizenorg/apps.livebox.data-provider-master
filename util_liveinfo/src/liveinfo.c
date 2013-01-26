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

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>

#include <glib.h>
#include <glib-object.h>

#include <packet.h>
#include <com-core_packet.h>
#include <com-core.h>

#include <livebox-service.h>

#include <Ecore.h>

#include "liveinfo.h"
#include "node.h"

#define PROMPT "liveinfo "

struct package {
	int primary;
	char *pkgid;

	int pid;
	char *slavename;
	char *abi;
	int refcnt;
	int fault_count;
	int inst_count;
};

struct instance {
	char *cluster;
	char *category;
	double period;
	char *state;
	int width;
	int height;
};

struct slave {
	int pid;
	char *pkgname;
	char *abi;
	int secured;
	int refcnt;
	int fault_count;
	char *state;
	int loaded_inst;
	int loaded_pkg;
	double ttl;
};

enum command {
	NOP,
	PKG_LIST,
	INST_LIST,
	TOGGLE_DEBUG,
	SLAVE_LIST,	
};

static struct info {
	int fifo_handle;
	int fd;
	Ecore_Fd_Handler *fd_handler;
	Ecore_Fd_Handler *in_handler;
	int input_fd;

	struct node *rootdir;
	struct node *curdir;

	enum command cmd;
} s_info = {
	.fifo_handle = -EINVAL,
	.fd = -EINVAL,
	.fd_handler = NULL,
	.in_handler = NULL,
	.input_fd = -1,
	.rootdir = NULL,
	.curdir = NULL,
	.cmd = NOP,
};

static inline void ls(void)
{
	struct node *node;
	int cnt = 0;
	int is_package;
	int is_provider;
	int is_instance;
	char *path;

	is_package = node_name(s_info.curdir) && !strcmp(node_name(s_info.curdir), "package");
	is_provider = !is_package && node_name(s_info.curdir) && !strcmp(node_name(s_info.curdir), "provider");
	is_instance = !is_package && !is_provider && node_parent(s_info.curdir) && node_name(node_parent(s_info.curdir)) && !strcmp(node_name(node_parent(s_info.curdir)), "package");

	node = node_child(s_info.curdir);
	while (node) {
		if (is_package) {
			struct package *info;
			info = node_data(node);
			printf(" %3d %20s %5s ", info->inst_count, info->slavename ? info->slavename : "-", info->abi ? info->abi : "-");
		} else if (is_provider) {
			struct slave *info;
			info = node_data(node);
			printf(" %3d %5s %5.2f ", info->loaded_inst, info->abi ? info->abi : "-", info->ttl);
		} else if (is_instance) {
			struct instance *info;
			struct stat stat;
			char buf[4096];
			info = node_data(node);

			printf(" %5.2f %6s %10s %10s %4dx%-4d ", info->period, info->state, info->cluster, info->category, info->width, info->height);
			snprintf(buf, sizeof(buf), "/opt/usr/share/live_magazine/reader/%s", node_name(node));
			if (lstat(buf, &stat) < 0)
				printf("%3d ERR ", errno);
			else
				printf("%2.2lf MB ", (double)stat.st_size / 1024.0f / 1024.0f);
		}

		if (node_type(node) == NODE_DIR)
			printf("%s/", node_name(node));
		else if (node_type(node) == NODE_FILE)
			printf("%s", node_name(node));

		printf("\n");
		node = node_next_sibling(node);
		cnt++;
	}
	printf("Total: %d\n", cnt);
	path = node_to_abspath(s_info.curdir);
	printf(PROMPT"%s # ", path);
	free(path);
}

static void send_slave_list(void)
{
	struct packet *packet;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	packet = packet_create_noack("slave_list", "d", 0.0f);
	if (!packet) {
		printf("Failed to create a packet\n");
		return;
	}

	com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	s_info.cmd = SLAVE_LIST;
}

static void send_pkg_list(void)
{
	struct packet *packet;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	packet = packet_create_noack("pkg_list", "d", 0.0f);
	if (!packet) {
		printf("Failed to create a packet\n");
		return;
	}

	com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	s_info.cmd = PKG_LIST;
}

static void send_toggle_debug(void)
{
	struct packet *packet;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	packet = packet_create_noack("toggle_debug", "d", 0.0f);
	if (!packet) {
		printf("Failed to create a packet\n");
		return;
	}

	com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	s_info.cmd = TOGGLE_DEBUG;
}

static void send_inst_list(const char *pkgname)
{
	struct packet *packet;

	if (s_info.cmd != NOP) {
		printf("Previous command is not finished\n");
		return;
	}

	packet = packet_create_noack("inst_list", "s", pkgname);
	if (!packet) {
		printf("Failed to create a packet\n");
		return;
	}

	com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
	s_info.cmd = INST_LIST;
}

static inline void help(void)
{
	printf("liveinfo - Livebox utility\n");
	printf("------------------------------ [Command list] ------------------------------\n");
	printf("[32cd - Change directory[0m\n");
	printf("[32ls - List up content as a file[0m\n");
	printf("[32cat - Open a file to get some detail information[0m\n");
	printf("[32mexit - [0m\n");
	printf("[32mquit - [0m\n");
	printf("----------------------------------------------------------------------------\n");
}

static int pkglist_cb(const char *appid, const char *lbid, int is_prime, void *data)
{
	struct node *parent = data;
	struct node *node;
	struct package *info;

	info = calloc(1, sizeof(*info));
	if (!info) {
		printf("Error: %s\n", strerror(errno));
		return -ENOMEM;
	}

	info->pkgid = strdup(appid);
	if (!info->pkgid) {
		printf("Error: %s\n", strerror(errno));
		free(info);
		return -ENOMEM;
	}

	info->primary = is_prime;

	node = node_create(parent, lbid, NODE_DIR);
	if (!node) {
		free(info->pkgid);
		free(info);
		return -ENOMEM;
	}

	node_set_mode(node, NODE_READ | NODE_EXEC);
	node_set_data(node, info);
	return 0;
}

static inline void init_directory(void)
{
	struct node *node;
	s_info.rootdir = node_create(NULL, NULL, NODE_DIR);
	if (!s_info.rootdir)
		return;

	node = node_create(s_info.rootdir, "provider", NODE_DIR);
	if (!node) {
		node_destroy(s_info.rootdir);
		s_info.rootdir = NULL;
		return;
	}
	node_set_mode(node, NODE_READ | NODE_EXEC);

	node = node_create(s_info.rootdir, "package", NODE_DIR);
	if (!node) {
		node_destroy(node_child(s_info.rootdir));
		node_destroy(s_info.rootdir);
		s_info.rootdir = NULL;
		return;
	}
	node_set_mode(node, NODE_READ | NODE_EXEC);

	s_info.curdir = s_info.rootdir;
	livebox_service_get_pkglist(pkglist_cb, node);
	return;
}

static inline void fini_directory(void)
{
}

static inline void do_command(const char *cmd)
{
	char command[256];
	char argument[4096] = { '\0', };
	char *path;

	if (!strlen(cmd)) {
		char *path;
		path = node_to_abspath(s_info.curdir);
		printf(PROMPT"%s # ", path);
		free(path);
		return;
	}

	if (sscanf(cmd, "%255[^ ] %4095s", command, argument) == 2)
		cmd = command;

	if (!strcasecmp(cmd, "exit") || !strcasecmp(cmd, "quit")) {
		ecore_main_loop_quit();
	} else if (!strcasecmp(cmd, "toggle_debug")) {
		if (s_info.cmd != NOP) {
			printf("Waiting the server response\n");
			return;
		}

		send_toggle_debug();
	} else if (!strcasecmp(cmd, "ls")) {
		if (node_name(s_info.curdir)) {
			if (!strcmp(node_name(s_info.curdir), "package")) {
				if (s_info.cmd != NOP) {
					printf("Waiting the server response\n");
					return;
				}
				send_pkg_list();
				return;
			} else if (!strcmp(node_name(s_info.curdir), "provider")) {
				if (s_info.cmd != NOP) {
					printf("Waiting the server response\n");
					return;
				}
				send_slave_list();
				return;
			}
		}

		if (node_parent(s_info.curdir) && node_name(node_parent(s_info.curdir))) {
			if (!strcmp(node_name(node_parent(s_info.curdir)), "package")) {
				if (s_info.cmd != NOP) {
					printf("Waiting the server response\n");
					return;
				}
				send_inst_list(node_name(s_info.curdir));
				return;
			}
		}

		ls();
	} else if (!strcasecmp(cmd, "cd")) {
		struct node *node;

		if (!strcmp(argument, "."))
			return;

		if (s_info.cmd != NOP) {
			printf("Waiting the server response\n");
			return;
		}

		if (!strcmp(argument, "..")) {
			if (node_parent(s_info.curdir) == NULL)
				return;

			s_info.curdir = node_parent(s_info.curdir);
		}

		node = node_child(s_info.curdir);
		while (node) {
			if (!strcmp(node_name(node), argument)) {
				if (node_type(node) != NODE_DIR)
					printf("Unable to go into the %s\n", node_name(node));
				else
					s_info.curdir = node;

				break;
			}

			node = node_next_sibling(node);
		}
		if (!node)
			printf("Not found: %s\n", argument);

		path = node_to_abspath(s_info.curdir);
		printf(PROMPT"%s # ", path);
		free(path);
	} else if (!strcasecmp(cmd, "help")) {
		goto errout;
	} else {
		printf("Unknown command - \"help\"\n");
		path = node_to_abspath(s_info.curdir);
		printf(PROMPT"%s # ", path);
		free(path);
	}

	return;

errout:
	help();
	path = node_to_abspath(s_info.curdir);
	printf(PROMPT"%s # ", path);
	free(path);
}

static Eina_Bool input_cb(void *data, Ecore_Fd_Handler *fd_handler)
{
	static int idx = 0;
	static char cmd_buffer[256];
	char *path;
	char ch;
	int fd;

	fd = ecore_main_fd_handler_fd_get(fd_handler);
	if (fd < 0) {
		printf("FD is not valid: %d\n", fd);
		return ECORE_CALLBACK_CANCEL;
	}

	/*!
	 * \note
	 * Using this routine, we can implement the command recommend algorithm.
	 * When a few more characters are matched with history of command, we can show it to user
	 * Then the user will choose one or write new command
	 */

	/* Silly.. Silly */
	if (read(fd, &ch, sizeof(ch)) != sizeof(ch)) {
		printf("Failed to get a byte: %s\n", strerror(errno));
		return ECORE_CALLBACK_CANCEL;
	}

	switch (ch) {
	case 0x08: /* BKSP */
		cmd_buffer[idx] = '\0';
		if (idx > 0)
			idx--;
		cmd_buffer[idx] = ' ';
		path = node_to_abspath(s_info.curdir);
		printf("\r"PROMPT"%s # %s", path, cmd_buffer);
		cmd_buffer[idx] = '\0';
		printf("\r"PROMPT"%s # %s", path, cmd_buffer);
		free(path);
		break;
	case '\n':
	case '\r':
		cmd_buffer[idx] = '\0';
		idx = 0;
		putc((int)'\n', stdout);
		do_command(cmd_buffer);
		memset(cmd_buffer, 0, sizeof(cmd_buffer));
		break;
	default:
		cmd_buffer[idx++] = ch;
		putc((int)ch, stdout);
		if (idx == sizeof(cmd_buffer) - 1) {
			cmd_buffer[idx] = '\0';
			printf("\nCommand buffer is overflow: %s\n", cmd_buffer);
			idx = 0;
		}
		break;
	}

	return ECORE_CALLBACK_RENEW;
}

static void processing_line_buffer(const char *buffer)
{
	int pid;
	char slavename[256];
	char pkgname[256];
	char abi[256];
	char inst_id[4096];
	char cluster[256];
	char category[256];
	char state[10];
	int refcnt;
	int fault_count;
	int list_count;
	int loaded_inst;
	int loaded_pkg;
	double ttl;
	int secured;
	double period;
	int width;
	int height;
	int debug;
	struct node *node;
	struct package *pkginfo;
	struct instance *instinfo;
	struct slave *slaveinfo;
	int i;

	switch (s_info.cmd) {
	case PKG_LIST:
		if (sscanf(buffer, "%d %[^ ] %[^ ] %[^ ] %d %d %d", &pid, slavename, pkgname, abi, &refcnt, &fault_count, &list_count) != 7) {
			printf("Invalid format : [%s]\n", buffer);
			return;
		}

		node = node_find(s_info.curdir, pkgname);
		if (!node) {
			pkginfo = malloc(sizeof(*pkginfo));
			if (!pkginfo) {
				printf("Error: %s\n", strerror(errno));
				return;
			}

			pkginfo->pkgid = strdup("conf.file");
			if (!pkginfo->pkgid)
				printf("Error: %s\n", strerror(errno));

			pkginfo->primary = 1;

			node = node_create(s_info.curdir, pkgname, NODE_DIR);
			if (!node) {
				free(pkginfo->pkgid);
				free(pkginfo);
				printf("Failed to create a new node (%s)\n", pkgname);
				return;
			}

			node_set_data(node, pkginfo);
		} else {
			pkginfo = node_data(node);

			free(pkginfo->slavename);
			free(pkginfo->abi);

			pkginfo->slavename = NULL;
			pkginfo->abi = NULL;
		}

		pkginfo->slavename = strdup(slavename);
		if (!pkginfo->slavename)
			printf("Error: %s\n", strerror(errno));

		pkginfo->abi = strdup(abi);
		if (!pkginfo->abi)
			printf("Error: %s\n", strerror(errno));

		pkginfo->pid = pid;
		pkginfo->refcnt = refcnt;
		pkginfo->fault_count = fault_count;
		pkginfo->inst_count = list_count;
		break;
	case SLAVE_LIST:
		if (sscanf(buffer, "%d %[^ ] %[^ ] %[^ ] %d %d %d %[^ ] %d %d %lf", &pid, slavename, pkgname, abi, &secured, &refcnt, &fault_count, state, &loaded_inst, &loaded_pkg, &ttl) != 11) {
			printf("Invalid format : [%s]\n", buffer);
			return;
		}
		node = node_find(s_info.curdir, slavename);
		if (!node) {
			slaveinfo = calloc(1, sizeof(*slaveinfo));
			if (!slaveinfo) {
				printf("Error: %s\n", strerror(errno));
				return;
			}

			node = node_create(s_info.curdir, slavename, NODE_DIR);
			if (!node) {
				free(slaveinfo);
				return;
			}

			node_set_data(node, slaveinfo);
		} else {
			slaveinfo = node_data(node);
		}
		free(slaveinfo->pkgname);
		free(slaveinfo->abi);
		free(slaveinfo->state);

		slaveinfo->pkgname = strdup(pkgname);
		slaveinfo->abi = strdup(abi);
		slaveinfo->state = strdup(state);

		slaveinfo->pid = pid;
		slaveinfo->secured = secured;
		slaveinfo->refcnt = refcnt;
		slaveinfo->fault_count = fault_count;
		slaveinfo->loaded_inst = loaded_inst;
		slaveinfo->loaded_pkg = loaded_pkg;
		slaveinfo->ttl = ttl;
		break;
	case INST_LIST:
		if (sscanf(buffer, "%[^ ] %[^ ] %[^ ] %lf %[^ ] %d %d", inst_id, cluster, category, &period, state, &width, &height) != 7) {
			printf("Invalid format : [%s]\n", buffer);
			return;
		}

		for (i = strlen(inst_id); i > 0 && inst_id[i] != '/'; i--);
		i += (inst_id[i] == '/');

		node = node_find(s_info.curdir, inst_id + i);
		if (!node) {
			instinfo = calloc(1, sizeof(*instinfo));
			if (!instinfo) {
				printf("Error: %s\n", strerror(errno));
				return;
			}

			node = node_create(s_info.curdir, inst_id + i, NODE_FILE);
			if (!node) {
				free(instinfo);
				return;
			}

			node_set_data(node, instinfo);
		} else {
			instinfo = node_data(node);
		}

		free(instinfo->cluster);
		free(instinfo->category);
		free(instinfo->state);

		instinfo->cluster = strdup(cluster);
		if (!instinfo->cluster)
			printf("Error: %s\n", strerror(errno));

		instinfo->category = strdup(category);
		if (!instinfo->category)
			printf("Error: %s\n", strerror(errno));

		instinfo->state = strdup(state);
		instinfo->period = period;
		instinfo->width = width;
		instinfo->height = height;
		break;
	case TOGGLE_DEBUG:
		sscanf(buffer, "%d", &debug);
		break;
	default:
		break;
	}
}

static inline void do_line_command(void)
{
	switch (s_info.cmd) {
	case PKG_LIST:
		ls();
		break;
	case INST_LIST:
		ls();
		break;
	case SLAVE_LIST:
		ls();
		break;
	case TOGGLE_DEBUG:
		break;
	default:
		break;
	}
}

static Eina_Bool read_cb(void *data, Ecore_Fd_Handler *fd_handler)
{
	int fd;
	static char *line_buffer = NULL;
	static int line_index = 0;
	static int bufsz = 256;
	char ch;

	fd = ecore_main_fd_handler_fd_get(fd_handler);
	if (fd < 0) {
		printf("FD is not valid: %d\n", fd);
		return ECORE_CALLBACK_CANCEL;
	}

	if (read(fd, &ch, sizeof(ch)) != sizeof(ch)) {
		printf("Error: %s\n", strerror(errno));
		return ECORE_CALLBACK_CANCEL;
	}

	if (!line_buffer) {
		line_index = 0;
		line_buffer = malloc(bufsz);
		if (!line_buffer) {
			printf("Error: %s\n", strerror(errno));
			return ECORE_CALLBACK_CANCEL;
		}
	}	

	if (ch == '\n') { /* End of a line */
		line_buffer[line_index] = '\0';

		if (!strcmp(line_buffer, "EOD")) {
			do_line_command();
			s_info.cmd = NOP;
		} else {
			processing_line_buffer(line_buffer);
		}

		free(line_buffer);
		line_buffer = NULL;
		line_index = 0;
		bufsz = 256;
	} else {
		char *new_buf;

		line_buffer[line_index++] = ch;
		if (line_index == bufsz - 1) {
			bufsz += 256;
			new_buf = realloc(line_buffer, bufsz);
			if (!new_buf) {
				printf("Error: %s\n", strerror(errno));
				free(line_buffer);
				line_buffer = 0;
				line_index = 0;
				bufsz = 256;
				return ECORE_CALLBACK_CANCEL;
			}

			line_buffer = new_buf;
		}
	}

	return ECORE_CALLBACK_RENEW;
}

static int ret_cb(pid_t pid, int handle, const struct packet *packet, void *data)
{
	const char *fifo_name;
	int ret;
	char *path;

	if (packet_get(packet, "si", &fifo_name, &ret) != 2) {
		printf("Invalid packet\n");
		return -EFAULT;
	}

	if (ret != 0) {
		printf("Returns %d\n", ret);
		return ret;
	}

	printf("FIFO: %s\n", fifo_name);

	s_info.fifo_handle = open(fifo_name, O_RDONLY | O_NONBLOCK);
	if (s_info.fifo_handle < 0) {
		printf("Error: %s\n", strerror(errno));
		s_info.fifo_handle = -EINVAL;
		ecore_main_loop_quit();
		return -EINVAL;
	}

	s_info.fd_handler = ecore_main_fd_handler_add(s_info.fifo_handle, ECORE_FD_READ, read_cb, NULL, NULL, NULL);
	if (!s_info.fd_handler) {
		printf("Failed to add a fd handler\n");
		close(s_info.fifo_handle);
		s_info.fifo_handle = -EINVAL;
		ecore_main_loop_quit();
		return -EFAULT;
	}

	path = node_to_abspath(s_info.curdir);
	printf(PROMPT"%s # ", path);
	free(path);

        if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) < 0)
                printf("Error: %s\n", strerror(errno));

	s_info.in_handler = ecore_main_fd_handler_add(STDIN_FILENO, ECORE_FD_READ, input_cb, NULL, NULL, NULL);
	if (!s_info.in_handler) {
		printf("Failed to add a input handler\n");
		ecore_main_loop_quit();
		return -EFAULT;
	}

	return 0;
}

static int disconnected_cb(int handle, void *data)
{
	printf("Disconnected\n");
	ecore_main_loop_quit();
	return 0;
}

static int connected_cb(int handle, void *data)
{
	struct packet *packet;

	printf("Connected\n");

	packet = packet_create("liveinfo_hello", "d", 0.0f);
	if (!packet) {
		printf("Failed to build a packet for hello\n");
		com_core_packet_client_fini(s_info.fd);
		s_info.fd = -EINVAL;
		return -EFAULT;
	}

	s_info.fd = handle;

	if (com_core_packet_async_send(s_info.fd, packet, 0.0f, ret_cb, NULL) < 0) {
		printf("Failed to send a packet hello\n");
		packet_destroy(packet);
		com_core_packet_client_fini(s_info.fd);
		s_info.fd = -EINVAL;
		return -EFAULT;
	}

	packet_destroy(packet);
	return 0;
}

int main(int argc, char *argv[])
{
	struct termios ttystate;
	static struct method s_table[] = {
		{
			.cmd = NULL,
			.handler = NULL,
		},
	};

	ecore_init();
	g_type_init();

	com_core_add_event_callback(CONNECTOR_DISCONNECTED, disconnected_cb, NULL);
	com_core_add_event_callback(CONNECTOR_CONNECTED, connected_cb, NULL);
	livebox_service_init();

	s_info.fd = com_core_packet_client_init(SOCKET_FILE, 0, s_table);
	if (s_info.fd < 0) {
		printf("Failed to make a connection\n");
		return -EIO;
	}

	printf("Type your command on below empty line\n");

	if (tcgetattr(STDIN_FILENO, &ttystate) < 0) {
		printf("Error: %s\n", strerror(errno));
	} else {
		ttystate.c_lflag &= ~(ICANON | ECHO);
		ttystate.c_cc[VMIN] = 1;

		if (tcsetattr(STDIN_FILENO, TCSANOW, &ttystate) < 0)
			printf("Error: %s\n", strerror(errno));
	}

	if (setvbuf(stdout, (char *)NULL, _IONBF, 0) != 0)
		printf("Error: %s\n", strerror(errno));

	init_directory();

	ecore_main_loop_begin();

	fini_directory();
	livebox_service_fini();

	ttystate.c_lflag |= ICANON | ECHO;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &ttystate) < 0)
		printf("Error: %s\n", strerror(errno));

	if (s_info.fd > 0) {
		com_core_packet_client_fini(s_info.fd);
		s_info.fd = -EINVAL;
	}

	if (s_info.fd_handler) {
		ecore_main_fd_handler_del(s_info.fd_handler);
		s_info.fd_handler = NULL;
	}

	if (s_info.fifo_handle > 0) {
		close(s_info.fifo_handle);
		s_info.fifo_handle = -EINVAL;
	}

	if (s_info.in_handler) {
		ecore_main_fd_handler_del(s_info.in_handler);
		s_info.in_handler = NULL;
	}

	ecore_shutdown();
	return 0;
}

/* End of a file */
