#pragma once

#include <netinet/in.h>

typedef struct
{
    struct sockaddr_in address;
    int conn_fd;
    socklen_t addrlen;
} IPv4ConnSock;

typedef struct
{
    int epoll_fd;
    int server_fd;
} Server;

int connect_to_tcp(const char *host, const uint16_t port);

IPv4ConnSock *start_tcp_server(const char *host, const uint16_t port);
void free_conn(IPv4ConnSock *conn);

IPv4ConnSock *initialize_server();
int start_event_loop(IPv4ConnSock *conn_sock);

Server get_server_instance();