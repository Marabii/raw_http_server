#pragma once
#include <stddef.h>

#define PORT 8080
#define MAX_EVENTS 10 // Maximum number of file descriptors epoll_wait can report at once.

// Read and write buffer sizes:
#define READ_BUFFER 4096
#define WRITE_BUFFER 4096

// Header size limits.
#define PATH_MAX_LENGTH 256
#define HEADER_KEY_MAX_LENGTH 256
#define HEADER_VAL_MAX_LENGTH 256

static const char *const SUPPORTED_PROTOCOLS[] = {"HTTP/1.1", NULL};

typedef enum
{
    GET = 0,
    POST = 1,
    PUT = 2,
    PATCH = 3,
    DELETE = 4,
    QUERY = 5
} HttpVerb;

typedef struct
{
    const char *verb_str;
    HttpVerb verb_code;
} HttpVerbsMap;

static const HttpVerbsMap http_verbs_map[] = {
    {"GET", GET},
    {"POST", POST},
    {"PUT", PUT},
    {"PATCH", PATCH},
    {"DELETE", DELETE},
    {"QUERY", QUERY},
    {NULL, 0}};

typedef struct
{
    char *key;
    char *val;
} HeaderEntry;

typedef char *Path;

typedef struct
{
    HttpVerb http_verb;
    Path path;
    HeaderEntry *header_entries;
    size_t header_entries_size;
} Headers;
