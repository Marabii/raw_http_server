#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "server/core/bytebuf.h"
#include "utils/utils.h"

// --- Byte buffer helper functions. They use Big Endian byte order ---
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
            free_ByteBuffer(bb);
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

// Write 1 byte
void bb_write_u8(ByteBuffer *bb, uint8_t val)
{
    bb_ensure_capacity(bb, 1);
    bb->data[bb->cursor++] = val;
}

void bb_write_string(ByteBuffer *bb, const char *str, size_t str_len, int include_null_terminator)
{
    if (str_len == 0)
    {
        str_len = strlen(str);
    }

    size_t len = str_len + include_null_terminator; // +1 includes the '\0' terminator
    bb_ensure_capacity(bb, len);
    memcpy(&bb->data[bb->cursor], str, len);
    bb->cursor += len;
}

char *bb_detach_cstr(ByteBuffer *bb)
{
    bb_write_u8(bb, '\0');
    char *str = (char *)bb->data;
    free(bb);
    return str;
}

void free_ByteBuffer(ByteBuffer *bb)
{
    if (bb)
    {
        free(bb->data);
        free(bb);
    }
}

int16_t parse_be16_to_int16_t(const uint8_t *buf)
{
    uint16_t u = ((uint16_t)buf[0] << 8) |
                 ((uint16_t)buf[1]);
    return (int16_t)u;
}

int32_t parse_be32_to_int32_t(const uint8_t *buf)
{
    uint32_t u = ((uint32_t)buf[0] << 24) |
                 ((uint32_t)buf[1] << 16) |
                 ((uint32_t)buf[2] << 8) |
                 ((uint32_t)buf[3]);
    return (int32_t)u;
}

// Copies len bytes of str into a new null terminated string.
char *dup_str(const char *str, size_t len)
{
    ByteBuffer *bb = bb_create(len + 1);
    bb_write_string(bb, str, len, 0);
    return bb_detach_cstr(bb);
}