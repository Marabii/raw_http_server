#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "db_driver.h"
#include "conn/conn.h"
#include "auth/auth.h"
#include "utils/utils.h"
#include "db_driver/helpers/helpers.h"
#include "db_driver/db_requests/db_requests.h"
#include "db_driver/post_auth_init/read_server_params.h"

static DBServerDetails *connect_to_db(void);

DBPool *db_pool_create(void)
{
    DBPool *pool = malloc(sizeof(DBPool));
    if (pool == NULL)
    {
        handle_oom();
    }

    pool->idle_count = 0;

    for (size_t i = 0; i < DB_POOL_SIZE; i++)
    {
        DBServerDetails *conn = connect_to_db();
        if (conn == NULL)
        {
            fprintf(stderr, "Failed to open DB connection %zu of %d\n", i + 1, DB_POOL_SIZE);
            db_pool_destroy(pool);
            return NULL;
        }

        pool->idle[pool->idle_count++] = conn;
    }

    return pool;
}

DBServerDetails *db_pool_acquire(DBPool *pool)
{
    if (pool->idle_count == 0)
    {
        return NULL;
    }

    return pool->idle[--pool->idle_count];
}

void db_pool_release(DBPool *pool, DBServerDetails *conn)
{
    if (conn == NULL)
    {
        return;
    }

    if (pool->idle_count == DB_POOL_SIZE)
    {
        fprintf(stderr, "Released a connection that does not belong to the pool\n");
        return;
    }

    pool->idle[pool->idle_count++] = conn;
}

void db_pool_destroy(DBPool *pool)
{
    if (pool == NULL)
    {
        return;
    }

    for (size_t i = 0; i < pool->idle_count; i++)
    {
        close_connection(pool->idle[i]);
    }

    free(pool);
}

// --- Helpers ---
static DBServerDetails *connect_to_db(void)
{
    const char *host = "127.0.0.1";
    uint16_t port = 5432;

    int db_fd = connect_to_tcp(host, port);
    if (db_fd < 0)
    {
        fprintf(stderr, "Failed to create TCP connection with the DB\n");
        return NULL;
    }

    int auth_result = authenticate(db_fd);
    if (auth_result == -1)
    {
        fprintf(stderr, "Failed to authenticate with the database\n");
        goto cleanup;
    }

    DBServerDetails *server_details = read_server_details(db_fd);
    if (server_details == NULL)
    {
        fprintf(stderr, "Failed to read server details\n");
        goto cleanup;
    }

    return server_details;

cleanup:
    close(db_fd);
    return NULL;
}

void free_DBServerDetails(DBServerDetails *server_details)
{
    if (server_details != NULL)
    {
        DBServerParam *param = server_details->params;
        while (param != NULL)
        {
            DBServerParam *next = param->next;
            free_DBServerParam(param);
            param = next;
        }
        free(server_details);
    }
}

void free_DBServerParam(DBServerParam *server_params)
{
    if (server_params != NULL)
    {
        free(server_params->name);
        free(server_params->val);
        free(server_params);
    }
}
