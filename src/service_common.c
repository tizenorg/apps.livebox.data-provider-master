/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.0 (the "License");
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
#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <secure_socket.h>
#include <packet.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <dlog.h>
#include <Eina.h>

#include "service_common.h"
#include "util.h"
#include "debug.h"
#include "conf.h"

#define EVT_CH	'e'
#define EVT_END_CH	'x'

#define CRITICAL_SECTION_BEGIN(handle) \
do { \
	int ret; \
	ret = pthread_mutex_lock(handle); \
	if (ret != 0) \
		ErrPrint("Failed to lock: %s\n", strerror(ret)); \
} while (0)

#define CRITICAL_SECTION_END(handle) \
do { \
	int ret; \
	ret = pthread_mutex_unlock(handle); \
	if (ret != 0) \
		ErrPrint("Failed to unlock: %s\n", strerror(ret)); \
} while (0)


int errno;

/*!
 * \note
 * Server information and global (only in this file-scope) variables are defined
 */
#define EVT_READ 0
#define EVT_WRITE 1
#define EVT_MAX 2

struct service_context {
	pthread_t thid; /*!< Server thread Id */
	int fd; /*!< Server socket handle */

	pthread_t service_thid;

	Eina_List *tcb_list; /*!< TCB list, list of every thread for client connections */
	pthread_mutex_t tcb_list_lock; /*!< tcb_list has to be handled safely */

	Eina_List *packet_list;
	pthread_mutex_t packet_list_lock;
	int evt_pipe[EVT_MAX];

	int (*service_thread_main)(struct tcb *tcb, struct packet *packet, void *data);
	void *service_thread_data;
};

struct packet_info {
	struct tcb *tcb;
	struct packet *packet;
};

/*!
 * \note
 * Thread Control Block
 * - The main server will create a thread for every client connections.
 *   When a new client is comming to us, this TCB block will be allocated and initialized.
 */
struct tcb { /* Thread controll block */
	struct service_context *svc_ctx;
	pthread_t thid; /*!< Thread Id */
	int fd; /*!< Connection handle */
	enum tcb_type type;
};

/*!
 * \note
 * Called from Client Thread
 */
static inline int tcb_destroy(struct service_context *svc_ctx, struct tcb *tcb)
{
	void *ret;
	int status;

	CRITICAL_SECTION_BEGIN(&svc_ctx->tcb_list_lock);
	svc_ctx->tcb_list = eina_list_remove(svc_ctx->tcb_list, tcb);
	CRITICAL_SECTION_END(&svc_ctx->tcb_list_lock);

	/*!
	 * ASSERT(tcb->fd >= 0);
	 */
	secure_socket_destroy_handle(tcb->fd);

	status = pthread_join(tcb->thid, &ret);
	if (status != 0)
		ErrPrint("Unable to join a thread: %s\n", strerror(status));
	/*!
	 * \NOTE
	 * Waiting termination client thread
	 */
	free(tcb);

	return 0;
}

/*!
 * Do service for clients
 * Routing packets to destination processes.
 */
static void *client_packet_pump_main(void *data)
{
	struct tcb *tcb = data;
	struct service_context *svc_ctx = tcb->svc_ctx;
	struct packet *packet;
	fd_set set;
	char *ptr;
	int size;
	int packet_offset;
	int recv_offset;
	int pid;
	int ret;
	char evt_ch = EVT_CH;
	enum {
		RECV_INIT,
		RECV_HEADER,
		RECV_PAYLOAD,
		RECV_DONE,
	} recv_state;
	struct packet_info *packet_info;

	ret = 0;
	recv_state = RECV_INIT;
	/*!
	 * \note
	 * To escape from the switch statement, we use this ret value
	 */
	while (ret == 0) {
		FD_ZERO(&set);
		FD_SET(tcb->fd, &set);
		ret = select(tcb->fd + 1, &set, NULL, NULL, NULL);
		if (ret < 0) {
			ret = -errno;
			if (errno == EINTR) {
				DbgPrint("INTERRUPTED\n");
				ret = 0;
				continue;
			}
			ErrPrint("Error: %s\n",strerror(errno));
			break;
		} else if (ret == 0) {
			ErrPrint("Timeout\n");
			ret = -ETIMEDOUT;
			break;
		}

		if (FD_ISSET(tcb->fd, &set)) {
			ErrPrint("Unexpected handler is toggled\n");
			ret = -EINVAL;
			break;
		}
		
		/*!
		 * \TODO
		 * Service!!! Receive packet & route packet
		 */
		switch (recv_state) {
		case RECV_INIT:
			size = packet_header_size();
			packet_offset = 0;
			recv_offset = 0;
			packet = NULL;
			ptr = malloc(size);
			if (!ptr) {
				ErrPrint("Heap: %s\n", strerror(errno));
				ret = -ENOMEM;
				break;
			}
			recv_state = RECV_HEADER;
			/* Go through, don't break from here */
		case RECV_HEADER:
			ret = secure_socket_recv(tcb->fd, ptr, size - recv_offset, &pid);
			if (ret <= 0) {
				if (ret == 0)
					ret = -ECANCELED;
				free(ptr);
				break;
			}

			recv_offset += ret;
			ret = 0;

			if (recv_offset == size) {
				packet = packet_build(packet, packet_offset, ptr, size);
				free(ptr);
				if (!packet) {
					ret = -EFAULT;
					break;
				}

				packet_offset += recv_offset;

				recv_state = RECV_PAYLOAD;
				recv_offset = 0;
				size = packet_payload_size(packet);

				ptr = malloc(size);
				if (!ptr) {
					ErrPrint("Heap: %s\n", strerror(errno));
					ret = -ENOMEM;
				}
			}
			break;
		case RECV_PAYLOAD:
			ret = secure_socket_recv(tcb->fd, ptr, size - recv_offset, &pid);
			if (ret <= 0) {
				if (ret == 0)
					ret = -ECANCELED;
				free(ptr);
				break;
			}

			recv_offset += ret;
			ret = 0;

			if (recv_offset == size) {
				packet = packet_build(packet, packet_offset, ptr, size);
				free(ptr);
				if (!packet) {
					ret = -EFAULT;
					break;
				}

				packet_offset += recv_offset;

				recv_state = RECV_DONE;
				recv_offset = 0;
			}
			break;
		case RECV_DONE:
			/*!
			 * Push this packet to the packet list with TCB
			 * Then the service main function will get this.
			 */
			packet_info = malloc(sizeof(*packet_info));
			if (!packet_info) {
				ret = -errno;
				ErrPrint("Heap: %s\n", strerror(errno));
				packet_destroy(packet);
				break;
			}

			packet_info->packet = packet;
			packet_info->tcb = tcb;

			CRITICAL_SECTION_BEGIN(&svc_ctx->packet_list_lock);
			svc_ctx->packet_list = eina_list_append(svc_ctx->packet_list, packet_info);
			CRITICAL_SECTION_END(&svc_ctx->packet_list_lock);

			if (write(svc_ctx->evt_pipe[EVT_WRITE], &evt_ch, sizeof(evt_ch)) != sizeof(evt_ch)) {
				ret = -errno;
				ErrPrint("Unable to write a pipe\n");
				CRITICAL_SECTION_BEGIN(&svc_ctx->packet_list_lock);
				svc_ctx->packet_list = eina_list_remove(svc_ctx->packet_list, packet_info);
				CRITICAL_SECTION_END(&svc_ctx->packet_list_lock);

				packet_destroy(packet);
				free(packet_info);
				break;
			}
			DbgPrint("Packet received: %d bytes\n", packet_offset);

			recv_state = RECV_INIT;
			break;
		default:
			/* Dead code */
			break;
		}
	}

	tcb_destroy(svc_ctx, tcb);
	return (void *)ret;
}

/*!
 * \note
 * Called from Server Main Thread
 */
static inline struct tcb *tcb_create(struct service_context *svc_ctx, int fd)
{
	struct tcb *tcb;
	int status;

	tcb = malloc(sizeof(*tcb));
	if (!tcb) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	tcb->fd = fd;
	tcb->svc_ctx = svc_ctx;

	status = pthread_create(&tcb->thid, NULL, client_packet_pump_main, tcb);
	if (status != 0) {
		ErrPrint("Unable to create a new thread: %s\n", strerror(status));
		free(tcb);
		return NULL;
	}

	CRITICAL_SECTION_BEGIN(&svc_ctx->tcb_list_lock);
	svc_ctx->tcb_list = eina_list_append(svc_ctx->tcb_list, tcb);
	CRITICAL_SECTION_END(&svc_ctx->tcb_list_lock);

	return tcb;
}

/*!
 * \note
 * Called from Main Thread
 */
static inline void tcb_teminate_all(struct service_context *svc_ctx)
{
	struct tcb *tcb;
	void *ret;
	int status;

	/*!
	 * We don't need to make critical section on here.
	 * If we call this after terminate the server thread first.
	 * Then there is no other thread to access tcb_list.
	 */
	CRITICAL_SECTION_BEGIN(&svc_ctx->tcb_list_lock);
	EINA_LIST_FREE(svc_ctx->tcb_list, tcb) {
		/*!
		 * ASSERT(tcb->fd >= 0);
		 */
		secure_socket_destroy_handle(tcb->fd);

		status = pthread_join(tcb->thid, &ret);
		if (status != 0)
			ErrPrint("Unable to join a thread: %s\n", strerror(status));

		free(tcb);
	}
	CRITICAL_SECTION_END(&svc_ctx->tcb_list_lock);
}

/*!
 * \NOTE
 * Packet consuming thread. service thread.
 */
static void *service_main(void *data)
{
	struct service_context *svc_ctx = data;
	fd_set set;
	int ret;
	char evt_ch;
	struct packet_info *packet_info;

	while (1) {
		FD_ZERO(&set);
		FD_SET(svc_ctx->evt_pipe[EVT_READ], &set);
		ret = select(svc_ctx->evt_pipe[EVT_READ] + 1, &set, NULL, NULL, NULL);
		if (ret < 0) {
			ret = -errno;
			if (errno == EINTR) {
				DbgPrint("INTERRUPTED\n");
				continue;
			}
			ErrPrint("Error: %s\n",strerror(errno));
			break;
		} else if (ret == 0) {
			ErrPrint("Timeout\n");
			ret = -ETIMEDOUT;
			break;
		}

		if (FD_ISSET(svc_ctx->evt_pipe[EVT_READ], &set)) {
			ErrPrint("Unexpected handler is toggled\n");
			ret = -EINVAL;
			break;
		}

		if (read(svc_ctx->evt_pipe[EVT_READ], &evt_ch, sizeof(evt_ch)) != sizeof(evt_ch)) {
			ErrPrint("Unable to read pipe: %s\n", strerror(errno));
			ret = -EIO;
			break;
		}

		if (evt_ch == EVT_END_CH) {
			ErrPrint("Thread is terminated\n");
			ret = 0;
			break;
		}

		CRITICAL_SECTION_BEGIN(&svc_ctx->packet_list_lock);
		packet_info = eina_list_nth(svc_ctx->packet_list, 0);
		svc_ctx->packet_list = eina_list_remove(svc_ctx->packet_list, packet_info);
		CRITICAL_SECTION_END(&svc_ctx->packet_list_lock);

		ret = svc_ctx->service_thread_main(packet_info->tcb, packet_info->packet, svc_ctx->service_thread_data);
		if (ret < 0)
			ErrPrint("Service thread returns: %d\n", ret);

		packet_destroy(packet_info->packet);
		free(packet_info);
	}

	return (void *)ret;
}

/*!
 * Accept new client connections
 * And create a new thread for service.
 */
static void *server_main(void *data)
{
	struct service_context *svc_ctx = data;
	fd_set set;
	int ret;
	int client_fd;
	struct tcb *tcb;

	while (1) {
		FD_ZERO(&set);
		FD_SET(svc_ctx->fd, &set);
		ret = select(svc_ctx->fd + 1, &set, NULL, NULL, NULL);
		if (ret < 0) {
			ret = -errno;
			if (errno == EINTR) {
				DbgPrint("INTERRUPTED\n");
				continue;
			}
			ErrPrint("Error: %s\n", strerror(errno));
			break;
		} else if (ret == 0) {
			ErrPrint("Timeout\n");
			ret = -ETIMEDOUT;
			break;
		}

		if (FD_ISSET(svc_ctx->fd, &set)) {
			ErrPrint("Unexpected handler is toggled\n");
			ret = -EINVAL;
			break;
		}

		client_fd = secure_socket_get_connection_handle(svc_ctx->fd);
		DbgPrint("New client connection arrived (%d)\n", client_fd);
		if (client_fd < 0) {
			ErrPrint("Failed to establish the client connection\n");
			ret = -EFAULT;
			break;
		}

		tcb = tcb_create(svc_ctx, client_fd);
		if (!tcb)
			secure_socket_destroy_handle(client_fd);
	}

	return (void *)ret;
}

HAPI struct service_context *service_common_create(const char *addr, int (*service_thread_main)(struct tcb *tcb, struct packet *packet, void *data), void *data)
{
	int status;
	struct service_context *svc_ctx;

	if (!service_thread_main || !addr) {
		ErrPrint("Invalid argument\n");
		return NULL;
	}

	svc_ctx = malloc(sizeof(*svc_ctx));
	if (!svc_ctx) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	svc_ctx->fd = secure_socket_create_server(addr);
	if (svc_ctx->fd < 0) {
		free(svc_ctx);
		return NULL;
	}

	svc_ctx->service_thread_main = service_thread_main;
	svc_ctx->service_thread_data = data;

	if (fcntl(svc_ctx->fd, F_SETFD, FD_CLOEXEC) < 0)
		ErrPrint("fcntl: %s\n", strerror(errno));

	if (fcntl(svc_ctx->fd, F_SETFL, O_NONBLOCK) < 0)
		ErrPrint("fcntl: %s\n", strerror(errno));

	if (pipe2(svc_ctx->evt_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		ErrPrint("pipe: %d\n", strerror(errno));
		secure_socket_destroy_handle(svc_ctx->fd);
		free(svc_ctx);
		return NULL;
	}

	status = pthread_mutex_init(&svc_ctx->packet_list_lock, NULL);
	if (status != 0) {
		ErrPrint("Unable to create a mutex: %s\n", strerror(status));
		status = close(svc_ctx->evt_pipe[EVT_READ]);
		if (status < 0)
			ErrPrint("Error: %s\n", strerror(errno));
		status = close(svc_ctx->evt_pipe[EVT_WRITE]);
		if (status < 0)
			ErrPrint("Error: %s\n", strerror(errno));
		secure_socket_destroy_handle(svc_ctx->fd);
		free(svc_ctx);
		return NULL;
	}

	status = pthread_mutex_init(&svc_ctx->tcb_list_lock, NULL);
	if (status != 0) {
		ErrPrint("Unable to initiate the mutex: %s\n", strerror(status));
		status = pthread_mutex_destroy(&svc_ctx->packet_list_lock);
		if (status != 0)
			ErrPrint("Error: %s\n", strerror(status));
		status = close(svc_ctx->evt_pipe[EVT_READ]);
		if (status < 0)
			ErrPrint("Error: %s\n", strerror(errno));
		status = close(svc_ctx->evt_pipe[EVT_WRITE]);
		if (status < 0)
			ErrPrint("Error: %s\n", strerror(errno));
		secure_socket_destroy_handle(svc_ctx->fd);
		free(svc_ctx);
		return NULL;
	}

	status = pthread_create(&svc_ctx->thid, NULL, server_main, svc_ctx);
	if (status != 0) {
		ErrPrint("Unable to create a thread for shortcut service: %s\n", strerror(status));
		status = pthread_mutex_destroy(&svc_ctx->tcb_list_lock);
		if (status != 0)
			ErrPrint("Error: %s\n", strerror(status));
		status = pthread_mutex_destroy(&svc_ctx->packet_list_lock);
		if (status != 0)
			ErrPrint("Error: %s\n", strerror(status));
		status = close(svc_ctx->evt_pipe[EVT_READ]);
		if (status != 0)
			ErrPrint("Error: %s\n", strerror(status));
		status = close(svc_ctx->evt_pipe[EVT_WRITE]);
		if (status != 0)
			ErrPrint("Error: %s\n", strerror(status));
		secure_socket_destroy_handle(svc_ctx->fd);
		free(svc_ctx);
		return NULL;
	}

	status = pthread_create(&svc_ctx->service_thid, NULL, service_main, svc_ctx);
	if (status != 0) {
		void *ret;
		ErrPrint("Unable to create a thread for shortcut service: %s\n", strerror(status));

		secure_socket_destroy_handle(svc_ctx->fd);

		status = pthread_join(svc_ctx->thid, &ret);
		if (status != 0)
			ErrPrint("Error: %s\n", strerror(status));

		status = pthread_mutex_destroy(&svc_ctx->tcb_list_lock);
		if (status != 0)
			ErrPrint("Error: %s\n", strerror(status));
		status = pthread_mutex_destroy(&svc_ctx->packet_list_lock);
		if (status != 0)
			ErrPrint("Error: %s\n", strerror(status));
		status = close(svc_ctx->evt_pipe[EVT_READ]);
		if (status < 0)
			ErrPrint("Error: %s\n", strerror(errno));
		status = close(svc_ctx->evt_pipe[EVT_WRITE]);
		if (status < 0)
			ErrPrint("Error: %s\n", strerror(errno));
		secure_socket_destroy_handle(svc_ctx->fd);
		free(svc_ctx);
		return NULL;
	}

	return svc_ctx;
}

HAPI int service_common_destroy(struct service_context *svc_ctx)
{
	int status;
	void *ret;
	char evt_ch = EVT_END_CH;

	if (!svc_ctx)
		return -EINVAL;

	/*!
	 * \note
	 * Terminate server thread
	 */
	secure_socket_destroy_handle(svc_ctx->fd);
	status = pthread_join(svc_ctx->thid, &ret);
	if (status != 0)
		ErrPrint("Join: %s\n", strerror(status));
	/*!
	 * \note
	 * Terminate all client threads.
	 */
	tcb_teminate_all(svc_ctx);

	/* Emit a finish event */
	if (write(svc_ctx->evt_pipe[EVT_WRITE], &evt_ch, sizeof(evt_ch)) == sizeof(evt_ch))
		ErrPrint("write: %s\n", strerror(errno));

	/* Waiting */
	status = pthread_join(svc_ctx->service_thid, &ret);
	if (status != 0)
		ErrPrint("Join: %s\n", strerror(status));

	status = pthread_mutex_destroy(&svc_ctx->tcb_list_lock);
	if (status != 0)
		ErrPrint("Unable to destroy a mutex: %s\n", strerror(status));

	status = pthread_mutex_destroy(&svc_ctx->packet_list_lock);
	if (status != 0)
		ErrPrint("Unable to destroy a mutex: %s\n", strerror(status));

	status = close(svc_ctx->evt_pipe[EVT_WRITE]);
	status = close(svc_ctx->evt_pipe[EVT_READ]);
	free(svc_ctx);
	return 0;
}

HAPI int tcb_fd(struct tcb *tcb)
{
	if (!tcb)
		return -EINVAL;

	return tcb->fd;
}

HAPI int tcb_client_type(struct tcb *tcb)
{
	if (!tcb)
		return -EINVAL;

	return tcb->type;
}

HAPI int tcb_client_type_set(struct tcb *tcb, enum tcb_type type)
{
	if (!tcb)
		return -EINVAL;

	tcb->type = type;
	return 0;
}

HAPI struct service_context *tcb_svc_ctx(struct tcb *tcb)
{
	if (!tcb)
		return NULL;

	return tcb->svc_ctx;
}

HAPI int service_common_unicast_packet(struct tcb *tcb, struct packet *packet)
{
	if (!tcb || !packet)
		return -EINVAL;

	return secure_socket_send(tcb->fd, (void *)packet_data(packet), packet_size(packet));
}

HAPI int service_common_multicast_packet(struct tcb *tcb, struct packet *packet, int type)
{
	Eina_List *l;
	struct tcb *target;
	struct service_context *svc_ctx;
	int ret;

	if (!tcb || !packet)
		return -EINVAL;

	svc_ctx = tcb->svc_ctx;

	CRITICAL_SECTION_BEGIN(&svc_ctx->tcb_list_lock);
	EINA_LIST_FOREACH(svc_ctx->tcb_list, l, target) {
		if (target == tcb || target->type != type)
			continue;

		ret = secure_socket_send(target->fd, (void *)packet_data(packet), packet_size(packet));
		if (ret < 0)
			ErrPrint("Failed to send packet: %d\n", ret);
	}
	CRITICAL_SECTION_END(&svc_ctx->tcb_list_lock);

	return 0;
}

/* End of a file */
