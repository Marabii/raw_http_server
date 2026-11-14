#include <stdlib.h>
#include <time.h>
#include <locale.h>
#include <string.h>
#include <stdio.h>

#include "server/runtime/scheduler.h"
#include "server/http/http.h"
#include "server/core/config.h"
#include "server/core/bytebuf.h"
#include "utils/utils.h"

// --- Helpers forward declaration ---
static const char *http_status_to_string(int status_code);
static int is_reserved_header(const char *key);

// --- Main logic ---
HttpResponse *create_HttpResponse(int status_code)
{
    HttpResponse *r = calloc(1, sizeof(HttpResponse));
    if (r == NULL)
    {
        handle_oom();
    }

    r->status_code = status_code;
    r->headers = create_HeaderList();
    return r;
}

void set_response_body(HttpResponse *r, const void *data, size_t len)
{
    if (len == 0)
    {
        return;
    }

    r->body = malloc(len);
    if (r->body == NULL)
    {
        handle_oom();
    }
    memcpy(r->body, data, len);
    r->body_len = len;
}

void free_HttpResponse(HttpResponse *r)
{
    if (r != NULL)
    {
        free_HeaderList(r->headers);
        free(r->body);
        free(r);
    }
}

char *format_response(const HttpResponse *response, size_t *formatted_response_size)
{
    if (response == NULL)
    {
        return NULL;
    }

    ByteBuffer *bb_response = bb_create(WRITE_BUFFER);

    bb_write_string(bb_response, "HTTP/1.1 ", 0, 0);
    bb_write_string(bb_response, http_status_to_string(response->status_code), 0, 0);
    bb_write_string(bb_response, "\r\n", 0, 0);

    // Writing curr date:
    time_t now = time(NULL);
    struct tm gmt_time;
    char date_buff[38]; // Date header is always 37 bytes including the ending \r\n. (+1 for the null byte which gets overriten)
    gmtime_r(&now, &gmt_time);
    locale_t c_locale = newlocale(LC_TIME_MASK, "C", (locale_t)0);
    strftime_l(date_buff, sizeof(date_buff), "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &gmt_time, c_locale);
    freelocale(c_locale);
    bb_write_string(bb_response, date_buff, 0, 0);

    // Writing the response length:
    char content_length[24]; // 24 bytes is large enough to hold any 64 bit number.
    snprintf(content_length, sizeof(content_length), "%zu", response->body_len);

    bb_write_string(bb_response, "Content-Length: ", 0, 0);
    bb_write_string(bb_response, content_length, 0, 0);
    bb_write_string(bb_response, "\r\n", 0, 0);

    // Add the handler's headers:
    for (size_t i = 0; i < response->headers->len; i++)
    {
        const HeaderEntry *entry = &response->headers->entries[i];
        if (is_reserved_header(entry->key))
        {
            continue;
        }

        bb_write_string(bb_response, entry->key, 0, 0);
        bb_write_string(bb_response, ": ", 0, 0);
        bb_write_string(bb_response, entry->val, 0, 0);
        bb_write_string(bb_response, "\r\n", 0, 0);
    }

    // Specify connection type:
    bb_write_string(bb_response, "Connection: close\r\n\r\n", 0, 0);

    // Add response body:
    if (response->body_len != 0 && response->body != NULL)
    {
        bb_write_string(bb_response, (const char *)response->body, response->body_len, 0);
    }

    *formatted_response_size = bb_response->cursor;
    return bb_detach_cstr(bb_response);
}

void send_failure_response(ContextWrapper *context_wrapper, int status_code)
{
    HttpResponse *response = create_HttpResponse(status_code);
    size_t formatted_response_size = 0;
    char *response_formatted = format_response(response, &formatted_response_size);
    async_write(context_wrapper, (const uint8_t *)response_formatted, formatted_response_size);
    free(response_formatted);
    free_HttpResponse(response);
}

// --- Helpers ---
static const char *http_status_to_string(int status_code)
{
    switch (status_code)
    {
    /* 2xx Success */
    case 200:
        return "200 OK";
    case 201:
        return "201 Created";
    case 204:
        return "204 No Content";

    /* 3xx Redirection */
    case 301:
        return "301 Moved Permanently";
    case 302:
        return "302 Found";
    case 304:
        return "304 Not Modified";

    /* 4xx Client Errors */
    case 400:
        return "400 Bad Request";
    case 401:
        return "401 Unauthorized";
    case 403:
        return "403 Forbidden";
    case 404:
        return "404 Not Found";
    case 405:
        return "405 Method Not Allowed";
    case 408:
        return "408 Request Timeout";
    case 429:
        return "429 Too Many Requests";

    /* 5xx Server Errors */
    case 500:
        return "500 Internal Server Error";
    case 502:
        return "502 Bad Gateway";
    case 503:
        return "503 Service Unavailable";
    case 504:
        return "504 Gateway Timeout";

    default:
        return "500 Internal Server Error";
    }
}

// Headers the server always generates itself, so handlers can't produce a conflicting or wrong value.
static int is_reserved_header(const char *key)
{
    return strcasecmp(key, "Content-Length") == 0 ||
           strcasecmp(key, "Date") == 0 ||
           strcasecmp(key, "Connection") == 0;
}