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
#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <secure_socket.h>
#include <packet.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include <dlog.h>
#include <Eina.h>
#include <com-core.h>

#include "service_common.h"
#include "util.h"
#include "debug.h"
#include "conf.h"

#define EVT_CH		'e'
#define EVT_END_CH	'x'
#define DEFAULT_TIMEOUT	2.0f

int errno;

struct service_event_item {
	enum {
		SERVICE_EVENT_TIMER
	} type;

	union {
		struct {
			int fd;
		} timer;
	} info;

	int (*event_cb)(struct service_context *svc_cx, void *data);
	void *cbdata;
};

/*!
 * \note
 * Server information and global (only in this file-scope) variables are defined
 */
struct service_context {
	pthread_t server_thid; /*!< Server thread Id */
	int fd; /*!< Server socket handle */

	Eina_List *tcb_list; /*!< TCB list, list of every thread for client connections */
	pthread_mutex_t tcb_list_lock;

	Eina_List *packet_list;
	pthread_mutex_t packet_list_lock;
	int evt_pipe[PIPE_MAX];
	int tcb_pipe[PIPE_MAX];

	int (*service_thread_main)(struct tcb *tcb, struct packet *packet, void *data);
	void *service_thread_data;

	Eina_List *event_list;
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
	int ctrl_pipe[PIPE_MAX];
};

/*!
 * Do services for clients
 * Routing packets to destination processes.
 * CLIENT THREAD
 */
static void *client_packet_pump_main(void *data)
{
	struct tcb *tcb = data;
	struct service_context *svc_ctx = tcb->svc_ctx;
	struct packet *packet = NULL;
	fd_set set;
	char *ptr = NULL;
	int size = 0;
	int packet_offset = 0;
	int recv_offset = 0;
	int pid;
	long ret;
	int fd;
	char evt_ch = EVT_CH;
	enum {
		RECV_INIT,
		RECV_HEADER,
		RECV_PAYLOAD,
		RECV_DONE,
	} recv_state;
	struct packet_info *packet_info;
	Eina_List *l;

	ret = 0;
	recv_state = RECV_INIT;
	/*!
	 * \note
	 * To escape from the switch statement, we use this ret value
	 */
	while (ret == 0) {
		FD_ZERO(&set);
		FD_SET(tcb->fd, &set);
		FD_SET(tcb->ctrl_pipe[PIPE_READ], &set);
		fd = tcb->fd > tcb->ctrl_pipe[PIPE_READ] ? tcb->fd : tcb->ctrl_pipe[PIPE_READ];
		ret = select(fd + 1, &set, NULL, NULL, NULL);
		if (ret < 0) {
			ret = -errno;
			if (errno == EINTR) {
				ErrPrint("INTERRUPTED\n");
				ret = 0;
				continue;
			}
			ErrPrint("Error: %s\n", strerror(errno));
			DbgFree(ptr);
			ptr = NULL;
			break;
		} else if (ret == 0) {
			ErrPrint("Timeout\n");
			ret = -ETIMEDOUT;
			DbgFree(ptr);
			ptr = NULL;
			break;
		}

		if (FD_ISSET(tcb->ctrl_pipe[PIPE_READ], &set)) {
			DbgPrint("Thread is canceled\n");
			ret = -ECANCELED;
			DbgFree(ptr);
			ptr = NULL;
			break;
		}

		if (!FD_ISSET(tcb->fd, &set)) {
			ErrPrint("Unexpected handler is toggled\n");
			ret = -EINVAL;
			DbgFree(ptr);
			ptr = NULL;
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
				if (ret == 0) {
					ret = -ECANCELED;
				}
				DbgFree(ptr);
				ptr = NULL;
				break;
			}

			recv_offset += ret;
			ret = 0;

			if (recv_offset == size) {
				packet = packet_build(packet, packet_offset, ptr, size);
				DbgFree(ptr);
				ptr = NULL;
				if (!packet) {
					ret = -EFAULT;
					break;
				}

				packet_offset += recv_offset;

				size = packet_payload_size(packet);
				if (size <= 0) {
					recv_state = RECV_DONE;
					recv_offset = 0;
					break;
				}

				recv_state = RECV_PAYLOAD;
				recv_offset = 0;

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
				if (ret == 0) {
					ret = -ECANCELED;
				}
				DbgFree(ptr);
				ptr = NULL;
				break;
			}

			recv_offset += ret;
			ret = 0;

			if (recv_offset == size) {
				packet = packet_build(packet, packet_offset, ptr, size);
				DbgFree(ptr);
				ptr = NULL;
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
		default:
			/* Dead code */
			break;
		}

		if (recv_state == RECV_DONE) {
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

			if (write(svc_ctx->evt_pipe[PIPE_WRITE], &evt_ch, sizeof(evt_ch)) != sizeof(evt_ch)) {
				ret = -errno;
				ErrPrint("Unable to write a pipe: %s\n", strerror(errno));
				CRITICAL_SECTION_BEGIN(&svc_ctx->packet_list_lock);
				svc_ctx->packet_list = eina_list_remove(svc_ctx->packet_list, packet_info);
				CRITICAL_SECTION_END(&svc_ctx->packet_list_lock);

				packet_destroy(packet);
				DbgFree(packet_info);
				ErrPrint("Terminate thread: %p\n", tcb);
				break;
			} else {
				DbgPrint("Packet received: %d bytes\n", packet_offset);
				recv_state = RECV_INIT;
			}

			/* Take a breathe */
			pthread_yield();
		}
	}

	CRITICAL_SECTION_BEGIN(&svc_ctx->packet_list_lock);
	EINA_LIST_FOREACH(svc_ctx->packet_list, l, packet_info) {
		if (packet_info->tcb == tcb) {
			DbgPrint("Reset ptr of the TCB[%p] in the list of packet info\n", tcb);
			packet_info->tcb = NULL;
		}
	}
	CRITICAL_SECTION_END(&svc_ctx->packet_list_lock);

	/*!
	 * \note
	 * Emit a signal to collect this TCB from the SERVER THREAD.
	 */
	if (write(svc_ctx->tcb_pipe[PIPE_WRITE], &tcb, sizeof(tcb)) != sizeof(tcb)) {
		ErrPrint("Unable to write pipe: %s\n", strerror(errno));
	}

	return (void *)ret;
}

/*!
 * \note
 * SERVER THREAD
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

	if (pipe2(tcb->ctrl_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		ErrPrint("pipe2: %s\n", strerror(errno));
		DbgFree(tcb);
		return NULL;
	}

	tcb->fd = fd;
	tcb->svc_ctx = svc_ctx;
	tcb->type = TCB_CLIENT_TYPE_APP;

	DbgPrint("Create a new service thread [%d]\n", fd);
	status = pthread_create(&tcb->thid, NULL, client_packet_pump_main, tcb);
	if (status != 0) {
		ErrPrint("Unable to create a new thread: %s\n", strerror(status));
		CLOSE_PIPE(tcb->ctrl_pipe);
		DbgFree(tcb);
		return NULL;
	}

	CRITICAL_SECTION_BEGIN(&svc_ctx->tcb_list_lock);
	svc_ctx->tcb_list = eina_list_append(svc_ctx->tcb_list, tcb);
	CRITICAL_SECTION_END(&svc_ctx->tcb_list_lock);
	return tcb;
}

/*!
 * \note
 * SERVER THREAD
 */
static inline void tcb_teminate_all(struct service_context *svc_ctx)
{
	struct tcb *tcb;
	void *ret;
	int status;
	char ch = EVT_END_CH;

	/*!
	 * We don't need to make critical section on here.
	 * If we call this after terminate the server thread first.
	 * Then there is no other thread to access tcb_list.
	 */
	EINA_LIST_FREE(svc_ctx->tcb_list, tcb) {
		/*!
		 * ASSERT(tcb->fd >= 0);
		 */
		if (write(tcb->ctrl_pipe[PIPE_WRITE], &ch, sizeof(ch)) != sizeof(ch)) {
			ErrPrint("write: %s\n", strerror(errno));
		}

		status = pthread_join(tcb->thid, &ret);
		if (status != 0) {
			ErrPrint("Unable to join a thread: %s\n", strerror(status));
		} else {
			DbgPrint("Thread returns: %p\n", ret);
		}

		secure_socket_destroy_handle(tcb->fd);

		CLOSE_PIPE(tcb->ctrl_pipe);
		DbgFree(tcb);
	}
}

/*!
 * \note
 * SERVER THREAD
 */
static inline void tcb_destroy(struct service_context *svc_ctx, struct tcb *tcb)
{
	void *ret;
	int status;
	char ch = EVT_END_CH;

	CRITICAL_SECTION_BEGIN(&svc_ctx->tcb_list_lock);
	svc_ctx->tcb_list = eina_list_remove(svc_ctx->tcb_list, tcb);
	CRITICAL_SECTION_END(&svc_ctx->tcb_list_lock);
	/*!
	 * ASSERT(tcb->fd >= 0);
	 * Close the connection, and then collecting the return value of thread
	 */
	if (write(tcb->ctrl_pipe[PIPE_WRITE], &ch, sizeof(ch)) != sizeof(ch)) {
		ErrPrint("write: %s\n", strerror(errno));
	}

	status = pthread_join(tcb->thid, &ret);
	if (status != 0) {
		ErrPrint("Unable to join a thread: %s\n", strerror(status));
	} else {
		DbgPrint("Thread returns: %p\n", ret);
	}

	secure_socket_destroy_handle(tcb->fd);

	CLOSE_PIPE(tcb->ctrl_pipe);
	DbgFree(tcb);
}

/*!
 * \note
 * SERVER THREAD
 */
static inline int update_fdset(struct service_context *svc_ctx, fd_set *set)
{
	Eina_List *l;
	struct service_event_item *item;
	int fd = 0;

	FD_ZERO(set);

	FD_SET(svc_ctx->fd, set);
	fd = svc_ctx->fd;

	FD_SET(svc_ctx->tcb_pipe[PIPE_READ], set);
	if (svc_ctx->tcb_pipe[PIPE_READ] > fd) {
		fd = svc_ctx->tcb_pipe[PIPE_READ];
	}

	FD_SET(svc_ctx->evt_pipe[PIPE_READ], set);
	if (svc_ctx->evt_pipe[PIPE_READ] > fd) {
		fd = svc_ctx->evt_pipe[PIPE_READ];
	}

	EINA_LIST_FOREACH(svc_ctx->event_list, l, item) {
		if (item->type == SERVICE_EVENT_TIMER) {
			FD_SET(item->info.timer.fd, set);
			if (fd < item->info.timer.fd) {
				fd = item->info.timer.fd;
			}
		}
	}

	return fd + 1;
}

/*!
 * \note
 * SERVER THREAD
 */
static inline void processing_timer_event(struct service_context *svc_ctx, fd_set *set)
{
	uint64_t expired_count;
	Eina_List *l;
	Eina_List *n;
	struct service_event_item *item;

	EINA_LIST_FOREACH_SAFE(svc_ctx->event_list, l, n, item) {
		switch (item->type) {
		case SERVICE_EVENT_TIMER:
			if (!FD_ISSET(item->info.timer.fd, set)) {
				break;
			}

			if (read(item->info.timer.fd, &expired_count, sizeof(expired_count)) == sizeof(expired_count)) {
				DbgPrint("Expired %d times\n", expired_count);
				if (item->event_cb(svc_ctx, item->cbdata) >= 0) {
					break;
				}
			} else {
				ErrPrint("read: %s\n", strerror(errno));
			}

			if (!eina_list_data_find(svc_ctx->event_list, item)) {
				break;
			}

			svc_ctx->event_list = eina_list_remove(svc_ctx->event_list, item);
			if (close(item->info.timer.fd) < 0) {
				ErrPrint("close: %s\n", strerror(errno));
			}
			DbgFree(item);
			break;
		default:
			ErrPrint("Unknown event: %d\n", item->type);
			break;
		}
	}
}

/*!
 * Accept new client connections
 * And create a new thread for service.
 *
 * Create Client threads & Destroying them
 * SERVER THREAD
 */
static void *server_main(void *data)
{
	struct service_context *svc_ctx = data;
	fd_set set;
	fd_set except_set;
	long ret;
	int client_fd;
	struct tcb *tcb;
	int fd;
	char evt_ch;
	struct packet_info *packet_info;

	DbgPrint("Server thread is activated\n");
	while (1) {
		fd = update_fdset(svc_ctx, &set);
		memcpy(&except_set, &set, sizeof(set));

		ret = select(fd, &set, NULL, &except_set, NULL);
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
			client_fd = secure_socket_get_connection_handle(svc_ctx->fd);
			if (client_fd < 0) {
				ErrPrint("Failed to establish a new connection [%d]\n", svc_ctx->fd);
				ret = -EFAULT;
				break;
			}

			tcb = tcb_create(svc_ctx, client_fd);
			if (!tcb) {
				ErrPrint("Failed to create a new TCB: %d (%d)\n", client_fd, svc_ctx->fd);
				secure_socket_destroy_handle(client_fd);
			}
		} 

		if (FD_ISSET(svc_ctx->evt_pipe[PIPE_READ], &set)) {
			if (read(svc_ctx->evt_pipe[PIPE_READ], &evt_ch, sizeof(evt_ch)) != sizeof(evt_ch)) {
				ErrPrint("Unable to read pipe: %s\n", strerror(errno));
				ret = -EFAULT;
				break;
			}

			CRITICAL_SECTION_BEGIN(&svc_ctx->packet_list_lock);
			packet_info = eina_list_nth(svc_ctx->packet_list, 0);
			svc_ctx->packet_list = eina_list_remove(svc_ctx->packet_list, packet_info);
			CRITICAL_SECTION_END(&svc_ctx->packet_list_lock);

			if (packet_info) {
				/*!
				 * \CRITICAL
				 * What happens if the client thread is terminated, so the packet_info->tcb is deleted
				 * while processing svc_ctx->service_thread_main?
				 */
				ret = svc_ctx->service_thread_main(packet_info->tcb, packet_info->packet, svc_ctx->service_thread_data);
				if (ret < 0) {
					ErrPrint("Service thread returns: %d\n", ret);
				}

				packet_destroy(packet_info->packet);
				DbgFree(packet_info);
			}

			/* Take a breathe */
			pthread_yield();
		}

		processing_timer_event(svc_ctx, &set);

		/*!
		 * \note
		 * Destroying TCB should be processed at last.
		 */
		if (FD_ISSET(svc_ctx->tcb_pipe[PIPE_READ], &set)) {
			Eina_List *lockfree_packet_list;
			Eina_List *l;
			Eina_List *n;

			if (read(svc_ctx->tcb_pipe[PIPE_READ], &tcb, sizeof(tcb)) != sizeof(tcb)) {
				ErrPrint("Unable to read pipe: %s\n", strerror(errno));
				ret = -EFAULT;
				break;
			}

			if (!tcb) {
				ErrPrint("Terminate service thread\n");
				ret = -ECANCELED;
				break;
			}

			lockfree_packet_list = NULL;
			CRITICAL_SECTION_BEGIN(&svc_ctx->packet_list_lock);
			EINA_LIST_FOREACH_SAFE(svc_ctx->packet_list, l, n, packet_info) {
				if (packet_info->tcb != tcb) {
					continue;
				}

				svc_ctx->packet_list = eina_list_remove(svc_ctx->packet_list, packet_info);
				lockfree_packet_list = eina_list_append(lockfree_packet_list, packet_info);
			}
			CRITICAL_SECTION_END(&svc_ctx->packet_list_lock);

			EINA_LIST_FREE(lockfree_packet_list, packet_info) {
				ret = read(svc_ctx->evt_pipe[PIPE_READ], &evt_ch, sizeof(evt_ch));
				DbgPrint("Flushing filtered pipe: %d (%c)\n", ret, evt_ch);
				ret = svc_ctx->service_thread_main(packet_info->tcb, packet_info->packet, svc_ctx->service_thread_data);
				if (ret < 0) {
					ErrPrint("Service thread returns: %d\n", ret);
				}
				packet_destroy(packet_info->packet);
				DbgFree(packet_info);
			}

			/*!
			 * \note
			 * Invoke the service thread main, to notify the termination of a TCB
			 */
			ret = svc_ctx->service_thread_main(tcb, NULL, svc_ctx->service_thread_data);

			/*!
			 * at this time, the client thread can access this tcb.
			 * how can I protect this TCB from deletion without disturbing the server thread?
			 */
			tcb_destroy(svc_ctx, tcb);
		} 

		/* If there is no such triggered FD? */
	}

	/*!
	 * Consuming all pended packets before terminates server thread.
	 *
	 * If the server thread is terminated, we should flush all pended packets.
	 * And we should services them.
	 * While processing this routine, the mutex is locked.
	 * So every other client thread will be slowed down, sequently, every clients can meet problems.
	 * But in case of termination of server thread, there could be systemetic problem.
	 * This only should be happenes while terminating the master daemon process.
	 */
	CRITICAL_SECTION_BEGIN(&svc_ctx->packet_list_lock);
	EINA_LIST_FREE(svc_ctx->packet_list, packet_info) {
		ret = read(svc_ctx->evt_pipe[PIPE_READ], &evt_ch, sizeof(evt_ch));
		DbgPrint("Flushing pipe: %d (%c)\n", ret, evt_ch);
		ret = svc_ctx->service_thread_main(packet_info->tcb, packet_info->packet, svc_ctx->service_thread_data);
		if (ret < 0) {
			ErrPrint("Service thread returns: %d\n", ret);
		}
		packet_destroy(packet_info->packet);
		DbgFree(packet_info);
	}
	CRITICAL_SECTION_END(&svc_ctx->packet_list_lock);

	tcb_teminate_all(svc_ctx);
	return (void *)ret;
}

/*!
 * \NOTE
 * MAIN THREAD
 */
HAPI struct service_context *service_common_create(const char *addr, int (*service_thread_main)(struct tcb *tcb, struct packet *packet, void *data), void *data)
{
	int status;
	struct service_context *svc_ctx;

	if (!service_thread_main || !addr) {
		ErrPrint("Invalid argument\n");
		return NULL;
	}

	if (strncmp(addr, COM_CORE_REMOTE_SCHEME, strlen(COM_CORE_REMOTE_SCHEME))) {
		int offset = 0;

		offset = strlen(COM_CORE_LOCAL_SCHEME);
		if (strncmp(addr, COM_CORE_LOCAL_SCHEME, offset)) {
			offset = 0;
		}

		if (unlink(addr + offset) < 0) {
			ErrPrint("[%s] - %s\n", addr, strerror(errno));
		}
	}

	svc_ctx = calloc(1, sizeof(*svc_ctx));
	if (!svc_ctx) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	svc_ctx->fd = secure_socket_create_server(addr);
	if (svc_ctx->fd < 0) {
		DbgFree(svc_ctx);
		return NULL;
	}

	svc_ctx->service_thread_main = service_thread_main;
	svc_ctx->service_thread_data = data;

	if (fcntl(svc_ctx->fd, F_SETFD, FD_CLOEXEC) < 0) {
		ErrPrint("fcntl: %s\n", strerror(errno));
	}

	if (fcntl(svc_ctx->fd, F_SETFL, O_NONBLOCK) < 0) {
		ErrPrint("fcntl: %s\n", strerror(errno));
	}

	if (pipe2(svc_ctx->evt_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		ErrPrint("pipe: %d\n", strerror(errno));
		secure_socket_destroy_handle(svc_ctx->fd);
		DbgFree(svc_ctx);
		return NULL;
	}

	if (pipe2(svc_ctx->tcb_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		ErrPrint("pipe: %s\n", strerror(errno));
		CLOSE_PIPE(svc_ctx->evt_pipe);
		secure_socket_destroy_handle(svc_ctx->fd);
		DbgFree(svc_ctx);
		return NULL;
	}

	status = pthread_mutex_init(&svc_ctx->packet_list_lock, NULL);
	if (status != 0) {
		ErrPrint("Unable to create a mutex: %s\n", strerror(status));
		CLOSE_PIPE(svc_ctx->evt_pipe);
		CLOSE_PIPE(svc_ctx->tcb_pipe);
		secure_socket_destroy_handle(svc_ctx->fd);
		DbgFree(svc_ctx);
		return NULL;
	}

	DbgPrint("Creating server thread\n");
	status = pthread_create(&svc_ctx->server_thid, NULL, server_main, svc_ctx);
	if (status != 0) {
		ErrPrint("Unable to create a thread for shortcut service: %s\n", strerror(status));
		status = pthread_mutex_destroy(&svc_ctx->packet_list_lock);
		if (status != 0) {
			ErrPrint("Error: %s\n", strerror(status));
		}
		CLOSE_PIPE(svc_ctx->evt_pipe);
		CLOSE_PIPE(svc_ctx->tcb_pipe);
		secure_socket_destroy_handle(svc_ctx->fd);
		DbgFree(svc_ctx);
		return NULL;
	}

	/*!
	 * \note
	 * To give a chance to run for server thread.
	 */
	pthread_yield();

	return svc_ctx;
}

/*!
 * \note
 * MAIN THREAD
 */
HAPI int service_common_destroy(struct service_context *svc_ctx)
{
	int status = 0;
	void *ret;

	if (!svc_ctx) {
		return -EINVAL;
	}

	/*!
	 * \note
	 * Terminate server thread
	 */
	if (write(svc_ctx->tcb_pipe[PIPE_WRITE], &status, sizeof(status)) != sizeof(status)) {
		ErrPrint("Failed to write: %s\n", strerror(errno));
	}

	status = pthread_join(svc_ctx->server_thid, &ret);
	if (status != 0) {
		ErrPrint("Join: %s\n", strerror(status));
	} else {
		DbgPrint("Thread returns: %p\n", ret);
	}

	secure_socket_destroy_handle(svc_ctx->fd);

	status = pthread_mutex_destroy(&svc_ctx->packet_list_lock);
	if (status != 0) {
		ErrPrint("Unable to destroy a mutex: %s\n", strerror(status));
	}

	CLOSE_PIPE(svc_ctx->evt_pipe);
	CLOSE_PIPE(svc_ctx->tcb_pipe);
	DbgFree(svc_ctx);
	return 0;
}

/*!
 * \note
 * SERVER THREAD or OTHER THREAD (not main)
 */
HAPI int tcb_is_valid(struct service_context *svc_ctx, struct tcb *tcb)
{
	Eina_List *l;
	struct tcb *tmp;
	int ret = 0;

	CRITICAL_SECTION_BEGIN(&svc_ctx->tcb_list_lock);
	EINA_LIST_FOREACH(svc_ctx->tcb_list, l, tmp) {
		if (tmp == tcb /* && tcb->svc_ctx == svc_ctx */) {
			ret = tcb->fd;
			break;
		}
	}
	CRITICAL_SECTION_END(&svc_ctx->tcb_list_lock);

	return ret;
}

/*!
 * \note
 * SERVER THREAD
 */
HAPI int tcb_fd(struct tcb *tcb)
{
	if (!tcb) {
		return -EINVAL;
	}

	return tcb->fd;
}

/*!
 * \note
 * SERVER THREAD
 */
HAPI int tcb_client_type(struct tcb *tcb)
{
	if (!tcb) {
		return -EINVAL;
	}

	return tcb->type;
}

/*!
 * \note
 * SERVER THREAD
 */
HAPI int tcb_client_type_set(struct tcb *tcb, enum tcb_type type)
{
	if (!tcb) {
		return -EINVAL;
	}

	DbgPrint("TCB[%p] Client type is changed to %d from %d\n", tcb, type, tcb->type);
	tcb->type = type;
	return 0;
}

/*!
 * \note
 * SERVER THREAD
 */
HAPI struct service_context *tcb_svc_ctx(struct tcb *tcb)
{
	if (!tcb) {
		return NULL;
	}

	return tcb->svc_ctx;
}

/*!
 * \note
 * SERVER THREAD
 */
HAPI int service_common_unicast_packet(struct tcb *tcb, struct packet *packet)
{
	if (!tcb || !packet) {
		DbgPrint("Invalid unicast: tcb[%p], packet[%p]\n", tcb, packet);
		return -EINVAL;
	}

	DbgPrint("Unicast packet\n");
	return com_core_send(tcb->fd, (void *)packet_data(packet), packet_size(packet), DEFAULT_TIMEOUT);
}

/*!
 * \note
 * SERVER THREAD
 */
HAPI int service_common_multicast_packet(struct tcb *tcb, struct packet *packet, int type)
{
	Eina_List *l;
	struct tcb *target;
	struct service_context *svc_ctx;
	int ret;

	if (!tcb || !packet) {
		DbgPrint("Invalid multicast: tcb[%p], packet[%p]\n", tcb, packet);
		return -EINVAL;
	}

	svc_ctx = tcb->svc_ctx;

	DbgPrint("Multicasting packets\n");

	/*!
	 * \note
	 * Does not need to make a critical section from here.
	 */
	EINA_LIST_FOREACH(svc_ctx->tcb_list, l, target) {
		if (target == tcb || target->type != type) {
			DbgPrint("Skip target: %p(%d) == %p/%d\n", target, target->type, tcb, type);
			continue;
		}

		ret = com_core_send(target->fd, (void *)packet_data(packet), packet_size(packet), DEFAULT_TIMEOUT);
		if (ret < 0) {
			ErrPrint("Failed to send packet: %d\n", ret);
		}
	}
	DbgPrint("Finish to multicast packet\n");
	return 0;
}

/*!
 * \note
 * SERVER THREAD
 */
HAPI struct service_event_item *service_common_add_timer(struct service_context *svc_ctx, double timer, int (*timer_cb)(struct service_context *svc_cx, void *data), void *data)
{
	struct service_event_item *item;

	item = calloc(1, sizeof(*item));
	if (!item) {
		ErrPrint("Heap: %s\n", strerror(errno));
		return NULL;
	}

	item->type = SERVICE_EVENT_TIMER;
	item->info.timer.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (item->info.timer.fd < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		DbgFree(item);
		return NULL;
	}

	if (service_common_update_timer(item, timer) < 0) {
		if (close(item->info.timer.fd) < 0) {
			ErrPrint("close: %s\n", strerror(errno));
		}
		DbgFree(item);
		return NULL;
	}

	item->event_cb = timer_cb;
	item->cbdata = data;

	svc_ctx->event_list = eina_list_append(svc_ctx->event_list, item);
	return item;
}

HAPI int service_common_update_timer(struct service_event_item *item, double timer)
{
	struct itimerspec spec;

	spec.it_interval.tv_sec = (time_t)timer;
	spec.it_interval.tv_nsec = (timer - spec.it_interval.tv_sec) * 1000000000;

	if (clock_gettime(CLOCK_MONOTONIC, &spec.it_value) < 0) {
		ErrPrint("clock_gettime: %s\n", strerror(errno));
		return -EFAULT;
	}

	spec.it_value.tv_sec += spec.it_interval.tv_sec;
	spec.it_value.tv_nsec += spec.it_interval.tv_nsec;

	if (timerfd_settime(item->info.timer.fd, TFD_TIMER_ABSTIME, &spec, NULL) < 0) {
		ErrPrint("Error: %s\n", strerror(errno));
		return -EFAULT;
	}

	DbgPrint("Armed interval: %u %u\n", spec.it_interval.tv_sec, spec.it_interval.tv_nsec);
	return 0;
}

/*!
 * \note
 * SERVER THREAD
 */
HAPI int service_common_del_timer(struct service_context *svc_ctx, struct service_event_item *item)
{
	if (!eina_list_data_find(svc_ctx->event_list, item)) {
		ErrPrint("Invalid event item\n");
		return -EINVAL;
	}

	svc_ctx->event_list = eina_list_remove(svc_ctx->event_list, item);

	if (close(item->info.timer.fd) < 0) {
		ErrPrint("close: %s\n", strerror(errno));
	}
	DbgFree(item);
	return 0;
}

HAPI int service_common_fd(struct service_context *ctx)
{
	return ctx->fd;
}

/* End of a file */
