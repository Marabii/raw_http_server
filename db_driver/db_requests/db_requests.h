#pragma once

#include <stdbool.h>

#include "db_driver/db_driver.h"

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

// Runs a simple query and returns the result. Free it with free_SqlTable.
SqlTable *run_query(DBServerDetails *conn, char *sql_query);

// Frees the table, its columns, and the data owned by its cells.
void free_SqlTable(SqlTable *table);

// Sends Terminate ('X'), closes the socket and frees the connection.
void close_connection(DBServerDetails *conn);
