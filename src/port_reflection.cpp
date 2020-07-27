/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//============================================================================
// Author : pengfei.wan
// Date : 2020.02.09
//============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>

#include "multiplex.h"
#include "functions.h"
#include "socket_broker.h"

typedef enum {
	ACCEPT_SERVER = 0, ACCEPT_PROXY = 1
} accept_mode;

typedef enum {
	ACCEPT_BAD = -1, ACCEPT_INIT = 0, ACCEPT_YES = 1
} accept_flag;

static int write_tunnel_ping(multiplex_context *tunnel) {
	multiplex_buffer buffer;

	buffer.magic = MAGIC_NUMBER;
	buffer.flags = FLAG_PING;
	buffer.ipv4 = -1;
	buffer.port = -1;
	buffer.data[0] = '\0';
	buffer.length = sizeof(multiplex_buffer);
	buffer.checksum = 0;
	buffer.checksum = checksum(&buffer, buffer.length);

	int len = buffer.length;
	encrypt((char*) &buffer, len, tunnel->o_cipher);
	if (write_fd_data(tunnel->fd, (char*) &buffer, len) != len) {
		return -1;
	}
	return 0;
}

static int write_tunnel_signin(multiplex_context *tunnel) {
	multiplex_buffer buffer;

	buffer.magic = MAGIC_NUMBER;
	buffer.flags = FLAG_SIGNIN;
	buffer.ipv4 = -1;
	buffer.port = -1;
	buffer.data[0] = '\0';
	buffer.length = sizeof(multiplex_buffer);
	buffer.checksum = 0;
	buffer.checksum = checksum(&buffer, buffer.length);

	int len = buffer.length;
	encrypt((char*) &buffer, len, tunnel->o_cipher);
	if (write_fd_data(tunnel->fd, (char*) &buffer, len) != len) {
		return -1;
	}
	return 0;
}

static int write_tunnel_accept(multiplex_context *tunnel, int fd, accept_flag flag) {
	multiplex_buffer buffer;

	buffer.magic = MAGIC_NUMBER;
	buffer.flags = FLAG_ACCEPT;
	buffer.ipv4 = fd;
	buffer.port = flag;
	buffer.data[0] = '\0';
	buffer.length = sizeof(multiplex_buffer);
	buffer.checksum = 0;
	buffer.checksum = checksum(&buffer, buffer.length);

	int len = buffer.length;
	encrypt((char*) &buffer, len, tunnel->o_cipher);
	if (write_fd_data(tunnel->fd, (char*) &buffer, len) != len) {
		return -1;
	}
	return 0;
}

multiplex_context* find_tunnel_context(socket_broker *broker, int tunnel_fd) {
	multiplex_context *context = NULL;

	for (int i = 0; i < broker->get_size(); i++) {
		multiplex_context *iter = *(broker->get_array() + i);

		if (iter->socket == 0 && iter->draft.data[0] == FLAG_SIGNIN && (context == NULL || iter->fd == tunnel_fd)) {
			context = iter;
			log_printf("find_tunnel_context , iter_fd = %d, tunnel_fd = %d", iter->fd, tunnel_fd);
		}
	}
	return context;
}

static void accept_connection(int socket_fd, accept_mode mode, socket_broker *broker, int tunnel_fd) {
	struct sockaddr in_addr;
	socklen_t in_len = sizeof(in_addr);
	int infd = -1;
	char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

	while ((infd = accept(socket_fd, &in_addr, &in_len)) != -1) {
		if (getnameinfo(&in_addr, in_len, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICHOST) == 0) {
			log_printf("===[Listen %d]===, accepted socket - fd : %d (host=%s, port=%s)", socket_fd, infd, hbuf, sbuf);
		}
		if (make_socket_non_blocking(infd) != 0) {
			log_printf("accept_connection failed, infd = %d!", infd);
			close(infd);
			continue;
		}
		in_len = sizeof(in_addr);

		multiplex_context *context = NULL;

		if (mode == ACCEPT_SERVER) {
			if ((context = broker->new_multiplex_context(infd, 0, 0, 0)) == NULL) {
				close(infd);
			} else {
				context->size = sizeof(multiplex_buffer);
				context->draft.data[0] = '\0';
			}
		} else if (mode == ACCEPT_PROXY) {
			multiplex_context *tunnel = NULL;

			if ((tunnel = find_tunnel_context(broker, tunnel_fd)) == NULL || (context = broker->new_multiplex_context(infd, infd, 0, 0)) == NULL) {
				close(infd);
			} else if (write_tunnel_accept(tunnel, infd, ACCEPT_INIT)) {
				broker->close_fd_delete_context(context->fd);
			}
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			// log_printf("accept_connection error");
		}
	}
}

static int read_socket_data(multiplex_context *context, int *remain) {

	*remain = 0;
	int count = read_fd_data(context->fd, context->buffer.data + context->count, context->size - context->count, remain);
	if (count > 0) {
		context->count += count;
	}
	return count;

}

static int read_tunnel_data(multiplex_context *tunnel, int *remain) {

	*remain = 0;
	int count = read_fd_data(tunnel->fd, ((char*) &tunnel->buffer) + tunnel->count, tunnel->size - tunnel->count, remain);
	if (count > 0) {
		encrypt(((char*) &tunnel->buffer) + tunnel->count, count, tunnel->i_cipher);
		tunnel->count += count;
	}
	return count;

}

static int check_buffer(multiplex_buffer *buffer) {
	if (buffer->magic != MAGIC_NUMBER) {
		return -1;
	}

	unsigned short cs1, cs2;

	cs1 = buffer->checksum;
	buffer->checksum = 0;
	cs2 = checksum(buffer, buffer->length);
	buffer->checksum = cs1;
	if (cs1 != cs2) {
		return -2;
	}

	return 0;
}

static int make_proxy(socket_broker *broker, multiplex_context *context, int fd, char *remote_ip, unsigned short remote_port, char *local_ip,
		unsigned short local_port) {
	multiplex_context *proxy = NULL;
	multiplex_context *local = NULL;
	int proxy_fd = -1;
	int local_fd = open_remote_socket(local_ip, local_port);

	if (local_fd > 0) {
		proxy_fd = open_remote_socket(remote_ip, remote_port);
		if (proxy_fd > 0) {
			if ((proxy = broker->new_multiplex_context(proxy_fd, 0, 0, 0)) != NULL) {
				if (write_tunnel_accept(proxy, fd, ACCEPT_YES) == 0) {
					proxy->size = DATA_LENGTH;
					proxy->socket = local_fd;
					local = broker->new_multiplex_context(local_fd, proxy_fd, 0, 0);
				}
			}
		}
	}
	if (local == NULL) {
		write_tunnel_accept(context, fd, ACCEPT_BAD);
		if (local_fd > 0) {
			close(local_fd);
		}
		if (proxy_fd > 0) {
			if (proxy != NULL) {
				broker->close_fd_delete_context(proxy_fd);
			} else {
				close(proxy_fd);
			}
		}

		return -1;
	}

	return 0;
}

static int tunnel_loop(socket_broker *broker, multiplex_context *context, int is_client, char *remote_ip, unsigned short remote_port, char *local_ip,
		unsigned short local_port, int *last_pong, int *tunnel_fd) {
	int remain = 0;
	int count = read_tunnel_data(context, &remain);

	while (context->count >= (int) sizeof(multiplex_buffer)) {
		multiplex_buffer *buffer = (multiplex_buffer*) &context->buffer;

		if (buffer->length > PACKAGE_LENGTH) {
			remain = -1;
			break;
		}
		if (context->count >= buffer->length) {

			context->count = 0;

			if (check_buffer(buffer) != 0) {
				remain = -1;
				break;
			}

			if ((buffer->flags & FLAG_PING) == FLAG_PING) {
				if (is_client) {
					*last_pong = (int) time(NULL);
				} else {
					*tunnel_fd = context->fd;
					write_tunnel_ping(context);
				}
			} else {
				if ((buffer->flags & FLAG_CLOSE) == FLAG_CLOSE) {
					remain = -1;
					break;
				} else if ((buffer->flags & FLAG_SIGNIN) == FLAG_SIGNIN) {
					context->draft.data[0] = FLAG_SIGNIN;
				} else if ((buffer->flags & FLAG_ACCEPT) == FLAG_ACCEPT) {
					int fd = buffer->ipv4;

					if (is_client) {
						log_printf("=== make_proxy start === fd : %d", fd);
						if (make_proxy(broker, context, fd, remote_ip, remote_port, local_ip, local_port)) {
							log_printf("=== tunnel_loop - make_proxy === failed fd : %d", fd);
						}
					} else {
						multiplex_context *proxy = broker->find_multiplex_context_by_fd(fd);

						log_printf("=== server receive accept  === fd : %d", fd);

						if (buffer->port == ACCEPT_YES) {
							if (proxy != NULL) {
								proxy->socket = context->fd;
								context->socket = proxy->fd;
								context->size = DATA_LENGTH;
								log_printf("=== server receive accept OK  === fd : %d", fd);
							} else {
								remain = -1;
								break;
							}
						} else if (proxy != NULL) {
							broker->close_fd_delete_context(proxy->fd);
						}
					}
				}
			}
		}

		break;
	}

	if (count < 0 || remain == -1) {
		log_printf("=== tunnel_loop - close_fd_delete_context ===");
		broker->close_fd_delete_context(context->fd);
		return -1;
	}

	return 0;
}

static int socket_loop(socket_broker *broker, multiplex_context *context) {
	int remain = 0;
	multiplex_context *proxy = NULL;

	if (context->socket == context->fd) {
		if (recv(context->fd, NULL, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
			remain = -1;
		}
	} else {
		int count = read_socket_data(context, &remain);

		proxy = broker->find_multiplex_context_by_fd(context->socket);
		if (count < 0 || proxy == NULL || write_fd_data(proxy->fd, context->buffer.data, context->count) != context->count) {
			remain = -1;
		} else {
			context->count = 0;
		}
	}

	if (remain == -1) {
		log_printf("socket_loop delete, fd= %d, socket = %d, broker.size = %d", context->fd, context->socket, broker->get_size());
		broker->close_fd_delete_context(context->fd);
		if (proxy != NULL) {
			broker->close_fd_delete_context(proxy->fd);
		}
	}
	return remain;
}

int ready_select(fd_set *rfds, socket_broker *broker) {
	int i = 0;
	int max = 0;

	for (i = 0; i < broker->get_size(); i++) {
		multiplex_context *context = *(broker->get_array() + i);

		FD_SET(context->fd, rfds);
		if (max < context->fd) {
			max = context->fd;
		} else if (max > context->fd) {
			log_printf("=== fd array bad order! ===");
		}
	}

	return max;
}

int main(int argc, char *argv[]) {
	int socket_fd = -1;
	int external_fd = -1;

	fd_set rfds;
	struct timeval tv;
	int i, max_fd, retval;

	int is_client = 0;
	unsigned short listen_port;
	char *remote_ip = NULL, *local_ip = NULL;
	unsigned short remote_port = 0, local_port = 0;

	log_printf("port_reflect Version : 1.0.0");
	log_printf("Usage [server_mode] : %s listen_port", argv[0]);
	log_printf("Usage [client_mode] : %s remote_ip remote_port local_ip local_port", argv[0]);

	if (argc == 2) {
		listen_port = atoi(argv[1]);
		remote_port = listen_port + 1;
	} else if (argc == 5) {
		is_client = 1;
		remote_ip = argv[1];
		remote_port = atoi(argv[2]);
		local_ip = argv[3];
		local_port = atoi(argv[4]);
	} else {
		log_printf("bad arguments!");
		abort();
	}

	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	if (!is_client) {
		socket_fd = socket_create_bind(listen_port);
		if (make_socket_non_blocking(socket_fd) != 0) {
			exit(1);
		}
		if (listen(socket_fd, 10) == -1) {
			log_printf("Listen");
			exit(1);
		}
		external_fd = socket_create_bind(remote_port);
		if (make_socket_non_blocking(external_fd) != 0) {
			exit(1);
		}
		if (listen(external_fd, 10) == -1) {
			log_printf("Listen");
			exit(1);
		}
		log_printf("TCPServer Waiting for client on port %d, remote port %d", listen_port, remote_port);
	}
	fflush(stdout);

	int remote_fd = -1;
	int last_ping = -1;
	int last_pong = -1;
	int tunnel_fd = -1;

	socket_broker *broker = new socket_broker(MAX_EVENTS);

	while (1) {
		multiplex_context *context = NULL;

		if (is_client) {
			if (remote_fd == -1) {
				if ((remote_fd = open_remote_socket(remote_ip, remote_port)) > 0) {
					multiplex_context *tunnel = NULL;

					log_printf("client open_remote_socket ..........");
					if ((tunnel = broker->new_multiplex_context(remote_fd, 0, 0, 0)) == NULL) {
						close(remote_fd);
						remote_fd = -1;
					} else if (write_tunnel_signin(tunnel)) {
						broker->close_fd_delete_context(tunnel->fd);
						remote_fd = -1;
					} else {
						tunnel->size = sizeof(multiplex_buffer);
						last_pong = (int) time(NULL);
					}
				}
			} else if ((context = broker->find_multiplex_context_by_fd(remote_fd)) != NULL) {
				int now = (int) time(NULL);

				if (last_ping + 5 < now || last_ping > now) {
					write_tunnel_ping(context);
					last_ping = now;
				} else if (last_pong + 25 < last_ping) {
					log_printf("tunnel connection is broken, close_multiplex_tunnel.");
					broker->close_fd_delete_context(context->fd);
					remote_fd = -1;
				}
				context = NULL;
			}
		}

		FD_ZERO(&rfds);
		max_fd = ready_select(&rfds, broker);
		if (socket_fd > 0) {
			FD_SET(socket_fd, &rfds);
			if (max_fd < socket_fd) {
				max_fd = socket_fd;
			}
		}
		if (external_fd > 0) {
			FD_SET(external_fd, &rfds);
			if (max_fd < external_fd) {
				max_fd = external_fd;
			}
		}
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (errno != 0) {
			errno = 0;
		}

		retval = select(max_fd + 1, &rfds, NULL, NULL, &tv);

		if (retval < 0 && errno != EBADF) {
			sleep(1);
			log_printf("select error, close_all : max_fd = %d, fd count = %d", max_fd, broker->close_all_fd_delete_all_context());
			continue;
		} else if (retval == 0) {
			fflush(stdout);
			continue;
		} else {
			int clear = 0;

			if (retval < 0 || errno == EFAULT) {
				log_printf("select error, close_all_fd_delete_all_context : max_fd = %d, fd count = %d", max_fd, broker->close_all_fd_delete_all_context());
				clear = 1;
			}

			if (!is_client) {
				if (clear || FD_ISSET(socket_fd, &rfds)) {
					accept_connection(socket_fd, ACCEPT_SERVER, broker, tunnel_fd);
				} else if (clear || FD_ISSET(external_fd, &rfds)) {
					accept_connection(external_fd, ACCEPT_PROXY, broker, tunnel_fd);
				}
			}

			for (i = 0; i < broker->get_size(); i++) {
				volatile multiplex_context *iter = *(broker->get_array() + i);

				if (iter->socket >= 0) {
					if (clear || FD_ISSET(iter->fd, &rfds)) {
						int fd = iter->fd;

						if ((context = broker->find_multiplex_context_by_fd(fd)) == NULL) {
							close(fd);
						} else {
							if (iter->socket == 0) { // for tunnel

								if (tunnel_loop(broker, context, is_client, remote_ip, remote_port, local_ip, local_port, &last_pong, &tunnel_fd)
										&& fd == remote_fd) {
									remote_fd = -1;
								}

							} else if (iter->socket > 0) { // for proxy

								socket_loop(broker, context);

							} else {
								broker->close_fd_delete_context(context->fd);
							}
						}
					}
				}
			}

		}

	}

	log_printf("main exit, not go here!");
	broker->close_all_fd_delete_all_context();
	if (!is_client) {
		if (socket_fd != -1) {
			close(socket_fd);
		}
		if (remote_fd != -1) {
			close(remote_fd);
		}
	}
	return 0;

}

