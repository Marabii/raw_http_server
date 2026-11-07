#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint8_t *data;
    size_t cursor;
    size_t capacity;
} ByteBuffer;

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
int send_db_request(int db_fd, ByteBuffer *message);

// --- Byte buffer helper functions. They use Big Endian byte order ---
char *dup_str(const char *str, size_t len);
ByteBuffer *bb_create(size_t initial_capacity);
void bb_ensure_capacity(ByteBuffer *bb, size_t additional);
void bb_write_u32_be(ByteBuffer *bb, uint32_t val);
void bb_write_length(ByteBuffer *bb, size_t length, size_t pos);
void bb_write_u8(ByteBuffer *bb, uint8_t val);
void bb_write_string(ByteBuffer *bb, const char *str, size_t str_len, int include_null_terminator); // include_null_terminator can only be 0 or 1.
char *bb_detach_cstr(ByteBuffer *bb);                                                               // Appends '\0', returns the data and frees the ByteBuffer wrapper. Caller owns the returned string.
void free_ByteBuffer(ByteBuffer *bb);
ByteBuffer *msg_begin(char type, size_t initial_capacity);
// Fills in the length reserved by msg_begin (it excludes the type byte).
void msg_end(ByteBuffer *bb);
int32_t parse_be32_to_int32_t(const uint8_t *buf);
int16_t parse_be16_to_int16_t(const uint8_t *buf);