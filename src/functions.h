//============================================================================
// Author : pengfei.wan
// Date : 2020.02.09
//============================================================================

#ifndef FUNCTIONS_H_
#define FUNCTIONS_H_

void log_printf(const char* const fmt, ...);

unsigned short checksum(void *buffer, size_t len);

int open_connection(const char *hostname, int port);

int socket_create_bind(unsigned short port);

int open_listener(char *host, int port);

int make_socket_non_blocking(int sfd);

int open_remote_socket(char *host, unsigned short port);

int read_fd_data(int fd, char *data, int size, int *remain);

int write_fd_data(int fd, char *data, int size);

uint32_t init_rand(rand_seed *rs);

uint32_t rand_cmwc(rand_seed *rs);

void encrypt(char *buff, int len, rand_seed *rs);

#endif /* FUNCTIONS_H_ */

