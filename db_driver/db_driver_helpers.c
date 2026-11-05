#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../utils/utils.h"
#include "db_driver_helpers.h"

ByteBuffer *bb_create(size_t initial_capacity)
{
    ByteBuffer *bb = malloc(sizeof(ByteBuffer));
    if (!bb)
    {
        handle_oom();
    }

    bb->data = malloc(initial_capacity);
    if (!bb->data)
    {
        free(bb);
        handle_oom();
    }

    bb->cursor = 0;
    bb->capacity = initial_capacity;
    return bb;
}

void bb_ensure_capacity(ByteBuffer *bb, size_t additional)
{
    if (bb->cursor + additional > bb->capacity)
    {
        while (bb->cursor + additional > bb->capacity)
        {
            bb->capacity *= 2; // Double capacity when full
        }

        uint8_t *intermediate = realloc(bb->data, bb->capacity);
        if (intermediate == NULL)
        {
            bb_free(bb);
            handle_oom();
        }
        bb->data = intermediate;
    }
}

// Write 4 bytes (protocol version) in Big-Endian
void bb_write_u32_be(ByteBuffer *bb, uint32_t val)
{
    bb_ensure_capacity(bb, 4);
    bb->data[bb->cursor++] = (uint8_t)((val >> 24) & 0xFF);
    bb->data[bb->cursor++] = (uint8_t)((val >> 16) & 0xFF);
    bb->data[bb->cursor++] = (uint8_t)((val >> 8) & 0xFF);
    bb->data[bb->cursor++] = (uint8_t)(val & 0xFF);
}

// Messages either have the length set either at pos 0 or pos 1
void bb_write_length(ByteBuffer *bb, size_t length, size_t pos)
{
    bb->data[pos++] = (uint8_t)((length >> 24) & 0xFF);
    bb->data[pos++] = (uint8_t)((length >> 16) & 0xFF);
    bb->data[pos++] = (uint8_t)((length >> 8) & 0xFF);
    bb->data[pos++] = (uint8_t)(length & 0xFF);
}

// Write 1 byte
void bb_write_u8(ByteBuffer *bb, uint8_t val)
{
    bb_ensure_capacity(bb, 1);
    bb->data[bb->cursor++] = val;
}

void bb_write_string(ByteBuffer *bb, const char *str)
{
    size_t len = strlen(str) + 1; // +1 includes the '\0' terminator
    bb_ensure_capacity(bb, len);
    memcpy(&bb->data[bb->cursor], str, len);
    bb->cursor += len;
}

void bb_free(ByteBuffer *bb)
{
    if (bb)
    {
        free(bb->data);
        free(bb);
    }
}

size_t parse_be32_to_sizet(const uint8_t *buf)
{
    return ((size_t)buf[0] << 24) |
           ((size_t)buf[1] << 16) |
           ((size_t)buf[2] << 8) |
           ((size_t)buf[3]);
}

size_t get_buff_len(const uint8_t *buf)
{
    size_t len = 0;
    for (size_t i = 0; buf[i] != '\0'; i++)
    {
        len++;
    }

    return len;
}