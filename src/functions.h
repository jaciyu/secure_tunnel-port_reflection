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

