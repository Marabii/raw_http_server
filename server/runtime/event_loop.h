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

IPv4ConnSock *initialize_server();
int start_event_loop(IPv4ConnSock *conn_sock);
void free_IPv4ConnSock(IPv4ConnSock *conn);

Server get_server_instance();