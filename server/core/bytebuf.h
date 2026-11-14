#pragma once

#include <stdint.h>

typedef struct
{
    uint8_t *data;
    size_t cursor;
    size_t capacity;
} ByteBuffer;

ByteBuffer *bb_create(size_t initial_capacity);
// when str_len is 0, it gets replaced with strlen(str).
void bb_write_string(ByteBuffer *bb, const char *str, size_t str_len, int include_null_terminator); // include_null_terminator can only be 0 or 1.
void bb_ensure_capacity(ByteBuffer *bb, size_t additional);
void bb_write_u32_be(ByteBuffer *bb, uint32_t val);
void bb_write_u8(ByteBuffer *bb, uint8_t val);
int32_t parse_be32_to_int32_t(const uint8_t *buf);
int16_t parse_be16_to_int16_t(const uint8_t *buf);
char *bb_detach_cstr(ByteBuffer *bb); // Appends '\0', returns the data and frees the ByteBuffer wrapper. Caller owns the returned string.
void free_ByteBuffer(ByteBuffer *bb);
char *dup_str(const char *str, size_t len);