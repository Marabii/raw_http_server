#pragma once

#include <stddef.h>
#include <stdint.h>

#include "server/runtime/scheduler.h"
#include "server/core/bytebuf.h"

typedef struct
{
    uint8_t type;
    size_t len; // data length.
    uint8_t *data;
    uint8_t *raw_buffer_start;
} DBResponse;

// --- DB IO helpers ---
void free_DBResponse(DBResponse *db_response);
DBResponse *read_db_response(int fd);
DBResponse *read_db_response_async(ContextWrapper *context_wrapper);
int send_db_request(int db_fd, ByteBuffer *message);
int send_db_request_async(ContextWrapper *context_wrapper, ByteBuffer *message);

// --- Byte buffer helper functions. They use Big Endian byte order ---
ByteBuffer *msg_begin(char type, size_t initial_capacity);
// Writes the length of the db message at pos (either the start: pos = 0 or after the type byte: pos = 1)
void bb_write_length(ByteBuffer *bb, size_t length, size_t pos);
// Fills in the length reserved by msg_begin (it excludes the type byte).
void msg_end(ByteBuffer *bb);