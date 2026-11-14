#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "server/pg/pg.h"
#include "server/pg/auth/auth.h"
#include "server/pg/startup.h"
#include "server/pg/wire.h"
#include "server/runtime/event_loop.h"
#include "server/core/bytebuf.h"
#include "utils/utils.h"

// Globals.
DBPool *db_pool = NULL;

// --- Helpers forward declaration ---
static DBServerDetails *connect_to_db(void);
static int connect_to_tcp(const char *host, const uint16_t port);
static void close_connection(DBServerDetails *conn);
static void free_DBServerParam(DBServerParam *server_params);
static void add_to_wait_queue(Task *task);
static Task *pop_from_wait_queue();

// --- Main logic ---
DBPool *db_pool_create(void)
{
    DBPool *pool = calloc(1, sizeof(DBPool));
    if (pool == NULL)
    {
        handle_oom();
    }

    pool->idle_count = 0;

    for (size_t i = 0; i < DB_POOL_SIZE; i++)
    {
        DBServerDetails *conn = connect_to_db();
        if (conn == NULL)
        {
            fprintf(stderr, "Failed to open DB connection %zu of %d\n", i + 1, DB_POOL_SIZE);
            goto cleanup;
        }

        ContextWrapper *db_conn_wrapper = calloc(1, sizeof(ContextWrapper));
        if (db_conn_wrapper == NULL)
        {
            handle_oom();
        }

        conn->context_wrapper = db_conn_wrapper;

        // Mark as non blocking and add it to epoll watch list.
        if (fcntl(conn->db_fd, F_SETFL, O_NONBLOCK) == -1)
        {
            free_DBServerDetails(conn);
            goto cleanup;
        }

        db_conn_wrapper->fd = conn->db_fd;
        db_conn_wrapper->waiting_task = NULL;

        union epoll_data ed_db_conn;
        ed_db_conn.ptr = db_conn_wrapper;

        struct epoll_event ev_db_conn;
        ev_db_conn.events = EPOLLIN;
        ev_db_conn.data = ed_db_conn;

        int epoll_ctl_res_new_conn = epoll_ctl(get_server_instance().epoll_fd, EPOLL_CTL_ADD, conn->db_fd, &ev_db_conn);

        if (epoll_ctl_res_new_conn < 0)
        {
            perror("Failed to add new connection fd to epoll's interest list");
            // dn_conn_wrapper wasn't added to the pool yet, it must be freed separately:
            free_DBServerDetails(conn);
            goto cleanup;
        }

        pool->idle[pool->idle_count++] = conn;
    }

    return pool;

cleanup:
    free_DBPool(pool);
    return NULL;
}

DBServerDetails *db_pool_acquire(DBPool *pool)
{
    while (pool->idle_count == 0)
    {
        // Add to the connections queue:
        add_to_wait_queue(scheduler.current_task);
        swapcontext(&scheduler.current_task->ctx, &scheduler.loop_ctx);
    }

    return pool->idle[--pool->idle_count];
}

void db_pool_release(DBPool *pool, DBServerDetails *conn)
{
    if (conn == NULL)
    {
        return;
    }

    if (pool->idle_count == DB_POOL_SIZE)
    {
        fprintf(stderr, "Released a connection that does not belong to the pool\n");
        return;
    }

    pool->idle[pool->idle_count++] = conn;

    // Check whether there was some other request which couldn't get access to a db connection.
    Task *starved_task = pop_from_wait_queue();
    if (starved_task != NULL)
    {
        schedule_task(starved_task);
    }
}

void free_DBPool(DBPool *pool)
{
    if (pool == NULL)
    {
        return;
    }

    for (size_t i = 0; i < pool->idle_count; i++)
    {
        close_connection(pool->idle[i]);
    }

    free(pool);
}

void free_DBServerDetails(DBServerDetails *server_details)
{
    if (server_details != NULL)
    {
        DBServerParam *param = server_details->params;
        free_ContextWrapper(server_details->context_wrapper);
        while (param != NULL)
        {
            DBServerParam *next = param->next;
            free_DBServerParam(param);
            param = next;
        }
        free(server_details);
    }
}

// --- Helpers ---

// At first, it is set to BLOCKING mode in order to force it to finish the 3 way handshake and also
// because we aren't serving any users at startup...
// We mark it as NONBLOCKING later.
static int connect_to_tcp(const char *host, const uint16_t port)
{
    int conn_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn_fd < 0)
    {
        perror("Socket failed");
        return -1;
    }

    struct sockaddr_in address;
    struct in_addr ipv4_addr;

    int inet_pton_response = inet_pton(AF_INET, host, &ipv4_addr);
    if (inet_pton_response <= 0)
    {
        perror("Invalid IPv4 address format or inet_pton failed");
        close(conn_fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr = ipv4_addr;
    address.sin_port = htons(port);

    if (connect(conn_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("Connection failed");
        close(conn_fd);
        return -1;
    }

    return conn_fd;
}

static DBServerDetails *connect_to_db(void)
{
    const char *host = "127.0.0.1";
    uint16_t port = 5432;

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

static void free_DBServerParam(DBServerParam *server_params)
{
    if (server_params != NULL)
    {
        free(server_params->name);
        free(server_params->val);
        free(server_params);
    }
}

// Sends Terminate ('X') so the server can clean up, then closes the socket.
static void close_connection(DBServerDetails *conn)
{
    ByteBuffer *terminate_msg = bb_create(5);
    bb_write_u8(terminate_msg, (uint8_t)'X');
    bb_write_u32_be(terminate_msg, 4);

    if (send_db_request(conn->db_fd, terminate_msg) < 0)
    {
        fprintf(stderr, "Warning! Failed to close db connection\n");
    }

    free_ByteBuffer(terminate_msg);
    free_DBServerDetails(conn);
}

static void add_to_wait_queue(Task *task)
{
    if (task->state == TASK_WAITING_POOL)
    {
        return; // The task is already enqueued.
    }

    task->state = TASK_WAITING_POOL;
    task->next = NULL;

    if (db_pool->conn_tail != NULL)
    {
        db_pool->conn_tail->next = task;
    }
    else
    {
        db_pool->conn_head = task;
    }

    db_pool->conn_tail = task;
}

static Task *pop_from_wait_queue()
{
    Task *task = db_pool->conn_head;

    if (task == NULL)
    {
        return NULL;
    }

    db_pool->conn_head = task->next;

    if (db_pool->conn_head == NULL)
    {
        db_pool->conn_tail = NULL;
    }

    task->next = NULL;
    return task;
}
