#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "server/pg/wire.h"
#include "utils/utils.h"
#include "server/runtime/scheduler.h"
#include "server/core/bytebuf.h"
#include "server/core/config.h"

// --- Helpers forward declaration ---
static int read_exact_async(ContextWrapper *context_wrapper, uint8_t *buf, size_t n);
static int read_exact(int fd, uint8_t *buf, size_t n);

// --- Main logic ---

// This is asynchronous.
int send_db_request_async(ContextWrapper *context_wrapper, ByteBuffer *message)
{
    size_t to_write = message->cursor;
    const char *buffer = (const char *)message->data;

    if (async_write(context_wrapper, (const uint8_t *)buffer, to_write) <= 0)
    {
        return -1;
    }

    return 0;
}

// This is blocking.
int send_db_request(int db_fd, ByteBuffer *message)
{
    size_t total_written = 0;
    size_t to_write = message->cursor;
    const char *buffer = (const char *)message->data;

    while (total_written < to_write)
    {
        ssize_t bytes_written = write(db_fd, buffer + total_written, to_write - total_written);

        if (bytes_written == -1)
        {
            perror("Failed to send data to the database");
            return -1;
        }

        if (bytes_written == 0)
        {
            fprintf(stderr, "Connection dropped\n");
            return -1;
        }

        total_written += (size_t)bytes_written;
    }

    return 0;
}

DBResponse *read_db_response_async(ContextWrapper *context_wrapper)
{
    uint8_t *raw_buffer_start = malloc(5); // Allocate 5 bytes just for the type and the message length.
    if (raw_buffer_start == NULL)
    {
        handle_oom();
    }

    // Read the type and length.
    int res = read_exact_async(context_wrapper, raw_buffer_start, 5);

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
    size_t len = (size_t)parse_be32_to_int32_t(raw_buffer_start + 1);

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
    res = read_exact_async(context_wrapper, raw_buffer_start + 5, len - 4);

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
    size_t len = (size_t)parse_be32_to_int32_t(raw_buffer_start + 1);

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

// --- Byte buffer helper functions for db messages ---
ByteBuffer *msg_begin(char type, size_t initial_capacity)
{
    ByteBuffer *bb = bb_create(initial_capacity);
    bb_write_u8(bb, (uint8_t)type);
    bb_write_u32_be(bb, 0);
    return bb;
}

// Messages either have the length set either at pos 0 or pos 1
void bb_write_length(ByteBuffer *bb, size_t length, size_t pos)
{
    bb->data[pos++] = (uint8_t)((length >> 24) & 0xFF);
    bb->data[pos++] = (uint8_t)((length >> 16) & 0xFF);
    bb->data[pos++] = (uint8_t)((length >> 8) & 0xFF);
    bb->data[pos++] = (uint8_t)(length & 0xFF);
}

// Fills in the length reserved by msg_begin (it excludes the type byte).
void msg_end(ByteBuffer *bb)
{
    bb_write_length(bb, bb->cursor - 1, 1);
}

// --- Helpers ---

// Reads exactly n bytes from fd into buf asynchronously.
static int read_exact_async(ContextWrapper *context_wrapper, uint8_t *buf, size_t n)
{
    size_t total = 0;

    while (total < n)
    {
        ssize_t bytes_read = async_read(context_wrapper, buf + total, n - total);
        if (bytes_read == 0)
        {
            return 0;
        }

        if (bytes_read < 0)
        {
            return -1;
        }

        total += (size_t)bytes_read;
    }

    return 1;
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
            return -1;
        }

        total += (size_t)bytes_read;
    }

    return 1;
}