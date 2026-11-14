#define _GNU_SOURCE // memmem, strcasecmp

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#include "utils/utils.h"
#include "server/http/http.h"
#include "server/core/bytebuf.h"

static const char *const SUPPORTED_PROTOCOLS[] = {"HTTP/1.1", NULL};

// --- Main logic ---
HeaderList *create_HeaderList(void)
{
    HeaderList *h = malloc(sizeof(HeaderList));
    if (h == NULL)
    {
        handle_oom();
    }

    h->len = 0;
    h->cap = 10; // allocate 10 headers in advance.

    h->entries = calloc(h->cap, sizeof(HeaderEntry));
    if (h->entries == NULL)
    {
        handle_oom();
    }

    return h;
}

void headers_add(HeaderList *h, const char *key, size_t key_len, const char *val, size_t val_len)
{
    if (key_len == 0)
    {
        key_len = strlen(key);
    }

    if (val_len == 0)
    {
        val_len = strlen(val);
    }

    if (h->len == h->cap)
    {
        size_t new_cap = h->cap * 2;
        HeaderEntry *intermediate = realloc(h->entries, new_cap * sizeof(HeaderEntry));
        if (intermediate == NULL)
        {
            handle_oom();
        }
        h->entries = intermediate;
        h->cap = new_cap;
    }

    h->entries[h->len].key = dup_str(key, key_len);
    h->entries[h->len].val = dup_str(val, val_len);
    h->len++;
}

// The key/val pair is duplicated, you can/MUST free key, val later.
void headers_set(HeaderList *h, const char *key, const char *val)
{
    for (size_t i = 0; i < h->len; i++)
    {
        if (strcasecmp(h->entries[i].key, key) == 0)
        {
            char *new_val = dup_str(val, strlen(val));
            free(h->entries[i].val);
            h->entries[i].val = new_val;
            return;
        }
    }

    headers_add(h, key, 0, val, 0);
}

const char *headers_get(const HeaderList *h, const char *key)
{
    if (h == NULL)
    {
        return NULL;
    }

    for (size_t i = 0; i < h->len; i++)
    {
        if (strcasecmp(h->entries[i].key, key) == 0)
        {
            return h->entries[i].val;
        }
    }

    return NULL;
}

void free_HeaderList(HeaderList *h)
{
    if (h == NULL)
    {
        return;
    }

    for (size_t i = 0; i < h->len; i++)
    {
        free(h->entries[i].key);
        free(h->entries[i].val);
    }

    free(h->entries);
    free(h);
}

// TODO: error out if there is a space right before the colon: RFC 7230/9112
int parse_request_header(const char *raw, size_t size, HttpVerb *verb, Path *path, HeaderList **headers)
{
    const char *end = raw + size;

    // We use memmem because the raw headers string isn't guarenteed to be null terminated.
    const char *line_end = memmem(raw, size, "\r\n", 2);
    if (line_end == NULL)
    {
        return -1;
    }

    const char *verb_end = memchr(raw, ' ', (size_t)(line_end - raw));
    if (verb_end == NULL)
    {
        return -1;
    }

    const char *path_start = verb_end + 1;
    const char *path_end = memchr(path_start, ' ', (size_t)(line_end - path_start));
    if (path_end == NULL || path_end == path_start)
    {
        return -1;
    }

    const char *protocol = path_end + 1;
    size_t protocol_len = (size_t)(line_end - protocol);

    int is_protocol_supported = 0;
    for (size_t i = 0; SUPPORTED_PROTOCOLS[i] != NULL; i++)
    {
        if (strlen(SUPPORTED_PROTOCOLS[i]) == protocol_len && memcmp(protocol, SUPPORTED_PROTOCOLS[i], protocol_len) == 0)
        {
            is_protocol_supported = 1;
            break;
        }
    }

    if (!is_protocol_supported)
    {
        return -1;
    }

    size_t verb_len = (size_t)(verb_end - raw);
    int verb_code = -1;
    for (size_t i = 0; http_verbs_map[i].verb_str != NULL; i++)
    {
        if (strlen(http_verbs_map[i].verb_str) == verb_len && memcmp(http_verbs_map[i].verb_str, raw, verb_len) == 0)
        {
            verb_code = (int)http_verbs_map[i].verb_code;
            break;
        }
    }

    if (verb_code == -1)
    {
        return -1;
    }

    // Header lines, ended by an empty line.
    HeaderList *parsed = create_HeaderList();

    const char *p = line_end + 2;
    for (;;)
    {
        if (end - p < 2)
        {
            goto fail; // Ran out of data before the blank line.
        }

        if (p[0] == '\r' && p[1] == '\n')
        {
            break;
        }

        const char *eol = memmem(p, (size_t)(end - p), "\r\n", 2);
        if (eol == NULL)
        {
            goto fail;
        }

        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (colon == NULL || colon == p)
        {
            goto fail;
        }

        const char *val = colon + 1;
        while (val < eol && *val == ' ')
        {
            val++; // skip whitespaces
        }

        if (val == eol)
        {
            goto fail; // empty value
        }

        headers_add(parsed, p, (size_t)(colon - p), val, (size_t)(eol - val));
        p = eol + 2;
    }

    path->raw_path = dup_str(path_start, (size_t)(path_end - path_start));

    *verb = (HttpVerb)verb_code;
    *headers = parsed;
    return 0;

fail:
    free_HeaderList(parsed);
    fprintf(stderr, "Failed to parse headers\n");
    return -1;
}