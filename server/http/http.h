#pragma once

#include <stddef.h>
#include <stdint.h>

#include "server/runtime/scheduler.h"

// Header size limits.
#define PATH_MAX_LENGTH 256
#define HEADER_KEY_MAX_LENGTH 256
#define HEADER_VAL_MAX_LENGTH 256

typedef enum
{
    HTTP_GET = 0,
    HTTP_POST = 1,
    HTTP_PUT = 2,
    HTTP_PATCH = 3,
    HTTP_DELETE = 4,
    HTTP_QUERY = 5
} HttpVerb;

typedef struct
{
    const char *verb_str;
    HttpVerb verb_code;
} HttpVerbsMap;

static const HttpVerbsMap http_verbs_map[] = {
    {"GET", HTTP_GET},
    {"POST", HTTP_POST},
    {"PUT", HTTP_PUT},
    {"PATCH", HTTP_PATCH},
    {"DELETE", HTTP_DELETE},
    {"QUERY", HTTP_QUERY},
    {NULL, 0}};

// Both strings are heap allocated and owned by the entry.
typedef struct
{
    char *key;
    char *val;
} HeaderEntry;

// Growable list of headers.
typedef struct
{
    HeaderEntry *entries;
    size_t len;
    size_t cap;
} HeaderList;

typedef struct
{
    char *key;
    char *val;
} PathParam;

typedef struct
{
    char *raw_path;

    // Must be defined like this: /users/:id
    // You can't use the same path param name twice otherwise the right most
    // param will override the existing one.
    PathParam *path_vars;
    size_t path_vars_count;

    PathParam *query_params;
    size_t query_params_count;
    size_t query_params_cap;
} Path;

typedef struct
{
    HttpVerb verb;
    Path *path;
    HeaderList *headers;
    uint8_t *body;
    size_t body_len;
} HttpRequest;

typedef struct
{
    int status_code;
    HeaderList *headers;
    uint8_t *body;
    size_t body_len;
} HttpResponse;

typedef HttpResponse *(*RouteHandler)(ContextWrapper *context_wrapper, HttpRequest *req);

typedef struct
{
    const char *path;
    HttpVerb verb;
    RouteHandler handler;
} Route;

typedef enum
{
    ROUTE_FOUND,
    ROUTE_NOT_FOUND,
    ROUTE_METHOD_NOT_ALLOWED
} RouteMatch;

// Headers logic
HeaderList *create_HeaderList(void);
void headers_add(HeaderList *h, const char *key, size_t key_len, const char *val, size_t val_len);
void headers_set(HeaderList *h, const char *key, const char *val);
const char *headers_get(const HeaderList *h, const char *key);
void free_HeaderList(HeaderList *h);
int parse_request_header(const char *raw, size_t size, HttpVerb *verb, Path *path, HeaderList **headers);

// Requests handler
void handle_http_client(ContextWrapper *context_wrapper);
void free_Path(Path* path);

// Get path variables:
char *get_path_var_val(Path *path, char *key);

// Responses handler
HttpResponse *create_HttpResponse(int status_code);
void set_response_body(HttpResponse *r, const void *data, size_t len);
char *format_response(const HttpResponse *response, size_t *formatted_response_size);
void send_failure_response(ContextWrapper *context_wrapper, int status_code);
void free_HttpResponse(HttpResponse *r);

// Router logic:

// Registers a handler for (verb, path). Registering the same (verb, path) again replaces the handler.
// Returns -1 if there was an issue or 0 if successful
int add_route(HttpVerb verb, const char *path, RouteHandler handler);

// Looks up the handler for (verb, path). Path queries and path params are not supported yet.
// ROUTE_FOUND: *out is set.
// ROUTE_METHOD_NOT_ALLOWED: the path exists but not for this verb.
RouteMatch find_route(HttpVerb verb, const char *path, Route *out);

#define add_GET(path, h) add_route(HTTP_GET, path, h)
#define add_POST(path, h) add_route(HTTP_POST, path, h)
