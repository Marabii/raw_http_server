#include <stdio.h>
#include <stdlib.h>
#include "string.h"

#include "utils/utils.h"
#include "env_parser/env.h"

// --- Globals ---
Env *env = NULL;

// --- Main logic ---
void load_env(void)
{
    size_t env_file_size = 0;
    char *env_file = read_file(".env", &env_file_size);

    if (env_file == NULL || env_file_size == 0)
    {
        fprintf(stderr, "Empty env file\n");
        exit(EXIT_FAILURE);
    }

    env = calloc(1, sizeof(Env));
    if (env == NULL)
    {
        handle_oom();
    }

    env->buff = env_file;

    size_t params_size = 0;
    size_t params_cap = 10;

    env->params = calloc(params_cap, sizeof(EnvParam));
    if (env->params == NULL)
    {
        handle_oom();
    }

    char *ptr = env_file;
    char *buf_end = env_file + env_file_size; // points at the '\0' terminator

    while (ptr < buf_end)
    {
        char *line = ptr;
        char *end_line = strchr(line, '\n');
        if (end_line != NULL)
        {
            *end_line = '\0';
            ptr = end_line + 1;
        }
        else
        {
            end_line = buf_end;
            ptr = buf_end;
        }

        // Strip trailing '\r' (CRLF files) and trailing spaces from the line.
        while (end_line > line && (end_line[-1] == '\r' || end_line[-1] == ' '))
        {
            end_line--;
        }
        *end_line = '\0';

        // Skip spaces, then blank lines and comments.
        while (*line == ' ')
        {
            line++;
        }
        if (*line == '\0' || *line == '#')
        {
            continue;
        }

        char *sep = strchr(line, '=');
        if (sep == NULL)
        {
            fprintf(stderr, "Malformed .env: Missing '=' in line \"%s\"\n", line);
            goto cleanup;
        }

        // Key: trim spaces before '='.
        char *key_end = sep;
        while (key_end > line && key_end[-1] == ' ')
        {
            key_end--;
        }
        *key_end = '\0';

        if (key_end == line)
        {
            fprintf(stderr, "Malformed .env: Empty key\n");
            goto cleanup;
        }
        char *key = line;

        // skip spaces after '='.
        char *val = sep + 1;
        while (*val == ' ')
        {
            val++;
        }

        if (*val == '\0')
        {
            fprintf(stderr, "Malformed .env: Empty val for key \"%s\"\n", key);
            goto cleanup;
        }

        env->params[params_size].key = key;
        env->params[params_size].val = val;

        params_size++;
        if (params_size == params_cap)
        {
            size_t new_cap = params_cap * 2;
            EnvParam *intermediate = realloc(env->params, new_cap * sizeof(EnvParam));
            if (intermediate == NULL)
            {
                handle_oom();
            }

            env->params = intermediate;
            params_cap = new_cap;
        }
    }

    env->params_count = params_size;
    return;

cleanup:
    free_Env();
    exit(EXIT_FAILURE);
}

void free_Env()
{
    if (env != NULL)
    {
        free(env->params);
        free(env->buff);
        free(env);
    }
}

char *get_env_val(char *key)
{
    if (env == NULL)
    {
        fprintf(stderr, "You must call load_env() first\n");
        return NULL;
    }

    for (size_t i = 0; i < env->params_count; i++)
    {
        if (strcmp(env->params[i].key, key) == 0)
        {
            return env->params[i].val;
        }
    }

    return NULL;
}