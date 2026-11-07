#include <stdlib.h>
#include <stdio.h>

#include "http_server.h"
#include "db_driver/db_driver.h"
#include "db_driver/db_requests/db_requests.h"
#include "conn/conn.h"

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

    DBServerDetails *db_conn = db_pool_acquire(db_pool);
    SqlTable *response = run_query(db_conn, "SELECT * FROM users;");

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