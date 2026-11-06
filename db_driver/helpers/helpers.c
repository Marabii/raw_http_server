#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "http_server.h"
#include "utils/utils.h"
#include "db_driver/helpers/helpers.h"

int send_db_request(int db_fd, ByteBuffer *message)
{
    int bytes_written = write(db_fd, message->data, message->cursor);

    if (bytes_written == -1)
    {
        perror("Failed to send data to the database");
        return -1;
    }

    return 0;
}

// Reads exactly n bytes from fd into buf.
static int read_exact(int fd, uint8_t *buf, size_t n)
{
    size_t total = 0;

    while (total < n)
    {
        ssize_t bytes_read = read(fd, buf + total, n - total);
        if (bytes_read == 0)
        {
            return 0;
        }

        if (bytes_read < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }

        total += (size_t)bytes_read;
    }

    return 1;
}

DBResponse *read_db_response(int fd)
{
    uint8_t *raw_buffer_start = malloc(5); // Allocate 5 bytes just for the type and the message length.
    if (raw_buffer_start == NULL)
    {
        handle_oom();
    }

    // Read the type and length.
    int res = read_exact(fd, raw_buffer_start, 5);

    if (res == 0)
    {
        fprintf(stderr, "Connection is closed\n");
        goto cleanup;
    }

    if (res < 0)
    {
        perror("Failed to read db response");
        goto cleanup;
    }

    // Get the type:
    uint8_t type = raw_buffer_start[0];

    // parse the length:
    size_t len = parse_be32_to_sizet(raw_buffer_start + 1);

    if (len > MAXIMUM_ALLOWED_PAYLOAD)
    {
        fprintf(stderr, "Maximum payload allowed is: %d\n", MAXIMUM_ALLOWED_PAYLOAD);
        goto cleanup;
    }

    if (len < 4)
    {
        fprintf(stderr, "Received ambigous message\n");
        goto cleanup;
    }

    uint8_t *intermediate = realloc(raw_buffer_start, len + 2); // len contains 4 bytes of the length int itself + payload. 1 is added for the type and another 1 for \0.
    if (intermediate == NULL)
    {
        handle_oom();
    }

    raw_buffer_start = intermediate;

    // Read the payload. When len == 4 the payload is empty and read_exact returns immediately.
    res = read_exact(fd, raw_buffer_start + 5, len - 4);

    if (res == 0)
    {
        fprintf(stderr, "Connection is closed\n");
        goto cleanup;
    }

    if (res < 0)
    {
        perror("Failed to read db response");
        goto cleanup;
    }

    raw_buffer_start[len + 1] = '\0';

    uint8_t *data = raw_buffer_start + 5; // 1 for type, 4 for length.

    // build response:
    DBResponse *db_response = malloc(sizeof(DBResponse));
    if (db_response == NULL)
    {
        handle_oom();
    }

    db_response->data = data;
    db_response->len = len - 4;
    db_response->type = type;
    db_response->raw_buffer_start = raw_buffer_start;

    return db_response;

cleanup:
    free(raw_buffer_start);
    return NULL;
}

void free_DBResponse(DBResponse *db_response)
{
    free(db_response->raw_buffer_start);
    db_response->raw_buffer_start = NULL;
    db_response->data = NULL;
    free(db_response);
}

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

void bb_write_string(ByteBuffer *bb, const char *str, size_t str_len, int include_null_terminator)
{
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

size_t parse_be32_to_sizet(const uint8_t *buf)
{
    return ((size_t)buf[0] << 24) |
           ((size_t)buf[1] << 16) |
           ((size_t)buf[2] << 8) |
           ((size_t)buf[3]);
}

// Copies len bytes of str into a new null terminated string.
char *dup_str(const char *str, size_t len)
{
    ByteBuffer *bb = bb_create(len + 1);
    bb_write_string(bb, str, len, 0);
    return bb_detach_cstr(bb);
}