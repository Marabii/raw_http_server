#define _GNU_SOURCE // Used to access the latest features.

#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ucontext.h>

#include "env_parser/env.h"
#include "server/core/config.h"
#include "server/runtime/event_loop.h"
#include "server/runtime/scheduler.h"
#include "server/http/http.h"
#include "utils/utils.h"

// Globals.
Server server = {.epoll_fd = -1, .server_fd = -1};

// --- Helpers forward declaration ---
static IPv4ConnSock *start_tcp_server(const char *host, const uint16_t port);
static int get_port(void);

// --- Main logic ---
IPv4ConnSock *initialize_server()
{
    int port = get_port();
    if (port == -1)
    {
        return NULL;
    }

    IPv4ConnSock *conn_sock = start_tcp_server("127.0.0.1", get_port());
    if (conn_sock == NULL)
    {
        fprintf(stderr, "Failed to start server\n");
        return NULL;
    }

    server.server_fd = conn_sock->conn_fd;
    printf("Server is listening on port %d\n", get_port());

    server.epoll_fd = epoll_create1(0);
    if (server.epoll_fd < 0)
    {
        perror("Failed to create an epoll instance");
        free_IPv4ConnSock(conn_sock);
        return NULL;
    }

    ContextWrapper *context_wrapper = calloc(1, sizeof(ContextWrapper));
    if (context_wrapper == NULL)
    {
        handle_oom();
    }

    context_wrapper->fd = server.server_fd;

    union epoll_data ed;
    ed.ptr = context_wrapper;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = ed;

    int epoll_ctl_res = epoll_ctl(server.epoll_fd, EPOLL_CTL_ADD, server.server_fd, &ev);

    if (epoll_ctl_res < 0)
    {
        perror("Failed to add the listening socket to the epoll interest list");
        free_IPv4ConnSock(conn_sock);
        return NULL;
    }

    return conn_sock;
}

int start_event_loop(IPv4ConnSock *conn_sock)
{
    struct epoll_event events[MAX_EVENTS];
    struct sockaddr_in address = conn_sock->address;
    socklen_t addrlen = conn_sock->addrlen;

    for (;;)
    {
        // Run all tasks within the ready queue:
        for (;;)
        {
            Task *task = pop_ready_task();
            if (task == NULL)
            {
                break;
            }

            scheduler.current_task = task;
            task->state = TASK_RUNNING;

            // Pause execution here until coroutine yields or finishes:
            swapcontext(&scheduler.loop_ctx, &task->ctx);

            // Finished or yielded:
            scheduler.current_task = NULL;

            if (task->state != TASK_WAITING_IO && task->state != TASK_WAITING_POOL)
            {
                task->state = TASK_DEAD;
                free_Task(task);
            }
        }

        int ready = epoll_wait(server.epoll_fd, events, MAX_EVENTS, -1); // Set to blocking mode. timeout = infinite.

        if (ready < 1)
        {
            if (errno == EINTR)
            {
                printf("Server shutdown\n");
                break;
            }
            else
            {
                perror("Failed to wait for events");
                goto cleanup;
            }
        }

        for (size_t i = 0; i < (size_t)ready; i++)
        {
            struct epoll_event event = events[i];
            ContextWrapper *context_wrapper = (ContextWrapper *)event.data.ptr;

            // Handle new connections:
            if (context_wrapper->fd == server.server_fd)
            {
                // Accept all connetions until hitting EAGAIN or EWOULDBLOCK
                for (;;)
                {
                    // Socket is marked as non-blocking to not block the entire event loop.
                    int new_conn_fd = accept4(server.server_fd, (struct sockaddr *)&address, &addrlen, SOCK_NONBLOCK);

                    if (new_conn_fd < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            // No more connections to accept.
                            break;
                        }
                        else
                        {
                            perror("WARNING! Failed to accept new connection");
                            continue;
                        }
                    }

                    ContextWrapper *new_conn_wrapper = calloc(1, sizeof(ContextWrapper));
                    if (new_conn_wrapper == NULL)
                    {
                        handle_oom();
                    }

                    new_conn_wrapper->fd = new_conn_fd;
                    new_conn_wrapper->waiting_task = NULL;

                    union epoll_data ed_new_conn;
                    ed_new_conn.ptr = new_conn_wrapper;

                    struct epoll_event ev_new_conn;
                    ev_new_conn.events = EPOLLIN;
                    ev_new_conn.data = ed_new_conn;

                    int epoll_ctl_res_new_conn = epoll_ctl(server.epoll_fd, EPOLL_CTL_ADD, new_conn_fd, &ev_new_conn);

                    if (epoll_ctl_res_new_conn < 0)
                    {
                        perror("Failed to add new connection fd to epoll's interest list");
                        free_ContextWrapper(new_conn_wrapper);
                        continue;
                    }

                    Task *task = create_http_client_task(new_conn_wrapper);
                    if (task == NULL)
                    {
                        fprintf(stderr, "Failed to create a task for a connection\n");
                        free_ContextWrapper(new_conn_wrapper);
                        continue;
                    }
                }
            }
            else
            {
                if (context_wrapper->waiting_task != NULL)
                {
                    // context_wrapper->waiting_task must not be null here.
                    // It must have been set at some earlier IO stage.
                    schedule_task(context_wrapper->waiting_task);
                }
            }
        }
    }

    close(server.epoll_fd);
    close(server.server_fd);

    return 0;

cleanup:
    if (server.epoll_fd > 2)
    {
        close(server.epoll_fd);
    }

    if (server.server_fd > 2)
    {
        close(server.server_fd);
    }

    return -1;
}

Server get_server_instance()
{
    return server;
}

// Frees the memory AND CLOSES THE FD.
void free_IPv4ConnSock(IPv4ConnSock *conn)
{
    if (conn != NULL)
    {
        if (conn->conn_fd > 2)
        {
            close(conn->conn_fd);
        }

        free(conn);
    }
}

// --- Helpers ---
static IPv4ConnSock *start_tcp_server(const char *host, const uint16_t port)
{
    IPv4ConnSock *conn_sock = calloc(1, sizeof(IPv4ConnSock));
    if (conn_sock == NULL)
    {
        handle_oom();
    }

    int conn_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    if (conn_fd < 0)
    {
        perror("Failed to create socket.");
        goto free_mem_close_fd;
    }

    int option = 1;
    if (setsockopt(conn_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) == -1)
    {
        perror("Failed to change socket option to SO_REUSEADDR");
        goto free_mem_close_fd;
    }

    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    struct in_addr ipv4_addr;

    int inet_pton_response = inet_pton(AF_INET, host, &ipv4_addr);
    if (inet_pton_response == 0)
    {
        perror("Invalid IPv4 address format");
        goto free_mem_close_fd;
    }

    else if (inet_pton_response < 0)
    {
        perror("Something wrong with inet_pton");
        goto free_mem_close_fd;
    }

    memset(&address, 0, addrlen);
    address.sin_family = AF_INET;
    address.sin_addr = ipv4_addr;
    address.sin_port = htons(port);

    if (bind(conn_fd, (struct sockaddr *)&address, addrlen) < 0)
    {
        perror("bind failed");
        goto free_mem_close_fd;
    }

    if (listen(conn_fd, 128) < 0)
    {
        perror("listen failed");
        goto free_mem_close_fd;
    }

    conn_sock->address = address;
    conn_sock->conn_fd = conn_fd;
    conn_sock->addrlen = addrlen;

    return conn_sock;

free_mem_close_fd:
    free_IPv4ConnSock(conn_sock);
    return NULL;
}

// Fetches and parses the server PORT to an int:
static int get_port(void)
{
    char *port_str = get_env_val("PORT");
    if (port_str == NULL)
    {
        fprintf(stderr, "Couldn't find PORT in .env file\n");
        return -1;
    }

    char *port_endptr;
    errno = 0; // Reset error state before calling
    int port = strtol(port_str, &port_endptr, 10);

    if (port_endptr == port_str)
    {
        fprintf(stderr, "Malformed port: No digits were found.\n");
        return -1;
    }

    if (*port_endptr != '\0')
    {
        fprintf(stderr, "Malformed port: Trailing invalid characters found: %s\n", port_endptr);
        return -1;
    }

    if (errno == ERANGE || port <= 0 || port >= 65535)
    {
        fprintf(stderr, "Number out of range. Valid range is ]0, 65535]\n");
        return -1;
    }

    return port;
}