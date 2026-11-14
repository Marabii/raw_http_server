#define _GNU_SOURCE // memmem, strcasecmp

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>

#include "server/http/http.h"
#include "server/core/config.h"
#include "server/runtime/scheduler.h"
#include "server/core/bytebuf.h"
#include "utils/utils.h"

// --- Helper forward declaration ---
static int parse_content_length(const char *str, size_t *out);
static void free_HttpRequest(HttpRequest *r);
static HttpRequest *read_http_request(ContextWrapper *context_wrapper);
static int parse_path_vars(const char *route_path, Path *path);

// --- Main Logic ---
void handle_http_client(ContextWrapper *context_wrapper)
{
    HttpRequest *http_request = read_http_request(context_wrapper);
    if (http_request == NULL)
    {
        // Closed connection, malformed or oversized request.
        free_ContextWrapper(context_wrapper);
        return;
    }

    Route route = {0};

    RouteMatch route_matched = find_route(http_request->verb, http_request->path->raw_path, &route);

    if (route_matched == ROUTE_NOT_FOUND)
    {
        send_failure_response(context_wrapper, 404);
    }
    else if (route_matched == ROUTE_METHOD_NOT_ALLOWED)
    {
        send_failure_response(context_wrapper, 405);
    }
    else if (route.handler == NULL)
    {
        send_failure_response(context_wrapper, 500);
    }
    else
    {
        if (parse_path_vars(route.path, http_request->path) < 0)
        {
            fprintf(stderr, "Failed to parse path params");
            send_failure_response(context_wrapper, 404);
        }
        else
        {
            RouteHandler handler = route.handler;
            HttpResponse *http_response = handler(context_wrapper, http_request);
            if (http_response != NULL)
            {
                size_t response_size = 0;
                char *response = format_response(http_response, &response_size);
                async_write(context_wrapper, (uint8_t *)response, response_size);
                free(response);
            }

            free_HttpResponse(http_response);
        }
    }
    free_ContextWrapper(context_wrapper);
    free_HttpRequest(http_request);
}

char *get_path_var_val(Path *path, char *key)
{
    for (size_t i = 0; i < path->path_vars_count; i++)
    {
        if (strcmp(path->path_vars[i].key, key) == 0)
        {
            return path->path_vars[i].val;
        }
    }

    return NULL;
}

void free_Path(Path *path)
{
    if (path != NULL)
    {
        free(path->raw_path);

        for (size_t i = 0; i < path->path_vars_count; i++)
        {
            free(path->path_vars[i].key);
            free(path->path_vars[i].val);
        }
        free(path->path_vars);
        free(path->query_params);
        free(path);
    }
}

// --- Helpers ---

// Reads one full HTTP request (headers + body) from client_fd.
// Returns NULL on a closed connection, malformed/unsupported request, or an oversized request.
static HttpRequest *read_http_request(ContextWrapper *context_wrapper)
{
    size_t rb_size = 0;
    size_t rb_capacity = READ_BUFFER;
    size_t header_len = 0; // length of the header section including the final \r\n\r\n
    size_t body_len = 0;
    size_t total_len = 0; // header_len + body_len, only valid once headers are parsed
    int headers_parsed = 0;

    HttpRequest *http_request = calloc(1, sizeof(HttpRequest));
    if (http_request == NULL)
    {
        handle_oom();
    }

    Path *path = calloc(1, sizeof(Path));
    if (path == NULL)
    {
        handle_oom();
    }

    http_request->path = path;

    uint8_t *request_buff = malloc(rb_capacity);
    if (request_buff == NULL)
    {
        handle_oom();
    }

    for (;;)
    {
        size_t want;

        if (!headers_parsed)
        {
            // Still looking for the end of the headers.
            if (rb_size == rb_capacity)
            {
                if (rb_capacity >= MAXIMUM_ALLOWED_PAYLOAD)
                {
                    goto cleanup; // Headers alone exceed the maximum allowed size.
                }

                size_t new_capacity = rb_capacity * 2;
                if (new_capacity > MAXIMUM_ALLOWED_PAYLOAD)
                {
                    new_capacity = MAXIMUM_ALLOWED_PAYLOAD;
                }

                uint8_t *intermediate = realloc(request_buff, new_capacity);
                if (intermediate == NULL)
                {
                    handle_oom();
                }
                request_buff = intermediate;
                rb_capacity = new_capacity;
            }
            want = rb_capacity - rb_size;
        }
        else
        {
            if (rb_size >= total_len)
            {
                break; // Full request received.
            }
            want = total_len - rb_size;
        }

        int rs = async_read(context_wrapper, request_buff + rb_size, want);
        if (rs <= 0)
        {
            goto cleanup;
        }
        rb_size += (size_t)rs;

        if (headers_parsed)
        {
            continue;
        }

        // The buffer is not null terminated, so search only the bytes received so far.
        uint8_t *marker = memmem(request_buff, rb_size, "\r\n\r\n", 4);
        if (marker == NULL)
        {
            continue;
        }

        header_len = (size_t)(marker - request_buff) + 4;
        if (parse_request_header((const char *)request_buff, header_len, &http_request->verb, path, &http_request->headers) != 0)
        {
            goto cleanup;
        }
        headers_parsed = 1;

        // Chunked bodies are not supported, refuse request.
        if (headers_get(http_request->headers, "Transfer-Encoding") != NULL)
        {
            goto cleanup;
        }

        const char *content_len_str = headers_get(http_request->headers, "Content-Length");
        if (content_len_str != NULL && parse_content_length(content_len_str, &body_len) != 0)
        {
            goto cleanup;
        }

        if (body_len > MAXIMUM_ALLOWED_PAYLOAD - header_len)
        {
            goto cleanup;
        }

        total_len = header_len + body_len;
        if (total_len > rb_capacity)
        {
            uint8_t *intermediate = realloc(request_buff, total_len);
            if (intermediate == NULL)
            {
                handle_oom();
            }
            request_buff = intermediate;
            rb_capacity = total_len;
        }
    }

    if (body_len > 0)
    {
        http_request->body = malloc(body_len);
        if (http_request->body == NULL)
        {
            handle_oom();
        }
        memcpy(http_request->body, request_buff + header_len, body_len);
    }

    http_request->body_len = body_len;

    free(request_buff);
    return http_request;

cleanup:
    free_HttpRequest(http_request);
    free(request_buff);
    return NULL;
}

// Returns 0 on success, -1 if invalid or larger than MAXIMUM_ALLOWED_PAYLOAD.
static int parse_content_length(const char *str, size_t *out)
{
    if (str == NULL || !isdigit((unsigned char)*str))
    {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(str, &end, 10);
    if (errno != 0 || val > MAXIMUM_ALLOWED_PAYLOAD)
    {
        return -1;
    }

    if (*end != '\0')
    {
        return -1;
    }

    *out = (size_t)val;
    return 0;
}

static void free_HttpRequest(HttpRequest *r)
{
    if (r != NULL)
    {
        free_HeaderList(r->headers);
        free(r->body);
        free_Path(r->path);
        free(r);
    }
}

static int parse_path_vars(const char *route_path, Path *path)
{
    size_t path_vars_count = 0;
    size_t path_vars_cap = 10;
    PathParam *path_vars = calloc(path_vars_cap, sizeof(PathParam));

    if (path_vars == NULL)
    {
        handle_oom();
    }

    const char *route_path_ptr = route_path;
    char *req_path_ptr = path->raw_path;

    for (;;)
    {
        char *path_param_key_start = strchr(route_path_ptr, ':');
        if (path_param_key_start == NULL)
        {
            break;
        }

        if (path_param_key_start[1] == '/' || path_param_key_start[1] == '\0')
        {
            goto cleanup;
        }

        char *path_param_val_start = req_path_ptr + (size_t)(path_param_key_start - route_path_ptr);

        const char *path_param_key_end = strchr(path_param_key_start, '/');
        if (path_param_key_end == NULL)
        {
            path_param_key_end = route_path + strlen(route_path);
        }

        char *key = dup_str(path_param_key_start + 1, path_param_key_end - path_param_key_start - 1); // 1 is for skipping ':'

        char *path_param_val_end = strchr(path_param_val_start, '/');
        if (path_param_val_end == NULL)
        {
            path_param_val_end = path->raw_path + strlen(path->raw_path);
        }

        if (path_param_val_end - path_param_val_start == 0)
        {
            free(key);
            goto cleanup;
        }

        char *val = dup_str(path_param_val_start, path_param_val_end - path_param_val_start);

        path_vars[path_vars_count].key = key;
        path_vars[path_vars_count].val = val;

        path_vars_count++;
        if (path_vars_count == path_vars_cap)
        {
            size_t new_cap = path_vars_cap * 2;
            PathParam *intermediate = realloc(path_vars, new_cap * sizeof(PathParam));
            if (intermediate == NULL)
            {
                handle_oom();
            }

            path_vars = intermediate;
            path_vars_cap = new_cap;
        }

        if (path_param_key_end[0] == '\0')
        {
            if (path_param_val_end[0] != '\0')
            {
                goto cleanup;
            }

            break;
        }

        req_path_ptr = path_param_val_end + 1;
        route_path_ptr = path_param_key_end + 1;
    }

    path->path_vars = path_vars;
    path->path_vars_count = path_vars_count;

    return 0;

cleanup:
    for (size_t i = 0; i < path_vars_count; i++)
    {
        free(path_vars[i].key);
        free(path_vars[i].val);
    }
    free(path_vars);
    return -1;
}