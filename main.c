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
#include <time.h>
#include <locale.h>

#include "http_server.h"
#include "headers_parser.h"

#define PORT 8080
#define MAX_EVENTS 10

char *format_response(size_t *size);
char *read_html_file(FILE *fp, size_t *sz);

int main()
{
    int server_fd = -1;
    int epoll_fd = -1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        perror("Socket failed");
        goto cleanup;
    }

    int option = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) == -1)
    {
        perror("Failed to change socket option to SO_REUSEADDR");
        goto cleanup;
    }

    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    struct in_addr ipv4_addr;

    int res = inet_pton(AF_INET, "127.0.0.1", &ipv4_addr);
    if (res == 0)
    {
        perror("Invalid IPv4 address format");
        goto cleanup;
    }
    else if (res < 0)
    {
        perror("Something wrong with inet_pton");
        goto cleanup;
    }

    memset(&address, 0, addrlen);
    address.sin_family = AF_INET;
    address.sin_addr = ipv4_addr;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, addrlen) < 0)
    {
        perror("bind failed");
        goto cleanup;
    }

    if (listen(server_fd, 128) < 0)
    {
        perror("Server failed to start listening");
        goto cleanup;
    }

    printf("Server is listening on port %d\n", PORT);

    // Starting the event loop:
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {
        perror("Failed to create an epoll instance");
        goto cleanup;
    }

    union epoll_data ed;
    ed.fd = server_fd;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data = ed;

    int epoll_ctl_res = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    if (epoll_ctl_res < 0)
    {
        perror("Failed to add the listening socket to the epoll interest list");
        goto cleanup;
    }

    struct epoll_event events[MAX_EVENTS];
    char read_buffer[READ_BUFFER];

    for (;;)
    {
        int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1); // Set to blocking mode. timeout = infinite.

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
            if (event.data.fd == server_fd)
            {

                // Socket is marked as non-blocking to not block the entire event loop.
                int new_conn_fd = accept4(server_fd, (struct sockaddr *)&address, &addrlen, SOCK_NONBLOCK);

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

                int epoll_ctl_res_new_conn = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_conn_fd, &ev_new_conn);

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

    close(epoll_fd);
    close(server_fd);

    return 0;

cleanup:
    if (epoll_fd >= 0)
    {
        close(epoll_fd);
    }

    if (server_fd >= 0)
    {
        close(server_fd);
    }

    return 1;
}

char *format_response(size_t *size)
{
    FILE *fp = fopen("./html/response.html", "r");

    if (fp == NULL)
    {
        perror("Error reading response.html file");
        return NULL;
    }

    size_t html_response_size = 0;
    char *html_response = read_html_file(fp, &html_response_size);

    if (html_response == NULL)
    {
        return NULL;
    }

    char *response = (char *)malloc(WRITE_BUFFER);

    if (response == NULL)
    {
        perror("Error creating write buffer");
        return NULL;
    }

    // Writing response code:
    *size += sprintf(response + *size, "HTTP/1.1 200 OK\r\n");

    // Writing curr date:
    time_t now = time(NULL);
    struct tm gmt_time;
    char date_buff[38]; // Date header is always 37 bytes including the ending \r\n. (+1 for the null byte which gets overriten)
    gmtime_r(&now, &gmt_time);

    locale_t c_locale = newlocale(LC_TIME_MASK, "C", (locale_t)0);
    strftime_l(date_buff, sizeof(date_buff), "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &gmt_time, c_locale);
    freelocale(c_locale);
    *size += sprintf(response + *size, "%s", date_buff);

    // Writing server details:
    *size += sprintf(response + *size, "Content-Type: text/html; charset=UTF-8\r\n");

    // Writing the response length:
    *size += sprintf(response + *size, "Content-Length: %zu\r\n", html_response_size);

    // Specify connection type:
    *size += sprintf(response + *size, "Connection: close\r\n\r\n");

    if (*size + html_response_size + 1 > WRITE_BUFFER)
    {
        char *intermediate = realloc(response, *size + html_response_size + 1);

        if (intermediate == NULL)
        {
            perror("Failed to expand response buffer");
            free(response);
            return NULL;
        }

        response = intermediate;
    }

    memcpy(response + *size, html_response, html_response_size);

    *size += html_response_size;
    response[*size] = '\0';

    return response;
}

char *read_html_file(FILE *fp, size_t *sz)
{
    if (!fp)
    {
        return NULL;
    }

    // Move to the end to get file length
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        return NULL;
    }

    long file_size = ftell(fp);
    if (file_size < 0)
    {
        return NULL;
    }

    *sz = (size_t)file_size;

    // Rewind back to the beginning
    rewind(fp);

    char *buffer = (char *)malloc((size_t)file_size);
    if (!buffer)
    {
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)file_size, fp);
    if (bytes_read != (size_t)file_size && ferror(fp))
    {
        free(buffer);
        return NULL;
    }

    return buffer;
}