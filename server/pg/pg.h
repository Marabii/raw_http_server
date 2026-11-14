#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "server/runtime/scheduler.h"

#define DB_POOL_SIZE 40

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
    ContextWrapper *context_wrapper;
    int db_fd;
} DBServerDetails;

typedef struct
{
    DBServerDetails *idle[DB_POOL_SIZE];
    size_t idle_count;
    // When trying to acquire a db connection fails, the task is appended to this queue
    // until a connection is released back to the pool.
    Task *conn_head;
    Task *conn_tail;
} DBPool;

extern DBPool *db_pool;

// Supported SQL value types
typedef enum
{
    SQL_TYPE_NULL = 0,
    SQL_TYPE_INT64,
    SQL_TYPE_DOUBLE,
    SQL_TYPE_TEXT,
    SQL_TYPE_BOOLEAN
} SqlType;

typedef struct
{
    SqlType type;
    union
    {
        int64_t as_int64;
        double as_double;
        bool as_bool;
        struct
        {
            char *data;
            size_t length;
        } as_text;
        struct
        {
            uint8_t *data;
            size_t size;
        } as_blob;
    } value;
} SqlCell;

typedef struct
{
    char name[64];
    SqlType type;
} SqlColumnMeta;

typedef struct
{
    size_t col_count;
    size_t row_count;
    SqlColumnMeta *columns; // Array of length col_count
    SqlCell *cells;         // Flattened 2D array: row * col_count + col
} SqlTable;

// Opens and authenticates DB_POOL_SIZE connections. Returns NULL if any of them fails.
DBPool *db_pool_create(void);

// Takes an idle connection out of the pool. Returns NULL if every connection is in use.
DBServerDetails *db_pool_acquire(DBPool *pool);

// Gives a connection obtained from db_pool_acquire back to the pool.
void db_pool_release(DBPool *pool, DBServerDetails *conn);

// Closes every connection and frees the pool. All connections should have been released first.
void free_DBPool(DBPool *pool);

// Frees the details only, does not close db_fd.
void free_DBServerDetails(DBServerDetails *server_details);

// Runs a simple query and returns the result. Free it with free_SqlTable.
SqlTable *run_query_async(char *sql_query);

// Frees the table, its columns, and the data owned by its cells.
void free_SqlTable(SqlTable *table);
