#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "db_driver/helpers/helpers.h"
#include "db_driver/auth/auth_helpers.h"

ByteBuffer *create_startup_message()
{
    const char *user = "marabii";
    const char *db_name = "mydb";

    ByteBuffer *message_buffer = bb_create(40);

    // Reserve the first 4 bytes for the length, which we fill in at the end.
    bb_ensure_capacity(message_buffer, 4);
    message_buffer->cursor += 4;

    // Write protocol version:
    bb_write_u32_be(message_buffer, 196608); // Translates to 0x00 0x03 0x00 0x00 => v3.0

    // Write user (key, then value):
    bb_write_string(message_buffer, "user", strlen("user"), 1);
    bb_write_string(message_buffer, user, strlen(user), 1);

    // Write database name:
    bb_write_string(message_buffer, "database", strlen("database"), 1);
    bb_write_string(message_buffer, db_name, strlen(db_name), 1);

    // Add null terminator:
    bb_write_u8(message_buffer, '\0');

    // Add the length back:
    size_t length = message_buffer->cursor;
    bb_write_length(message_buffer, length, 0);

    return message_buffer;
}

// The data must be null terminated.
int parse_auth_response(DBResponse *db_response)
{
    if (db_response == NULL)
    {
        return -1;
    }

    if (db_response->type != (uint8_t)'R')
    {
        return -1;
    }

    size_t auth_code = (size_t)parse_be32_to_int32_t(db_response->data);

    switch (auth_code)
    {
    case PSQL_AuthenticationOk:
        printf("Received AuthenticationOk\n");
        return PSQL_AuthenticationOk;

    case PSQL_AuthenticationCleartextPassword:
        printf("Received AuthenticationCleartextPassword\n");
        return PSQL_AuthenticationCleartextPassword;

    case PSQL_AuthenticationMD5Password:
        printf("Received AuthenticationMD5Password\n");
        return PSQL_AuthenticationMD5Password;

    case PSQL_AuthenticationSASL:
        printf("Received AuthenticationSASL\n");
        return PSQL_AuthenticationSASL;

    case PSQL_AuthenticationSASLContinue:
        printf("Received AuthenticationSASLContinue\n");
        return PSQL_AuthenticationSASLContinue;

    case PSQL_AuthenticationSASLFinal:
        printf("Received AuthenticationSASLFinal\n");
        return PSQL_AuthenticationSASLFinal;

    default:
        fprintf(stderr, "Received unknown authentication code: %zu\n", auth_code);
        return -1;
    }
}
