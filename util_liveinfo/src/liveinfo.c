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

#include <Ecore.h>

#define PROMPT "liveinfo) "

static struct info {
	int fifo_handle;
	int fd;
	Ecore_Fd_Handler *fd_handler;
	Ecore_Fd_Handler *in_handler;
	int input_fd;
} s_info = {
	.fifo_handle = -EINVAL,
	.fd = -EINVAL,
	.fd_handler = NULL,
	.in_handler = NULL,
	.input_fd = -1,
};

static void send_slave_list(void)
{
	struct packet *packet;

	packet = packet_create_noack("slave_list", "d", 0.0f);
	if (!packet) {
		fprintf(stderr, "Failed to create a packet\n");
		return;
	}

	com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);

	printf("----------------------------------------------------------------------[Slave List]------------------------------------------------------------------------------\n");
	printf("    pid          slave name                     package name                   abi     secured   refcnt   fault           state           inst   pkg     ttl    \n");
	printf("----------------------------------------------------------------------------------------------------------------------------------------------------------------\n");
}

static void send_pkg_list(void)
{
	struct packet *packet;

	packet = packet_create_noack("pkg_list", "d", 0.0f);
	if (!packet) {
		fprintf(stderr, "Failed to create a packet\n");
		return;
	}

	com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);

	printf("+----------------------------------------------[Package List]------------------------------------------------+\n");
	printf("    pid          slave name                     package name                   abi     refcnt   fault   inst  \n");
	printf("+------------------------------------------------------------------------------------------------------------+\n");
}

static void send_toggle_debug(void)
{
	struct packet *packet;

	packet = packet_create_noack("toggle_debug", "d", 0.0f);
	if (!packet) {
		fprintf(stderr, "Failed to create a packet\n");
		return;
	}

	com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
}

static void send_slave_load(pid_t pid)
{
	struct packet *packet;

	packet = packet_create_noack("slave_load", "i", pid);
	if (!packet) {
		fprintf(stderr, "Failed to create a packet\n");
		return;
	}

	com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);
}

static void send_inst_list(const char *pkgname)
{
	struct packet *packet;

	packet = packet_create_noack("inst_list", "s", pkgname);
	if (!packet) {
		fprintf(stderr, "Failed to create a packet\n");
		return;
	}

	com_core_packet_send_only(s_info.fd, packet);
	packet_destroy(packet);

	printf("-----------------------------------------------[Instance List]---------------------------------------\n");
	printf("         ID         |      Cluster ID    |   Sub cluster ID   | Period | Visibility | Width | Height \n");
	printf("-----------------------------------------------------------------------------------------------------\n");
}

static inline void help(void)
{
	printf("liveinfo - Livebox utility\n");
	printf("------------------------------ [Command list] ------------------------------\n");
	printf("[33mpkg_list[0m - Display the installed package list\n");
	printf("[33mslave_list[0m - Display the slave list\n");
	printf("[33minst_list[0m [37mLIVEBOX_PKGNAME[0m - Display the instance list of this LIVEBOX_PKGNAME\n");
	printf("[33mslave_load[0m [37mSLAVE_PID[0m - Display the loaded livebox instance list on the given slave\n");
	printf("[33mtoggle_debug[0m - Enable/Disable debug mode\n");
	printf("[32mexit - [0m\n");
	printf("[32mquit - [0m\n");
	printf("----------------------------------------------------------------------------\n");
}

static inline void do_command(const char *cmd)
{
	char command[256];
	char argument[256] = { '\0', };

	if (sscanf(cmd, "%255[^ ] %255s", command, argument) == 2)
		cmd = command;

	if (!strcasecmp(cmd, "inst_list") && *argument) {
		send_inst_list(argument);
	} else if (!strcasecmp(cmd, "slave_load") && *argument) {
		pid_t pid;

		if (sscanf(argument, "%d", &pid) == 1)
			send_slave_load(pid);
		else
			goto errout;
	} else if (!strcasecmp(cmd, "pkg_list")) {
		send_pkg_list();
	} else if (!strcasecmp(cmd, "slave_list")) {
		send_slave_list();
	} else if (!strcasecmp(cmd, "exit") || !strcasecmp(cmd, "quit")) {
		ecore_main_loop_quit();
	} else if (!strcasecmp(cmd, "toggle_debug")) {
		send_toggle_debug();
	} else if (!strcasecmp(cmd, "help")) {
		goto errout;
	} else {
		printf("Unknown command - \"help\"\n");
		fputs(PROMPT, stdout);
	}

	return;

errout:
	help();
	fputs(PROMPT, stdout);
}

static Eina_Bool input_cb(void *data, Ecore_Fd_Handler *fd_handler)
{
	static int idx = 0;
	static char cmd_buffer[256];
	char ch;
	int ret;
	int fd;

	fd = ecore_main_fd_handler_fd_get(fd_handler);

	/*!
	 * \note
	 * Using this routine, we can implement the command recommend algorithm.
	 * When a few more characters are matched with history of command, we can show it to user
	 * Then the user will choose one or write new command
	 */

	/* Silly.. Silly */
	ret = read(fd, &ch, 1);
	if (ret != 1 || ret < 0) {
		fprintf(stderr, "Failed to get a byte: %s\n", strerror(errno));
		return ECORE_CALLBACK_RENEW;
	}

	switch (ch) {
	case 0x08: /* BKSP */
		cmd_buffer[idx] = '\0';
		if (idx > 0)
			idx--;
		cmd_buffer[idx] = ' ';
		printf("\r"PROMPT"%s", cmd_buffer); /* Cleare the last bytes */
		cmd_buffer[idx] = '\0';
		printf("\r"PROMPT"%s", cmd_buffer); /* Cleare the last bytes */
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

static Eina_Bool read_cb(void *data, Ecore_Fd_Handler *fd_handler)
{
	int fd;
	static const char *eod = "EOD\n";
	char ch;

	fd = ecore_main_fd_handler_fd_get(fd_handler);
	if (fd < 0) {
		fprintf(stderr, "FD is not valid: %d\n", fd);
		return ECORE_CALLBACK_RENEW;
	}

	read(fd, &ch, sizeof(ch));
	if (ch == *eod)
		eod++;
	else
		eod = "EOD\n";

	putc(ch, stdout);

	if (*eod == '\0') {
		fputs(PROMPT, stdout);
		eod = "EOD\n";
	}

	return ECORE_CALLBACK_RENEW;
}

static int ret_cb(pid_t pid, int handle, const struct packet *packet, void *data)
{
	const char *fifo_name;
	int ret;

	if (packet_get(packet, "si", &fifo_name, &ret) != 2) {
		fprintf(stderr, "Invalid packet\n");
		return -EFAULT;
	}

	if (ret != 0) {
		fprintf(stderr, "Returns %d\n", ret);
		return ret;
	}

	printf("FIFO: %s\n", fifo_name);

	s_info.fifo_handle = open(fifo_name, O_RDONLY | O_NONBLOCK);
	if (s_info.fifo_handle < 0) {
		fprintf(stderr, "Error: %s\n", strerror(errno));
		s_info.fifo_handle = -EINVAL;
		ecore_main_loop_quit();
		return -EINVAL;
	}

	s_info.fd_handler = ecore_main_fd_handler_add(s_info.fifo_handle, ECORE_FD_READ, read_cb, NULL, NULL, NULL);
	if (!s_info.fd_handler) {
		fprintf(stderr, "Failed to add a fd handler\n");
		close(s_info.fifo_handle);
		s_info.fifo_handle = -EINVAL;
		ecore_main_loop_quit();
		return -EFAULT;
	}

	fputs(PROMPT, stdout);
        if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) < 0)
                fprintf(stderr, "Error: %s\n", strerror(errno));

	s_info.in_handler = ecore_main_fd_handler_add(STDIN_FILENO, ECORE_FD_READ, input_cb, NULL, NULL, NULL);
	if (!s_info.in_handler) {
		fprintf(stderr, "Failed to add a input handler\n");
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
		fprintf(stderr, "Failed to build a packet for hello\n");
		com_core_packet_client_fini(s_info.fd);
		s_info.fd = -EINVAL;
		return -EFAULT;
	}

	s_info.fd = handle;

	if (com_core_packet_async_send(s_info.fd, packet, 0.0f, ret_cb, NULL) < 0) {
		fprintf(stderr, "Failed to send a packet hello\n");
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

	s_info.fd = com_core_packet_client_init(SOCKET_FILE, 0, s_table);
	if (s_info.fd < 0) {
		fprintf(stderr, "Failed to make a connection\n");
		return -EIO;
	}

	printf("Type your command on below empty line\n");

	if (tcgetattr(STDIN_FILENO, &ttystate) < 0) {
		fprintf(stderr, "Error: %s\n", strerror(errno));
	} else {
		ttystate.c_lflag &= ~(ICANON | ECHO);
		ttystate.c_cc[VMIN] = 1;

		if (tcsetattr(STDIN_FILENO, TCSANOW, &ttystate) < 0)
			fprintf(stderr, "Error: %s\n", strerror(errno));
	}

	if (setvbuf(stdout, (char *)NULL, _IONBF, 0) != 0)
		fprintf(stderr, "Error: %s\n", strerror(errno));

	ecore_main_loop_begin();

	ttystate.c_lflag |= ICANON | ECHO;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &ttystate) < 0)
		fprintf(stderr, "Error: %s\n", strerror(errno));

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
