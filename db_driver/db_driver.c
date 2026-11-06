#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "db_driver.h"
#include "conn/conn.h"
#include "auth/auth.h"
#include "db_driver/post_auth_init/read_server_params.h"

DBServerDetails *connect_to_db()
{
    const char *host = "127.0.0.1";
    u_int16_t port = 5432;

    int db_fd = connect_to_tcp(host, port);
    if (db_fd < 0)
    {
        fprintf(stderr, "Failed to create TCP connection with the DB\n");
        return NULL;
    }

    int auth_result = authenticate(db_fd);
    if (auth_result == -1)
    {
        fprintf(stderr, "Failed to authenticate with the database\n");
        goto cleanup;
    }

    DBServerDetails *server_details = read_server_details(db_fd);
    if (server_details == NULL)
    {
        fprintf(stderr, "Failed to read server details\n");
        goto cleanup;
    }

    return server_details;

cleanup:
    close(db_fd);
    return NULL;
}

