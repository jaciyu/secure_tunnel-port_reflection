//============================================================================
// Author : pengfei.wan
// Date : 2020.02.09
//============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <stdint.h>
#include <time.h>

#include "multiplex.h"
#include "functions.h"
#include "socket_broker.h"

socket_broker::socket_broker(int capacity) :
		multiplex_context_capacity(capacity) {

	multiplex_context_size = 0;
	multiplex_context_array = new multiplex_context*[capacity];
	memset(multiplex_context_array, 0, sizeof(multiplex_context*) * capacity);

}

static int socket_all_compare(void const *a, void const *b) {
	multiplex_context *mc1 = *((multiplex_context**) a);
	multiplex_context *mc2 = *((multiplex_context**) b);

	if (mc1->fd < mc2->fd) {
		return -1;
	} else if (mc1->fd > mc2->fd) {
		return 1;
	} else if (mc1->draft.ipv4 < mc2->draft.ipv4) {
		return -1;
	} else if (mc1->draft.ipv4 > mc2->draft.ipv4) {
		return 1;
	} else {
		return (int) mc1->draft.port - mc2->draft.port;
	}
}

static int socket_fd_compare(void const *a, void const *b) {
	multiplex_context *mc1 = *((multiplex_context**) a);
	multiplex_context *mc2 = *((multiplex_context**) b);

	return mc1->fd - mc2->fd;
}

void socket_broker::sort_multiplex_context_array() {
	if (multiplex_context_size > 0 && multiplex_context_size < MAX_EVENTS) {
		qsort(multiplex_context_array, multiplex_context_size, sizeof(multiplex_context*), socket_all_compare);
	}
}

multiplex_context** socket_broker::search_multiplex_context_by_fd(int fd) {
	multiplex_context mc;
	multiplex_context *mc2 = &mc;

	mc.fd = fd;
	return (multiplex_context**) bsearch(&mc2, multiplex_context_array, multiplex_context_size, sizeof(multiplex_context*), socket_fd_compare);
}

multiplex_context** socket_broker::search_multiplex_context(int fd, unsigned int ipv4, unsigned short port) {
	multiplex_context mc;
	multiplex_context *mc2 = &mc;

	mc.fd = fd;
	mc.draft.ipv4 = ipv4;
	mc.draft.port = port;
	return (multiplex_context**) bsearch(&mc2, multiplex_context_array, multiplex_context_size, sizeof(multiplex_context*), socket_all_compare);
}

multiplex_context* socket_broker::new_multiplex_context(int fd, int socket, unsigned int ipv4, unsigned short port) {
	multiplex_context *context = NULL;

	if (multiplex_context_size + 1 >= multiplex_context_capacity) {
		return NULL;
	}
	if (socket < 0) {
		context = (multiplex_context*) malloc(sizeof(multiplex_context));
	} else {
		context = (multiplex_context*) malloc(WRAP_LENGTH);
	}
	if (context == NULL) {
		log_printf("new_multiplex_context error - fd %d, socket : %d", fd, socket);
		abort();
	}
	memset(context, 0, sizeof(multiplex_context));

	context->fd = fd;
	context->socket = socket;
	context->count = 0;
	context->i_cipher = NULL;
	context->o_cipher = NULL;
	if (socket == 0 && ipv4 == 0 && port == 0) { // for tunnel == 0
		context->size = PACKAGE_LENGTH;
		context->i_cipher = (rand_seed*) malloc(sizeof(rand_seed));
		context->o_cipher = (rand_seed*) malloc(sizeof(rand_seed));
		init_rand(context->i_cipher);
		memcpy(context->o_cipher, context->i_cipher, sizeof(rand_seed));
	} else if (socket > 0) { // for proxy > 0
		context->size = DATA_LENGTH;
	} else { // for socket < 0
		context->size = 0;
	}
	context->draft.magic = MAGIC_NUMBER;
	context->draft.ipv4 = ipv4;
	context->draft.port = port;

	*(multiplex_context_array + multiplex_context_size) = context;
	++multiplex_context_size;
	sort_multiplex_context_array();

	return context;
}

void socket_broker::delete_multiplex_context(multiplex_context *context) {
	multiplex_context **index = this->search_multiplex_context(context->fd, context->draft.ipv4, context->draft.port);

	*index = multiplex_context_array[multiplex_context_size - 1];
	--multiplex_context_size;
	sort_multiplex_context_array();

	if (context->i_cipher != NULL) {
		free(context->i_cipher);
	}
	if (context->o_cipher != NULL) {
		free(context->o_cipher);
	}
	free(context);
}

void socket_broker::close_multiplex_context(multiplex_context *context) {

	if (context->socket <= 0) {
		close(context->fd);
	}
	delete_multiplex_context(context);

}

void socket_broker::close_multiplex_tunnel(multiplex_context *tunnel) {
	int sd, fd = tunnel->fd;
	multiplex_context **iter = NULL;

	for (;;) {
		iter = search_multiplex_context_by_fd(fd);
		if (iter == NULL) {
			break;
		}
		sd = (*iter)->socket;
		if (sd < 0) {
			log_printf("close_multiplex_tunnel [panic] - fd : %d, socket : %d", fd, sd);
			abort();
		}
		close_multiplex_context(*iter);
		if (sd > 0) {
			multiplex_context **socket = search_multiplex_context_by_fd(sd);

			if (socket != NULL) {
				close_multiplex_context(*socket);
			} else {
				close(sd);
				log_printf("[panic] - bad proxy, fd : %d, socket : %d", fd, sd);
			}
		}
	}
}

int socket_broker::close_all_multiplex_context() {
	int n = 0;

	while (multiplex_context_size > 0) {
		multiplex_context *context = *multiplex_context_array;
		log_printf("loop close context, fd : %d, socket : %d, - ipv4 : %u, port : %hu", context->fd, context->socket, context->draft.ipv4, context->draft.port);
		close_multiplex_context(context);
		++n;
	}

	return n;
}

multiplex_context* socket_broker::find_multiplex_context_by_fd(int fd) {
	multiplex_context **index = search_multiplex_context_by_fd(fd);

	if (index != NULL) {
		return *index;
	}

	return NULL;
}

void socket_broker::close_fd_delete_context(int fd) {
	multiplex_context *context = find_multiplex_context_by_fd(fd);

	close(fd);
	if (context != NULL) {
		delete_multiplex_context(context);
	} else {
		log_printf("=== panic === close_fd_delete_context lost? fd = %d", fd);
	}
}

int socket_broker::close_all_fd_delete_all_context() {
	int count = 0;

	while (get_size() > 0) {
		multiplex_context *iter = *get_array();

		close_fd_delete_context(iter->fd);
		++count;
	}

	log_printf("close_all_fd_delete_all_context count = %d!", count);
	return count;
}

