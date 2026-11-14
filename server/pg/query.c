#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "utils/utils.h"
#include "server/pg/pg.h"
#include "server/pg/wire.h"

// Each column description is a null terminated name followed by this many fixed bytes:
// table_oid (4) + column_index (2) + data_type_oid (4) + data_type_size (2) + type_modifier (4) + format_code (2).
#define COLUMN_DESCRIPTION_FIXED_SIZE 18

// --- Helpers forward declaration ---
static int parse_column_descriptions(SqlTable *table, const uint8_t *data, size_t data_size, size_t column_count);
static SqlType sql_type_from_oid(int32_t data_type_oid);
static int parse_data_row(SqlTable *table, const uint8_t *data, size_t data_size, size_t *cells_capacity);
static int parse_cell(SqlCell *cell, SqlType type, const uint8_t *bytes, size_t len);
void free_SqlTable(SqlTable *table);

// --- Main logic ---
SqlTable *run_query_async(char *sql_query)
{
    DBResponse *res = NULL;
    ByteBuffer *query_buffer = NULL;
    SqlTable *sql_response = calloc(1, sizeof(SqlTable));
    if (sql_response == NULL)
    {
        handle_oom();
    }

    DBServerDetails *server_details = db_pool_acquire(db_pool);
    if (server_details == NULL)
    {
        // TODO, handle pool exhaustion correctly.
        fprintf(stderr, "No db connection is available\n");
        goto cleanup;
    }

    // Prepare the request:
    size_t sql_query_len = strlen(sql_query);
    query_buffer = msg_begin('Q', sql_query_len + 5);
    bb_write_string(query_buffer, sql_query, sql_query_len, 1);
    msg_end(query_buffer);

    if (send_db_request_async(server_details->context_wrapper, query_buffer) < 0)
    {
        fprintf(stderr, "Failed to send db query\n");
        goto cleanup;
    }

    res = read_db_response_async(server_details->context_wrapper);
    if (res == NULL)
    {
        goto cleanup;
    }

    if (res->type == (uint8_t)'T')
    {

        if (res->len < 2)
        {
            goto cleanup;
        }

        int16_t column_count = parse_be16_to_int16_t(res->data);
        if (column_count <= 0)
        {
            goto cleanup;
        }

        // The column descriptions start after the int16 column count.
        // Names are copied into the table, so res isn't needed once this returns.
        if (parse_column_descriptions(sql_response, res->data + 2, res->len - 2, (size_t)column_count) < 0)
        {
            goto cleanup;
        }

        free_DBResponse(res);
        res = NULL;

        // Parse and add data rows as they arrive.
        size_t cells_capacity = 16;
        SqlCell *cells = calloc(cells_capacity, sizeof(SqlCell));
        if (cells == NULL)
        {
            handle_oom();
        }

        sql_response->cells = cells;

        for(;;)
        {
            res = read_db_response_async(server_details->context_wrapper);
            if (res == NULL)
            {
                goto cleanup;
            }

            if (res->type != (uint8_t)'D')
            {
                break;
            }

            if (parse_data_row(sql_response, res->data, res->len, &cells_capacity) < 0)
            {
                goto cleanup;
            }

            free_DBResponse(res);
            res = NULL;
        }
    }

    if (res->type != (uint8_t)'C' && res->type != (uint8_t)'I')
    {
        goto cleanup;
    }

    // The rows are followed by 'C' (CommandComplete) and then 'Z' (ReadyForQuery).
    // Keep reading until 'Z' so the connection is in a clean state for the next query.
    // An 'E' (ErrorResponse) can show up instead of the rows if the query fails midway.
    int query_failed = 0;
    while (res->type != (uint8_t)'Z')
    {
        if (res->type == (uint8_t)'E')
        {
            query_failed = 1;
        }

        free_DBResponse(res);
        res = read_db_response_async(server_details->context_wrapper);
        if (res == NULL)
        {
            goto cleanup;
        }
    }

    free_DBResponse(res);
    res = NULL;

    if (query_failed)
    {
        goto cleanup;
    }

    db_pool_release(db_pool, server_details);
    free_ByteBuffer(query_buffer);
    return sql_response;

// TODO
// This is actually not ideal.
// if we received a bad response, we must either keep on reading until we hit a 'Z' (ready for query) message or simply close the connection.
// Not doing so would leave the connection in an intermediate state that would corrupt the next request that uses the same connection.
// Will handle this properly on a next update.
cleanup:
    if (res != NULL)
    {
        free_DBResponse(res);
    }

    db_pool_release(db_pool, server_details);
    free_SqlTable(sql_response);
    free_ByteBuffer(query_buffer);
    return NULL;
}

// --- Helpers ---

// Maps a Postgres type OID to the closest SqlType.
// Everything else is just plain text, after  all, postgres returns data in text format, even integers are represented as ascii characters.
static SqlType sql_type_from_oid(int32_t data_type_oid)
{
    switch (data_type_oid)
    {
    case 16: // bool
        return SQL_TYPE_BOOLEAN;
    case 20: // int8
    case 21: // int2
    case 23: // int4
        return SQL_TYPE_INT64;
    case 700: // float4
    case 701: // float8
        return SQL_TYPE_DOUBLE;
    default:
        return SQL_TYPE_TEXT;
    }
}

// Converts the text representation of a value (not null terminated) into a cell of the given type.
// Returns 0 on success, -1 if the value can't be converted. On failure nothing is allocated.
static int parse_cell(SqlCell *cell, SqlType type, const uint8_t *bytes, size_t len)
{
    if (type == SQL_TYPE_BOOLEAN)
    {
        cell->type = SQL_TYPE_BOOLEAN;
        cell->value.as_bool = (len == 1 && bytes[0] == 't'); // Postgres sends 't' or 'f'.
        return 0;
    }

    char *text = dup_str((const char *)bytes, len);

    if (type == SQL_TYPE_INT64)
    {
        char *end = NULL;
        long long value = strtoll(text, &end, 10);
        int ok = end != text && *end == '\0';
        free(text);
        if (!ok)
        {
            return -1;
        }

        cell->type = SQL_TYPE_INT64;
        cell->value.as_int64 = (int64_t)value;
        return 0;
    }

    if (type == SQL_TYPE_DOUBLE)
    {
        char *end = NULL;
        double value = strtod(text, &end); // Also understands the "NaN" and "Infinity" Postgres sends.
        int ok = end != text && *end == '\0';
        free(text);
        if (!ok)
        {
            return -1;
        }

        cell->type = SQL_TYPE_DOUBLE;
        cell->value.as_double = value;
        return 0;
    }

    cell->type = SQL_TYPE_TEXT;
    cell->value.as_text.data = text;
    cell->value.as_text.length = len;
    return 0;
}

// Parses a DataRow ('D') message and appends its cells to table->cells, growing it when needed.
// Returns 0 on success, -1 if the message is malformed (the table is left as it was).
static int parse_data_row(SqlTable *table, const uint8_t *data, size_t data_size, size_t *cells_capacity)
{
    if (data_size < 2 || (size_t)parse_be16_to_int16_t(data) != table->col_count)
    {
        fprintf(stderr, "Malformed DataRow message\n");
        return -1;
    }

    size_t cells_needed = (table->row_count + 1) * table->col_count;
    if (cells_needed > *cells_capacity)
    {
        size_t new_capacity = *cells_capacity * 2;
        SqlCell *grown = realloc(table->cells, new_capacity * sizeof(SqlCell));
        if (grown == NULL)
        {
            handle_oom();
        }

        table->cells = grown;
        *cells_capacity = new_capacity;
    }

    SqlCell *row = &table->cells[table->row_count * table->col_count];
    const uint8_t *ptr = data + 2;
    const uint8_t *end = data + data_size;
    size_t i = 0;

    for (; i < table->col_count; i++)
    {
        if (end - ptr < 4)
        {
            goto malformed;
        }

        int32_t value_len = parse_be32_to_int32_t(ptr);
        ptr += 4;

        if (value_len == -1) // NULL
        {
            row[i].type = SQL_TYPE_NULL;
            row[i].value.as_blob.data = NULL;
            row[i].value.as_blob.size = 0;
            continue;
        }

        if (value_len < 0 || (size_t)(end - ptr) < (size_t)value_len)
        {
            goto malformed;
        }

        if (parse_cell(&row[i], table->columns[i].type, ptr, (size_t)value_len) < 0)
        {
            goto malformed;
        }

        ptr += value_len;
    }

    table->row_count++;
    return 0;

malformed:
    fprintf(stderr, "Malformed DataRow message\n");
    // row_count wasn't incremented, so free_SqlTable won't see the cells of this row. Free the ones already parsed.
    for (size_t j = 0; j < i; j++)
    {
        if (row[j].type == SQL_TYPE_TEXT)
        {
            free(row[j].value.as_text.data);
        }
    }
    return -1;
}

// data_size is the number of bytes left after reading the column count int16 integer.
// returns 0 in success, -1 if errors occured.
static int parse_column_descriptions(SqlTable *table, const uint8_t *data, size_t data_size, size_t column_count)
{
    SqlColumnMeta *columns = malloc(column_count * sizeof(SqlColumnMeta));
    if (columns == NULL)
    {
        handle_oom();
    }

    const uint8_t *ptr = data;
    const uint8_t *end = data + data_size;

    for (size_t i = 0; i < column_count; i++)
    {
        // The name is null terminated, make sure that terminator exists inside the message.
        const uint8_t *name_end = memchr(ptr, '\0', (size_t)(end - ptr));
        if (name_end == NULL)
        {
            goto malformed;
        }

        size_t name_len = (size_t)(name_end - ptr);
        if (name_len >= sizeof(columns[i].name)) // Postgres limits identifiers to 63 bytes, so this should never happen.
        {
            goto malformed;
        }

        if ((size_t)(end - (name_end + 1)) < COLUMN_DESCRIPTION_FIXED_SIZE)
        {
            goto malformed;
        }

        memcpy(columns[i].name, ptr, name_len + 1); // +1 for \0
        ptr = name_end + 1;

        // int32 table_oid and int16 column_index are not needed.
        ptr += 4 + 2;

        int32_t data_type_oid = parse_be32_to_int32_t(ptr);
        columns[i].type = sql_type_from_oid(data_type_oid);
        ptr += 4;

        // int16 data_type_size, int32 type_modifier (VARCHAR(50) -> 50) and int16 format_code are not needed for now.
        ptr += 2 + 4 + 2;
    }

    table->columns = columns;
    table->col_count = column_count;
    return 0;

malformed:
    fprintf(stderr, "Malformed RowDescription message\n");
    free(columns);
    return -1;
}

// Frees the table, its columns, and the data owned by its cells.
void free_SqlTable(SqlTable *table)
{
    if (table == NULL)
    {
        return;
    }

    if (table->cells != NULL)
    {
        for (size_t i = 0; i < table->row_count * table->col_count; i++)
        {
            // as_text.data and as_blob.data share the same storage, so this covers both.
            if (table->cells[i].type == SQL_TYPE_TEXT)
            {
                free(table->cells[i].value.as_text.data);
            }
        }
    }

    free(table->cells);
    free(table->columns);
    free(table);
}