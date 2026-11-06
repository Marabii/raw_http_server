#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "db_driver/db_requests/db_requests.h"
#include "db_driver/helpers/helpers.h"
#include "db_driver/post_auth_init/read_server_params.h"

// Sends Terminate ('X') so the server can clean up, then closes the socket.
void close_connection(DBServerDetails *conn)
{
    ByteBuffer *terminate_msg = bb_create(5);
    bb_write_u8(terminate_msg, (uint8_t)'X');
    bb_write_u32_be(terminate_msg, 4);

    if (send_db_request(conn->db_fd, terminate_msg) < 0)
    {
        fprintf(stderr, "Warning! Failed to close db connection\n");
    }

    free_ByteBuffer(terminate_msg);
    close(conn->db_fd);
    free_DBServerDetails(conn);
}
