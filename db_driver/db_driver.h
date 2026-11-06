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

DBServerDetails *connect_to_db();
