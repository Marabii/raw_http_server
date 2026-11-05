#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "db_driver.h"
#include "db_driver_helpers.h"
#include "../conn/conn.h"
#include "../utils/utils.h"
#include "../http_server.h"

// --- structs and enums ---
typedef enum
{
    AuthenticationOk = 0,
    AuthenticationCleartextPassword = 3,
    AuthenticationMD5Password = 5,
    AuthenticationSASL = 10,
    AuthenticationSASLContinue = 11,
    AuthenticationSASLFinal = 12
} AuthCode;

typedef struct
{
    uint8_t type;
    size_t len;
    const uint8_t *data;
} DBResponse;

// --- Helpers ---
DBResponse *parse_db_response(const uint8_t *buf);
ByteBuffer *create_startup_message();

// --- Main logic ---
int connect_to_db()
{
    const char *host = "127.0.0.1";
    u_int16_t port = 5432;

    int db_fd = connect_to_tcp(host, port);
    if (db_fd < 0)
    {
        fprintf(stderr, "Failed to create TCP connection with the DB");
        return -1;
    }

    // Send startup message:
    ByteBuffer *message = create_startup_message();
    ssize_t startup_msg_w_res = write(db_fd, message->data, message->cursor + 1); // +1 because cursor starts at 0;
    if (startup_msg_w_res == -1)
    {
        perror("Failed to send startup message to the database");
        goto cleanup;
    }

    // Receive response:
    char read_buffer[READ_BUFFER];
    ssize_t startup_msg_r_res = read(db_fd, read_buffer, READ_BUFFER - 1);
    if (startup_msg_r_res == 0)
    {
        // client dropped connection:
        goto cleanup;
    }
    else if (startup_msg_r_res < 0)
    {
        perror("Failed to read startup response from the database");
        goto cleanup;
    }

    printf("received from db: %s\n", read_buffer);

    return 0;

cleanup:
    close(db_fd);
    bb_free(message);
    return -1;
}

ByteBuffer *create_startup_message()
{
    const char *user = "marabii";
    const char *db_name = "mydb";

    ByteBuffer *message_buffer = bb_create(40);

    // Reserve the first 4 bytes for the length, which we fill in at the end.
    // Only move the cursor: data must stay the pointer returned by malloc/realloc.
    bb_ensure_capacity(message_buffer, 4);
    message_buffer->cursor += 4;

    // Write protocol version:
    bb_write_u32_be(message_buffer, 196608); // Translates to 0x00 0x03 0x00 0x00 => v3.0

    // Write user (key, then value):
    bb_write_string(message_buffer, "user");
    bb_write_string(message_buffer, user);

    // Write database name:
    bb_write_string(message_buffer, "database");
    bb_write_string(message_buffer, db_name);

    // Add null terminator:
    bb_write_u8(message_buffer, '\0');

    // Add the length back:
    size_t length = message_buffer->cursor;
    bb_write_length(message_buffer, length, 0);

    return message_buffer;
}

// The data is null terminated.
int parse_auth_response(uint8_t *data)
{
    DBResponse *db_response = parse_db_response(data);
    if (db_response == NULL)
    {
        goto cleanup; // free(NULL) is no-op
    }

    if (db_response->type != (uint8_t)'R')
    {
        goto cleanup;
    }

    size_t auth_code = parse_be32_to_sizet(db_response->data);

    switch (auth_code)
    {
    case AuthenticationOk:
        printf("Received AuthenticationOk\n");
        break;

    case AuthenticationCleartextPassword:
        printf("Received AuthenticationCleartextPassword\n");
        break;

    case AuthenticationMD5Password:
        printf("Received AuthenticationMD5Password\n");
        break;

    case AuthenticationSASL:
        printf("Received AuthenticationSASL\n");
        break;

    case AuthenticationSASLContinue:
        printf("Received AuthenticationSASLContinue\n");
        break;

    case AuthenticationSASLFinal:
        printf("Received AuthenticationSASLFinal\n");
        break;

    default:
        printf("Received unknown authentication code: %zu\n", auth_code);
        goto cleanup;
    }

cleanup:
    free(db_response);
    return -1;
}

// --- helpers ---
DBResponse *parse_db_response(const uint8_t *data)
{
    if (*data == '\0')
    {
        return NULL;
    }

    uint8_t type = *data;
    data++; // advance past the type msg.

    size_t len = get_buff_len(data);

    if (len < 4)
    {
        return NULL;
    }

    size_t message_length = parse_be32_to_sizet(data);
    if (message_length != len)
    {
        return NULL;
    }

    data += 4;

    DBResponse *db_response = (DBResponse *)malloc(sizeof(DBResponse));
    if (db_response == NULL)
    {
        handle_oom();
    }

    db_response->data = data;
    db_response->len = message_length - 4; // 4 bytes for the length uint32_t
    db_response->type = type;

    return db_response;
}