#pragma once

typedef struct
{
    char *key;
    char *val;
} EnvParam;

typedef struct
{
    EnvParam *params;
    size_t params_count;
    char *buff; // The buffer that owns the key val pairs.
} Env;

extern Env *env;

void load_env(void);
char *get_env_val(char *key);
void free_Env();