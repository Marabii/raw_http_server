#pragma once

typedef struct
{
    uint8_t *data;
    size_t cursor;
    size_t capacity;
} ByteBuffer;

ByteBuffer *bb_create(size_t initial_capacity);
void bb_ensure_capacity(ByteBuffer *bb, size_t additional);
void bb_write_u32_be(ByteBuffer *bb, uint32_t val);
void bb_write_length(ByteBuffer *bb, size_t length, size_t pos);
void bb_write_u8(ByteBuffer *bb, uint8_t val);
void bb_write_string(ByteBuffer *bb, const char *str);
void bb_free(ByteBuffer *bb);
size_t parse_be32_to_sizet(const uint8_t *buf);
size_t get_buff_len(const uint8_t *buf);