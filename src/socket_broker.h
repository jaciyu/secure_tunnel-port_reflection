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

#ifndef SOCKET_BROKER_H_
#define SOCKET_BROKER_H_

#include <stdlib.h>

#include "multiplex.h"

class socket_broker {

public:

	socket_broker(int capacity);
	virtual ~socket_broker() {
	}

	multiplex_context* new_multiplex_context(int fd, int socket, unsigned int ipv4, unsigned short port);
	multiplex_context** search_multiplex_context_by_fd(int fd);
	multiplex_context** search_multiplex_context(int fd, unsigned int ipv4, unsigned short port);
	void delete_multiplex_context(multiplex_context *context);
	void close_multiplex_context(multiplex_context *context);
	void close_multiplex_tunnel(multiplex_context *tunnel);
	int close_all_multiplex_context();

	multiplex_context* find_multiplex_context_by_fd(int fd);
	void close_fd_delete_context(int fd);
	int close_all_fd_delete_all_context();

	inline int get_size() {
		return multiplex_context_size;
	}

	inline multiplex_context** get_array() {
		return multiplex_context_array;
	}

protected:

	socket_broker(socket_broker& other) {
		multiplex_context_capacity = 0;
		multiplex_context_size = 0;
		multiplex_context_array = NULL;
	}
	socket_broker& operator=(const socket_broker& other) {
		return *this;
	}

	void sort_multiplex_context_array();

private:

	int multiplex_context_capacity;
	int multiplex_context_size;
	multiplex_context** multiplex_context_array;

};

#endif /* SOCKET_BROKER_H_ */

