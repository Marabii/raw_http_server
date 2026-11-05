#define _GNU_SOURCE // Used to access the latest features.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <locale.h>
#include <sys/stat.h>
#include <string.h>

#include "http_server.h"
#include "response_handler.h"
#include "../utils/utils.h"

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
        handle_oom();
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
            handle_oom();
        }

        response = intermediate;
    }

    memcpy(response + *size, html_response, html_response_size);

    free(html_response);
    html_response = NULL;
    fclose(fp);

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

    int fd = fileno(fp);
    struct stat st;

    if (fstat(fd, &st))
    {
        perror("Couldn't retrieve the size of the file");
        return NULL;
    }

    *sz = (size_t)st.st_size;

    char *buffer = (char *)malloc(*sz);
    if (buffer == NULL)
    {
        handle_oom();
    }

    size_t bytes_read = fread(buffer, 1, (size_t)(*sz), fp);
    if (bytes_read != (size_t)(*sz) && ferror(fp))
    {
        free(buffer);
        return NULL;
    }

    return buffer;
}