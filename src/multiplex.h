//============================================================================
// Author : pengfei.wan
// Date : 2020.02.09
//============================================================================

#ifndef MULTIPLEX_H_
#define MULTIPLEX_H_

#define MAX_EVENTS     ((int) 1024)
#define DATA_LENGTH    ((int) 1000)
#define BUFFER_LENGTH  ((int) sizeof(multiplex_buffer) - sizeof(((multiplex_buffer*) 0)->data))
#define WRAP_LENGTH    ((int) sizeof(multiplex_context) + DATA_LENGTH)
#define PACKAGE_LENGTH ((int) BUFFER_LENGTH + DATA_LENGTH)

#define MAGIC_NUMBER   ((char) 0x81)
#define FLAG_INIT      ((char) 0x1)
#define FLAG_DATA      ((char) 0x2)
#define FLAG_CLOSE     ((char) 0x4)
#define FLAG_PING      ((char) 0x8)
#define FLAG_SIGNIN    ((char) 0x10)
#define FLAG_ACCEPT    ((char) 0x20)

typedef struct rand_seed {
	uint32_t Q[4096];
	uint32_t c;
} rand_seed;

#pragma pack(push)
#pragma pack(1)

typedef struct multiplex_buffer {
	char magic;
	char flags;
	short length;
	unsigned int ipv4;
	unsigned short port;
	unsigned short checksum;
	char data[1];
} multiplex_buffer;

#pragma pack(pop)

typedef struct multiplex_context {
	int fd;
	int size;
	int count;
	int socket;
	rand_seed *i_cipher;
	rand_seed *o_cipher;
	multiplex_buffer draft;
	multiplex_buffer buffer;
} multiplex_context;

typedef struct multiplex_bundle {
	multiplex_context *tunnel;
	multiplex_context *proxy;
	multiplex_context *socket;
} multiplex_bundle;

#endif /* MULTIPLEX_H_ */

