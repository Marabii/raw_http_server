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

#include "http_server.h"
#include "conn.h"
#include "../responses/response_handler.h"
#include "../headers/headers_parser.h"

Server server = {.epoll_fd = -1, .server_fd = -1};

IPv4ConnSock *start_tcp_server(const char *host, const uint16_t port)
{
    int conn_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (conn_fd < 0)
    {
        perror("Socket failed");
        return NULL;
    }

    int option = 1;
    if (setsockopt(conn_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) == -1)
    {
        perror("Failed to change socket option to SO_REUSEADDR");
        goto close_fd;
    }

    IPv4ConnSock *conn_sock = (IPv4ConnSock *)malloc(sizeof(IPv4ConnSock));
    if (conn_sock == NULL)
    {
        perror("Failed to allocate memory");
        goto close_fd;
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
    free(conn_sock);
close_fd:
    close(conn_fd);
    return NULL;
}

int connect_to_tcp(const char *host, const uint16_t port)
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

    return conn_fd; // Return the connected file descriptor
}

IPv4ConnSock *initialize_server()
{
    IPv4ConnSock *conn_sock = start_tcp_server("127.0.0.1", PORT);
    if (conn_sock == NULL)
    {
        fprintf(stderr, "Failed to start server");
        return NULL;
    }

    server.server_fd = conn_sock->conn_fd;
    printf("Server is listening on port %d\n", PORT);

    // Starting the event loop:
    server.epoll_fd = epoll_create1(0);
    if (server.epoll_fd < 0)
    {
        perror("Failed to create an epoll instance");
        free_conn(conn_sock);
        return NULL;
    }

    union epoll_data ed;
    ed.fd = server.server_fd;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = ed;

    int epoll_ctl_res = epoll_ctl(server.epoll_fd, EPOLL_CTL_ADD, server.server_fd, &ev);

    if (epoll_ctl_res < 0)
    {
        perror("Failed to add the listening socket to the epoll interest list");
        free_conn(conn_sock);
        return NULL;
    }

    return conn_sock;
}

int start_event_loop(IPv4ConnSock *conn_sock)
{
    struct epoll_event events[MAX_EVENTS];
    char read_buffer[READ_BUFFER];
    struct sockaddr_in address = conn_sock->address;
    socklen_t addrlen = conn_sock->addrlen;

    for (;;)
    {
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

            // Handle new connections:
            if (event.data.fd == server.server_fd)
            {
                // Socket is marked as non-blocking to not block the entire event loop.
                int new_conn_fd = accept4(server.server_fd, (struct sockaddr *)&address, &addrlen, SOCK_NONBLOCK);

                if (new_conn_fd < 0)
                {
                    perror("Failed to accept new connection");
                    goto cleanup;
                }

                union epoll_data ed_new_conn;
                ed_new_conn.fd = new_conn_fd;

                struct epoll_event ev_new_conn;
                ev_new_conn.events = EPOLLIN;
                ev_new_conn.data = ed_new_conn;

                int epoll_ctl_res_new_conn = epoll_ctl(server.epoll_fd, EPOLL_CTL_ADD, new_conn_fd, &ev_new_conn);

                if (epoll_ctl_res_new_conn < 0)
                {
                    perror("Failed to add new connection fd to epoll's interest list");
                    goto cleanup;
                }
            }
            else
            {
                ssize_t read_sz = read(event.data.fd, read_buffer, READ_BUFFER - 1); // -1 for the null terminator.

                if (read_sz == 0)
                {
                    close(event.data.fd);
                    continue;
                }

                if (read_sz < 0)
                {
                    // Must handle EGAIN...
                    perror("Failed to read request header");
                    goto cleanup;
                }

                // Set the null byte at the end:
                read_buffer[read_sz] = '\0';

                Headers *headers = parse_headers(read_buffer, read_sz);
                if (headers != NULL)
                {
                    print_http_request(headers);
                }

                size_t response_size = 0;
                char *response = format_response(&response_size);

                if (response == NULL)
                {
                    goto cleanup;
                }

                write(event.data.fd, response, response_size);

                close(event.data.fd);
                free(response);
            }
        }
    }

    close(server.epoll_fd);
    close(server.server_fd);

    return 0;

cleanup:
    if (server.epoll_fd >= 0)
    {
        close(server.epoll_fd);
    }

    if (server.server_fd >= 0)
    {
        close(server.server_fd);
    }

    free_conn(conn_sock);
    return 1;
}

Server get_server_instance()
{
    return server;
}

void free_conn(IPv4ConnSock *conn)
{
    if (conn != NULL)
    {
        close(conn->conn_fd);
        free(conn);
    }
}