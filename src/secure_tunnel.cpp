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

static int write_tunnel_data(multiplex_bundle *bundle, int type) {
	multiplex_buffer *buffer = &bundle->proxy->buffer;

	memcpy(&bundle->proxy->buffer, &bundle->proxy->draft, BUFFER_LENGTH);
	buffer->flags = type;

	if (type == FLAG_DATA) {
		buffer->length = BUFFER_LENGTH + bundle->proxy->count;
	} else {
		buffer->data[0] = '\0';
		buffer->length = sizeof(multiplex_buffer);
	}
	buffer->checksum = 0;
	buffer->checksum = checksum(buffer, buffer->length);

	int len = buffer->length;
	encrypt((char*) buffer, len, bundle->tunnel->o_cipher);
	if (write_fd_data(bundle->tunnel->fd, (char*) buffer, len) != len) {
		return -1;
	}
	return 0;
}

static void close_proxy_socket(multiplex_bundle *bundle, socket_broker *broker) {

	if (bundle->tunnel != NULL && bundle->proxy != NULL) {
		write_tunnel_data(bundle, FLAG_CLOSE);
	}
	if (bundle->proxy != NULL) {
		broker->close_multiplex_context(bundle->proxy);
	}
	if (bundle->socket != NULL) {
		broker->close_multiplex_context(bundle->socket);
	}

}

static void accept_connection(int socket_fd, int is_client, multiplex_bundle *bundle, socket_broker *broker) {
	struct sockaddr in_addr;
	socklen_t in_len = sizeof(in_addr);
	int infd = -1;
	char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

	while ((infd = accept(socket_fd, &in_addr, &in_len)) != -1) {
		if (getnameinfo(&in_addr, in_len, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICHOST) == 0) {
			log_printf("Accepted socket - fd : %d (host=%s, port=%s)", infd, hbuf, sbuf);
		}
		if (make_socket_non_blocking(infd) != 0) {
			log_printf("accept_connection failed, infd = %d!", infd);
			close(infd);
			continue;
		}
		in_len = sizeof(in_addr);

		unsigned int ipv4 = ntohl((unsigned int) ((struct sockaddr_in*) &in_addr)->sin_addr.s_addr);
		unsigned short port = ntohs(((struct sockaddr_in*) &in_addr)->sin_port);

		if (is_client) {
			if (bundle->tunnel == NULL || (bundle->socket = broker->new_multiplex_context(infd, -bundle->tunnel->fd, ipv4, port)) == NULL) {
				close(infd);
			} else if ((bundle->proxy = broker->new_multiplex_context(bundle->tunnel->fd, infd, ipv4, port)) == NULL) {
				broker->close_multiplex_context(bundle->socket);
			} else if (write_tunnel_data(bundle, FLAG_INIT)) {
				log_printf("BAD === accept_connection ===");
				close_proxy_socket(bundle, broker);
			}
		} else if (broker->new_multiplex_context(infd, 0, 0, 0) == NULL) {
			close(infd);
		}
	}

	if (errno != EAGAIN && errno != EWOULDBLOCK) {
		log_printf("accept_connection error");
	}

}

static int read_socket_data(multiplex_context *proxy, int *remain) {

	*remain = 0;
	int count = read_fd_data(proxy->socket, proxy->buffer.data + proxy->count, proxy->size - proxy->count, remain);
	if (count > 0) {
		proxy->count += count;
	}
	return count;

}

static int read_tunnel_data(multiplex_context *tunnel, int *remain) {

	*remain = 0;
	int count = read_fd_data(tunnel->fd, (char*) &tunnel->buffer + tunnel->count, tunnel->size - tunnel->count, remain);
	if (count > 0) {
		encrypt((char*) &tunnel->buffer + tunnel->count, count, tunnel->i_cipher);
		tunnel->count += count;
	}
	return count;

}

static multiplex_context* new_proxy_socket(int fd, char *host, unsigned short port, unsigned int ipv4, unsigned short portv4, socket_broker *broker) {
	int proxy_fd = -1;
	multiplex_context *proxy = NULL;
	multiplex_context *socket = NULL;

	proxy_fd = open_remote_socket(host, port);
	if (proxy_fd > 0) {
		if ((proxy = broker->new_multiplex_context(fd, proxy_fd, ipv4, portv4)) == NULL) {
			close(proxy_fd);
		} else if ((socket = broker->new_multiplex_context(proxy_fd, -fd, ipv4, portv4)) == NULL) {
			close(proxy_fd);
			broker->delete_multiplex_context(proxy);
			proxy = NULL;
		}
	}
	if (proxy == NULL) {
		proxy = broker->new_multiplex_context(fd, 65535, ipv4, portv4); // sentinel means failed
	}

	return proxy;
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

static void tunnel_loop(multiplex_bundle *bundle, socket_broker *broker, char *host, unsigned short port, int is_client, int *last_pong) {
	int remain = 0;

	do {
		int count = read_tunnel_data(bundle->tunnel, &remain);

		while (bundle->tunnel->count >= (int) sizeof(multiplex_buffer)) {
			multiplex_context **context = NULL;
			multiplex_buffer *buffer = (multiplex_buffer*) &bundle->tunnel->buffer;

			if (buffer->length > PACKAGE_LENGTH) {
				remain = -1;
				break;
			}

			if (bundle->tunnel->count >= buffer->length) {

				if (check_buffer(buffer) != 0) {
					remain = -1;
					break;
				}

				if ((buffer->flags & FLAG_PING) == FLAG_PING) {
					if (is_client) {
						*last_pong = (int) time(NULL);
					} else {
						write_tunnel_ping(bundle->tunnel);
					}
				} else {
					int closed = 0;

					bundle->proxy = bundle->socket = NULL;
					if ((context = broker->search_multiplex_context(bundle->tunnel->fd, buffer->ipv4, buffer->port)) != NULL) {
						bundle->proxy = *context;
						if ((context = broker->search_multiplex_context(bundle->proxy->socket, bundle->proxy->draft.ipv4, bundle->proxy->draft.port)) != NULL) {
							bundle->socket = *context;
						}
					}

					if ((buffer->flags & FLAG_CLOSE) == FLAG_CLOSE) {
						closed = 1;
					} else if ((buffer->flags & FLAG_INIT) == FLAG_INIT) {
						close_proxy_socket(bundle, broker);
						bundle->proxy = bundle->socket = NULL;
						log_printf("FLAG_INIT - fd : %d, %d.%d.%d.%d:%hu", bundle->tunnel->fd, (buffer->ipv4 >> 24) & 0xFF, (buffer->ipv4 >> 16) & 0xFF,
								(buffer->ipv4 >> 8) & 0xFF, buffer->ipv4 & 0xFF, buffer->port);
						if ((bundle->proxy = new_proxy_socket(bundle->tunnel->fd, host, port, buffer->ipv4, buffer->port, broker)) != NULL) {
							if ((context = broker->search_multiplex_context(bundle->proxy->socket, bundle->proxy->draft.ipv4, bundle->proxy->draft.port))
									!= NULL) {
								bundle->socket = *context;
							}
						}
					}

					if (closed == 0 && bundle->socket != NULL && (buffer->flags & FLAG_DATA) == FLAG_DATA) {
						int len = buffer->length - BUFFER_LENGTH;

						if (write_fd_data(bundle->socket->fd, buffer->data, len) != len) {
							closed = 1;
						}
					}

					if (closed != 0 || bundle->socket == NULL) {
						close_proxy_socket(bundle, broker);
					}

				}

				if (bundle->tunnel->count > buffer->length) {
					bundle->tunnel->count -= buffer->length;
					memmove(buffer, ((char*) buffer) + buffer->length, bundle->tunnel->count);
				} else {
					bundle->tunnel->count = 0;
				}
			} else {
				break;
			}
		}

		if (count < 0 || remain == -1) {
			log_printf("=== tunnel_loop - close_multiplex_tunnel === begin");
			broker->close_multiplex_tunnel(bundle->tunnel);
			log_printf("=== tunnel_loop - close_multiplex_tunnel === end");
			break;
		}

	} while (remain == 1);

}

static void socket_loop(multiplex_bundle *bundle, socket_broker *broker) {
	int remain = 0;

	do {
		int result = 0;
		int count = read_socket_data(bundle->proxy, &remain);

		if (count > 0) {
			result = write_tunnel_data(bundle, FLAG_DATA);
		}
		if (count < 0 || remain == -1) {
			result = write_tunnel_data(bundle, FLAG_CLOSE);
		}
		if (result == -1) {
			log_printf("=== socket_loop - close_multiplex_tunnel === begin");
			broker->close_multiplex_tunnel(bundle->tunnel);
			log_printf("=== socket_loop - close_multiplex_tunnel === end");
			break;
		} else if (count < 0 || remain == -1) {
			close_proxy_socket(bundle, broker);
			break;
		} else {
			bundle->proxy->count = 0;
		}
	} while (remain == 1);

}

static int ready_select(fd_set *rfds, socket_broker *broker) {
	int i = 0;
	int max = 0;

	for (i = 0; i < broker->get_size(); i++) {
		multiplex_context *context = *(broker->get_array() + i);

		if (context->socket <= 0) {
			FD_SET(context->fd, rfds);
			if (max < context->fd) {
				max = context->fd;
			} else if (max > context->fd) {
				log_printf("========= fd array bad order! =========");
			}
		}
	}

	return max;
}

int check_select(socket_broker *broker, int socket_fd) {
	int i = 0;
	int ok = 0;
	int bad = 0;
	struct stat tStat;

	if (fstat(socket_fd, &tStat) == -1) {
		log_printf("socket_fd fstat %d error:%s", socket_fd, strerror(errno));
		++bad;
	} else {
		int flags = fcntl(socket_fd, F_GETFL, 0);

		if (flags == -1) {
			log_printf("fstat bad fd : %d", socket_fd);
			++bad;
		} else {
			++ok;
		}
	}

	for (i = 0; i < broker->get_size(); i++) {
		multiplex_context *context = *(broker->get_array() + i);

		if (context->socket <= 0) {
			struct stat tStat;

			if (fstat(context->fd, &tStat) == -1) {
				log_printf("fstat %d error:%s", context->fd, strerror(errno));
				log_printf("Bad file descriptor, fd : %d, socket : %d, - ipv4 : %u, port : %hu", context->fd, context->socket, context->draft.ipv4,
						context->draft.port);
				++bad;
			} else {
				int flags = fcntl(socket_fd, F_GETFL, 0);

				if (flags == -1) {
					log_printf("fstat bad fd : %d", context->fd);
					++bad;
				} else {
					++ok;
				}
			}
		}
	}

	log_printf("DEBUG - fstat - check_select total ok fd : %d", ok);

	return bad;
}

int main(int argc, char *argv[]) {
	int socket_fd = -1;
	fd_set rfds;
	struct timeval tv;
	int i, max_fd, retval;

	unsigned short listen_port;
	char *remote_ip;
	unsigned short remote_port;
	int is_client = 0;

	log_printf("secure_tunnel Version : 1.0.0");
	log_printf("Usage : %s listen_port remote_ip remote_port [is_client]", argv[0]);
	if (argc < 4) {
		log_printf("bad arguments!");
		abort();
	} else if (argc > 4) {
		is_client = 1;
	}

	listen_port = atoi(argv[1]);
	remote_ip = argv[2];
	remote_port = atoi(argv[3]);

	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	if (is_client) {
		socket_fd = open_listener((char*) "127.0.0.1", listen_port);
	} else {
		socket_fd = socket_create_bind(listen_port);
	}
	if (make_socket_non_blocking(socket_fd) != 0) {
		exit(1);
	}
	if (listen(socket_fd, 10) == -1) {
		log_printf("Listen");
		exit(1);
	}
	log_printf("TCPServer Waiting for client on port %d", listen_port);
	fflush(stdout);

	int remote_fd = -1;
	int last_ping = -1;
	int last_pong = -1;

	socket_broker *broker = new socket_broker(MAX_EVENTS);

	while (1) {
		multiplex_bundle bundle;
		multiplex_context **context = NULL;

		if (is_client) { // ping server
			if (remote_fd > 0 && (context = broker->search_multiplex_context(remote_fd, 0, 0)) != NULL) {
				int now = (int) time(NULL);

				if (last_ping + 5 < now || last_ping > now) {
					write_tunnel_ping(*context);
					last_ping = now;
				} else if (last_pong + 25 < last_ping) {
					log_printf("tunnel connection is broken, close_multiplex_tunnel.");
					broker->close_multiplex_tunnel(*context);
					remote_fd = -1;
				}
				context = NULL;
			}
		}

		FD_ZERO(&rfds);
		max_fd = ready_select(&rfds, broker);
		FD_SET(socket_fd, &rfds);
		if (max_fd < socket_fd) {
			max_fd = socket_fd;
		}
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (errno != 0) {
			errno = 0;
		}

		retval = select(max_fd + 1, &rfds, NULL, NULL, &tv);

		if (retval < 0 && errno != EBADF) {
			log_printf("select()");
			log_printf("Bad file descriptor, count = %d", check_select(broker, socket_fd));
			sleep(1);
			log_printf("select error, close_all_multiplex_context : max_fd = %d, fd count = %d", max_fd, broker->close_all_multiplex_context());
			continue;
		} else if (retval == 0) {
			fflush(stdout);
			continue;
		} else {
			int clear = 0;

			if (retval < 0) {
				clear = 1;
			}

			if (clear || FD_ISSET(socket_fd, &rfds)) {

				bundle.tunnel = bundle.proxy = bundle.socket = NULL;

				if (is_client) {
					if (remote_fd > 0 && broker->search_multiplex_context(remote_fd, 0, 0) == NULL) {
						remote_fd = -1;
					}
					if (remote_fd == -1) {
						if ((remote_fd = open_remote_socket(remote_ip, remote_port)) > 0) {
							if (broker->new_multiplex_context(remote_fd, 0, 0, 0) == NULL) {
								close(remote_fd);
								remote_fd = -1;
							} else {
								last_pong = (int) time(NULL);
							}
						}
					}
					if ((context = broker->search_multiplex_context(remote_fd, 0, 0)) != NULL) {
						bundle.tunnel = *context;
					}
				}

				accept_connection(socket_fd, is_client, &bundle, broker);

			}

			for (i = 0; i < broker->get_size(); i++) {
				volatile multiplex_context *iter = *(broker->get_array() + i);

				bundle.tunnel = bundle.proxy = bundle.socket = NULL;

				if (iter->socket <= 0) {
					if (clear || FD_ISSET(iter->fd, &rfds)) {
						int fd = iter->fd;

						if (iter->socket < 0) {
							context = broker->search_multiplex_context_by_fd(fd);
							if (context == NULL) {
								close(fd);
							} else {
								bundle.socket = *context;
								if ((context = broker->search_multiplex_context(-bundle.socket->socket, 0, 0)) != NULL) {
									bundle.tunnel = *context;
								}
								if ((context = broker->search_multiplex_context(-bundle.socket->socket, bundle.socket->draft.ipv4, bundle.socket->draft.port))
										!= NULL) {
									bundle.proxy = *context;
								}
								if (bundle.tunnel == NULL || bundle.proxy == NULL) {
									log_printf("BAD - === main ===");
									close_proxy_socket(&bundle, broker);
								} else {
									socket_loop(&bundle, broker);
								}
							}
						} else {
							if ((context = broker->search_multiplex_context(fd, 0, 0)) == NULL) {
								close(fd);
							} else {
								bundle.tunnel = *context;
								tunnel_loop(&bundle, broker, remote_ip, remote_port, is_client, &last_pong);
							}
						}

					}
				}
			}

		}

	}

	log_printf("secure_tunnel main exit, not go here!");
	broker->close_all_multiplex_context();
	close(socket_fd);
	return 0;

}

