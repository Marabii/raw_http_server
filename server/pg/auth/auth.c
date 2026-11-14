#include <stdlib.h>
#include <stdio.h>

#include "server/pg/auth/scram-sha-256/mechanism.h"
#include "server/pg/auth/auth_helpers.h"
#include "server/core/bytebuf.h"
#include "server/pg/wire.h"

int authenticate(int db_fd)
{
    // Send startup message:
    ByteBuffer *startup_message = create_startup_message();
    ssize_t startup_msg_res = send_db_request(db_fd, startup_message);

    if (startup_msg_res == -1)
    {
        perror("Failed to send startup message to the database");
        free_ByteBuffer(startup_message);
        return -1;
    }

    free_ByteBuffer(startup_message);

    // Read startup message response:
    DBResponse *auth_db_response = read_db_response(db_fd);
    if (auth_db_response == NULL)
    {
        return -1;
    }

    AuthCode auth_code_res = parse_auth_response(auth_db_response);
    if (auth_code_res == PSQL_UnknownAuthCode)
    {
        free_DBResponse(auth_db_response);
        return -1;
    }

    free_DBResponse(auth_db_response);

    AuthCode auth_code = (AuthCode)auth_code_res;

    switch (auth_code)
    {
    case PSQL_AuthenticationSASL:
        return authenticate_scram_sha_256(db_fd);

    default:
        fprintf(stderr, "Auth mechanism isn't supported yet.");
        return -1;
    }
}
