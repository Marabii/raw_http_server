#include <stdlib.h>
#include <stdio.h>

#include "http_server.h"
#include "./db_driver/db_driver.h"
#include "./conn/conn.h"

int main()
{
    IPv4ConnSock *conn_sock = initialize_server();
    Server server = get_server_instance();

    if (conn_sock == NULL || server.epoll_fd == -1 || server.server_fd == -1)
    {
        fprintf(stderr, "Server initialization failed\n");
        return EXIT_FAILURE;
    }

    DBPool *db_pool = db_pool_create();
    if (db_pool == NULL)
    {
        return EXIT_FAILURE;
    }

    int event_loop_res = start_event_loop(conn_sock);
    if (event_loop_res == -1)
    {
        fprintf(stderr, "Failed to start event loop\n");
        db_pool_destroy(db_pool);
        return EXIT_FAILURE;
    }

    db_pool_destroy(db_pool);

    return 0;
}