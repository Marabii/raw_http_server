#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    DB_I = 0, // Idle, not in a transaction.
    DB_T = 1, // In transaction.
    DB_E = 2  // In failed transaction. An error occurred inside a BEGIN block, queries will be rejected until ROLLBACK
} TransactionStatus;

typedef struct DBServerParam
{
    char *name;
    size_t name_len;
    char *val;
    size_t val_len;
    struct DBServerParam *next; // next node. (This would make it easy to add new params...)
} DBServerParam;

typedef struct
{
    DBServerParam *params; // Head of the list.
    uint32_t db_pid;
    uint32_t cancellation_key;
    TransactionStatus transaction_status;
    int db_fd;
} DBServerDetails;

#define DB_POOL_SIZE 5

typedef struct
{
    DBServerDetails *idle[DB_POOL_SIZE];
    size_t idle_count;
} DBPool;

// Opens and authenticates DB_POOL_SIZE connections. Returns NULL if any of them fails.
DBPool *db_pool_create(void);

// Takes an idle connection out of the pool. Returns NULL if every connection is in use.
DBServerDetails *db_pool_acquire(DBPool *pool);

// Gives a connection obtained from db_pool_acquire back to the pool.
void db_pool_release(DBPool *pool, DBServerDetails *conn);

// Closes every connection and frees the pool. All connections should have been released first.
void db_pool_destroy(DBPool *pool);

// Frees the details only, does not close db_fd.
void free_DBServerDetails(DBServerDetails *server_details);

// Frees connection params
void free_DBServerParam(DBServerParam *server_params);
