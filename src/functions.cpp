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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <sched.h>

#include "multiplex.h"
#include "functions.h"

void log_printf(const char* const fmt, ...) {
	char buff[64];
	char format[512];
	time_t now = time(0);
	va_list args;
	int error = errno;

	strftime(buff, sizeof(buff), "%Y-%m-%d %H:%M:%S", localtime(&now));
	if (error) {
		snprintf(format, sizeof(format), "%s - [%d : %s] - %s\n", buff, error, strerror(error), fmt);
	} else {
		snprintf(format, sizeof(format), "%s - %s\n", buff, fmt);
	}
	va_start(args, fmt);
	vprintf(format, args);
	va_end(args);
	if (error) {
		fflush(stdout);
		errno = 0;
	}

}

unsigned short checksum(void *buffer, size_t len) {
	unsigned char *buf = (unsigned char *) buffer;
	unsigned int seed = 0x1234;
	size_t i;

	for (i = 0; i < len; ++i) {
		seed += (unsigned int) (*buf++);
	}
	return (seed >> 2) & 0xFFFF;
}

int open_connection(const char *hostname, int port) {
	int sd;
	struct hostent *host;
	struct sockaddr_in addr;

	if ((host = gethostbyname(hostname)) == NULL) {
		log_printf("gethostbyname error - hostname : %s", hostname);
		return -1;
	}
	sd = socket(PF_INET, SOCK_STREAM, 0);
	if (sd > 0) {
		bzero(&addr, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = *(long *) (host->h_addr);
		if (connect(sd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
			close(sd);
			log_printf("connect error - %s:%d", hostname, port);
			return -1;
		}
		return sd;
	}
	return -1;
}

int socket_create_bind(unsigned short port) {
	struct sockaddr_in server_addr;
	int socket_fd = -1;
	int opt = 1;

	do {
		if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			log_printf("socket error");
			break;
		}
		if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (void *) &opt, sizeof(opt))) {
			log_printf("setsockopt SO_REUSEADDR error - socket : %d", socket_fd);
			break;
		}
		opt = 1;
		if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, (void *) &opt, sizeof(opt))) {
			log_printf("setsockopt SO_KEEPALIVE error - socket : %d", socket_fd);
			break;
		};
		server_addr.sin_family = AF_INET;
		server_addr.sin_port = htons(port);
		server_addr.sin_addr.s_addr = INADDR_ANY;
		bzero(&(server_addr.sin_zero), 8);
		if (bind(socket_fd, (struct sockaddr *) &server_addr, sizeof(struct sockaddr)) == -1) {
			log_printf("bind error - port : %d", port);
			break;
		}
		return socket_fd;
	} while (0);
	if (socket_fd != -1) {
		close(socket_fd);
	}
	return -1;
}

int open_listener(char *host, int port) {
	int sd;
	struct sockaddr_in addr;

	sd = socket(PF_INET, SOCK_STREAM, 0);
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(host);
	if (bind(sd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		log_printf("bind error - port : %d", port);
		abort();
	}
	if (listen(sd, 10) != 0) {
		log_printf("listen error");
		abort();
	}
	return sd;
}

int make_socket_non_blocking(int sfd) {
	int flags = 0;

	flags = fcntl(sfd, F_GETFL, 0);
	if (flags == -1) {
		log_printf("fcntl F_GETFL error - socket : %d", sfd);
		return -1;
	}
	flags |= O_NONBLOCK;
	if (fcntl(sfd, F_SETFL, flags) == -1) {
		log_printf("fcntl F_SETFL error - socket : %d", sfd);
		return -1;
	}
	flags = 1;
	if (setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, &flags, sizeof(flags)) < 0) {
		log_printf("setsockopt SO_KEEPALIVE error - socket : %d", sfd);
	}

	return 0;
}

int make_socket_blocking(int sfd) {
	int flags = 0;

	flags = fcntl(sfd, F_GETFL, 0);
	if (flags == -1) {
		log_printf("fcntl F_GETFL error - socket : %d", sfd);
		return -1;
	}
	flags &= !O_NONBLOCK;
	if (fcntl(sfd, F_SETFL, flags) == -1) {
		log_printf("fcntl F_SETFL error - socket : %d", sfd);
		return -1;
	}
	flags = 1;
	if (setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, &flags, sizeof(flags)) < 0) {
		log_printf("setsockopt SO_KEEPALIVE error - socket : %d", sfd);
	}

	return 0;
}

int open_remote_socket(char *host, unsigned short port) {
	int remote_fd = open_connection(host, port);

	if (remote_fd < 0) {
		return -1;
	}
	if (make_socket_non_blocking(remote_fd) != 0) {
		close(remote_fd);
		remote_fd = -1;
	}

	return remote_fd;
}

int read_fd_data(int fd, char *data, int size, int *remain) {
	int count = 0;
	ssize_t num = 0;

	*remain = 0;
	while ((num = read(fd, data + count, size - count))) {
		if (num < 0) {
			if (errno == EINTR) {
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				errno = 0;
				break;
			}
			log_printf("read socket error - socket : %d", fd);
			return -1;
		}
		count += num;
		if (count >= size) {
			// NOTICE, not epoll, skip!
			// *remain = 1;
			break;
		}
	}
	if (num == 0) {
		*remain = -1;
	}

	return count;
}

int write_fd_data_orig(int fd, char *data, int size, int *blocking) {
	int total = 0;

	while (total < size) {
		int n = write(fd, data + total, size - total);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct stat tStat;

				if (fstat(fd, &tStat) == 0 && fcntl(fd, F_GETFL, 0) != -1) {
					int error = 0;
					socklen_t len = sizeof(error);
					int retval = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);

					if (*blocking == 0 && retval == 0 && error == 0) {
						// sched_yield(); // nothing!
						*blocking = 1;
						if (make_socket_blocking(fd) == 0) {
							continue;
						}
					}
				}
			}
			log_printf("write socket error (n < 0) - socket : %d", fd);
			return -1;
		}
		if (n == 0) {
			log_printf("write socket error (n == 0) - socket : %d", fd);
			return -2;
		}
		total += n;
	}

	return total;
}

int write_fd_data(int fd, char *data, int size) {
	int blocking = 0;
	int rc = write_fd_data_orig(fd, data, size, &blocking);
	if (blocking != 0) {
		make_socket_non_blocking(fd);
	}
	return rc;
}

uint32_t init_rand(rand_seed *rs) {
	int i;
	uint32_t x = 'a';
	uint32_t PHI = 0x9e3779b9;
	time_t t = time(NULL);

	PHI = ((uint32_t) t / 3600 * 3600) % PHI;
	x = (PHI % (2 * 3 * 5 * 7 * 11 * 13 - 1) % 27) + x;
	rs->c = 362436;
	rs->Q[0] = x & 0xffffff;
	rs->Q[1] = (x + PHI) & 0xffffff;
	rs->Q[2] = (x + PHI + PHI) & 0xffffff;
	for (i = 3; i < 4096; i++) {
		rs->Q[i] = rs->Q[i - 3] ^ rs->Q[i - 2] ^ PHI ^ i;
	}

	return PHI;
}

uint32_t rand_cmwc(rand_seed *rs) {
	uint32_t t, a = 18782L;
	uint32_t i = rs->c;
	uint32_t x, r = 0xfffffe;

	i = (i + 1) % 4096;
	t = a * rs->Q[i] + rs->c;
	rs->c = (t >> 16) & 0xffffff;
	x = t + rs->c;
	if (x < rs->c) {
		x++;
		rs->c++;
	}

	return (rs->Q[i] = r ^ x);
}

void encrypt(char *buff, int len, rand_seed *rs) {
	int i = 0;

	for (i = 0; i < len; i++) {
		unsigned char x = rand_cmwc(rs) & 0xff;
		unsigned char *ch = (unsigned char*) (buff + i);
		*ch ^= x;
	}
}

