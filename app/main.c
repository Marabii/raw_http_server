#include <stdlib.h>
#include <stdio.h>

#include "server/runtime/event_loop.h"
#include "server/pg/pg.h"

#include "app/shortner.h"
#include "app/health.h"

#include "env_parser/env.h"

int main()
{
    // Load environment variables:
    load_env();

    IPv4ConnSock *conn_sock = initialize_server();
    Server server = get_server_instance();

    if (conn_sock == NULL || server.epoll_fd == -1 || server.server_fd == -1)
    {
        fprintf(stderr, "Server initialization failed\n");
        free_IPv4ConnSock(conn_sock);
        return EXIT_FAILURE;
    }

    db_pool = db_pool_create();
    if (db_pool == NULL)
    {
        return EXIT_FAILURE;
    }

    add_POST("/shorten-url", shorten_url);
    add_GET("/get-shortened-url/:id", read_original_url);
    add_GET("/health", health_check);

    int event_loop_res = start_event_loop(conn_sock);
    if (event_loop_res == -1)
    {
        fprintf(stderr, "Failed to start event loop\n");
        free_IPv4ConnSock(conn_sock);
        free_DBPool(db_pool);
        return EXIT_FAILURE;
    }

    free_DBPool(db_pool);
    free_Env();

    return 0;
}