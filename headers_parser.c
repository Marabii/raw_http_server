#include "http_server.h"
#include "headers_parser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/*Converts string to lowercase in-place*/
void str_tolower(char *str);

Headers *parse_headers(const char *raw_headers, const size_t size)
{
    char *headers_copy = (char *)malloc(size);
    if (headers_copy == NULL)
    {
        return NULL;
    }

    char *headers_copy_start = headers_copy;

    memcpy(headers_copy, raw_headers, size);

    // Parse http verb:
    char *verb_end = strchr(headers_copy, ' ');
    if (verb_end == NULL)
    {
        goto cleanup;
    }

    *verb_end = '\0';
    char *verb_str = headers_copy;
    int verb_code = -1;
    for (size_t i = 0; http_verbs_map[i].verb_str != NULL; i++)
    {
        if (strcmp(http_verbs_map[i].verb_str, verb_str) == 0)
        {
            verb_code = http_verbs_map[i].verb_code;
            break;
        }
    }

    if (verb_code == -1)
    {
        goto cleanup;
    }

    HttpVerb verb = (HttpVerb)verb_code;
    headers_copy = verb_end + 1;

    if (*headers_copy == '\0')
    {
        goto cleanup;
    }

    // Parse the path
    char *path_end = strchr(headers_copy, ' ');
    if (path_end == NULL)
    {
        goto cleanup;
    }
    *path_end = '\0';

    if (path_end - headers_copy < 1)
    {
        goto cleanup;
    }

    Path path = (Path)headers_copy;
    headers_copy = path_end + 1;
    if (*headers_copy == '\0')
    {
        goto cleanup;
    }

    // Verify protocol is supported
    char *protocol_end = strstr(headers_copy, "\r\n");
    if (protocol_end == NULL)
    {
        goto cleanup;
    }
    *protocol_end = '\0';
    char *protocol = headers_copy;
    int is_protocol_supported = 0;
    for (size_t i = 0; SUPPORTED_PROTOCOLS[i] != NULL; i++)
    {
        if (strcmp(protocol, SUPPORTED_PROTOCOLS[i]) == 0)
        {
            is_protocol_supported = 1;
            break;
        }
    }

    if (is_protocol_supported == 0)
    {
        goto cleanup;
    }
    headers_copy = protocol_end + 2;
    if (*headers_copy == '\0')
    {
        goto cleanup;
    }

    // Parse headers:
    size_t header_entries_size = 0;
    size_t header_entries_capacity = 10;
    HeaderEntry *header_entries = (HeaderEntry *)malloc(sizeof(HeaderEntry) * header_entries_capacity);
    if (header_entries == NULL)
    {
        goto cleanup;
    }

    for (;;)
    {
        char *sep = strchr(headers_copy, ':');
        if (sep == NULL)
        {
            goto cleanup_free_entries;
        }

        *sep = '\0';
        char *header_key = headers_copy;

        if (sep - header_key < 1)
        {
            goto cleanup_free_entries;
        }

        str_tolower(header_key);

        headers_copy = sep + 1;
        headers_copy = headers_copy + strspn(headers_copy, " "); // skip whitespaces;

        if (*headers_copy == '\0')
        {
            goto cleanup_free_entries;
        }

        char *header_val_end = strstr(headers_copy, "\r\n");
        if (header_val_end == NULL)
        {
            goto cleanup_free_entries;
        }

        *header_val_end = '\0';
        char *header_val = headers_copy;

        headers_copy = header_val_end + 2;

        if (*headers_copy == '\0')
        {
            goto cleanup_free_entries;
        }

        HeaderEntry header_entry = {header_key, header_val};
        header_entries[header_entries_size] = header_entry;
        header_entries_size++;

        if (header_entries_size == header_entries_capacity)
        {
            size_t new_capacity = header_entries_capacity * 2;
            HeaderEntry *intermediate = (HeaderEntry *)realloc(header_entries, new_capacity * sizeof(HeaderEntry));

            if (intermediate == NULL)
            {
                goto cleanup_free_entries;
            }

            header_entries_capacity = new_capacity;
            header_entries = intermediate;
        }

        if (headers_copy[0] == '\r' && headers_copy[1] == '\n')
        {
            break;
        }
    }

    // reallocate to fit the size:
    header_entries = realloc(header_entries, sizeof(HeaderEntry) * (header_entries_size + 1));

    Headers *headers = (Headers *)malloc(sizeof(Headers));

    if (headers == NULL)
    {
    }
    headers->header_entries = header_entries;
    headers->header_entries_size = header_entries_size;
    headers->http_verb = verb;
    headers->path = path;

    return headers;

cleanup_free_entries:
    free(header_entries);
    header_entries = NULL;
    goto cleanup;

cleanup:
    free(headers_copy_start);
    headers_copy_start = NULL;
    fprintf(stderr, "Failed to parse headers\n");
    return NULL;
}

void str_tolower(char *str)
{
    if (!str)
        return;
    for (unsigned char *p = (unsigned char *)str; *p; ++p)
    {
        *p = (unsigned char)tolower(*p);
    }
}

const char *get_verb_name(HttpVerb verb)
{
    size_t map_size = sizeof(http_verbs_map) / sizeof(http_verbs_map[0]);

    for (size_t i = 0; i < map_size; i++)
    {
        if (http_verbs_map[i].verb_code == verb)
        {
            return http_verbs_map[i].verb_str;
        }
    }
    return "UNKNOWN";
}

void print_http_request(const Headers *h)
{
    if (h == NULL)
    {
        printf("Error: Null header pointer\n");
        return;
    }

    printf("HTTP REQUEST\n");

    printf("  METHOD: %-7s\n", get_verb_name(h->http_verb));
    printf("  PATH:   %s\n", (h->path ? h->path : "/"));

    printf("  HEADERS:\n");
    if (h->header_entries == NULL || h->header_entries_size == 0)
    {
        printf("    (none)\n");
    }
    else
    {
        for (size_t i = 0; i < h->header_entries_size; i++)
        {
            const char *key = h->header_entries[i].key ? h->header_entries[i].key : "NULL";
            const char *val = h->header_entries[i].val ? h->header_entries[i].val : "NULL";

            printf("    %-15s: %s\n", key, val);
        }
    }
}
