#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "db_driver/post_auth_init/read_server_params.h"
#include "utils/utils.h"
#include "db_driver/helpers/helpers.h"

// --- Helpers ---
static void free_DBServerDetails(DBServerDetails *server_details);
static void free_DBServerParam(DBServerParam *server_params);
static int add_server_param(DBServerDetails *server_details, const uint8_t *data, size_t data_len);
static int add_key_data(DBServerDetails *server_details, const uint8_t *data, size_t data_len);

// --- Main logic ---
DBServerDetails *read_server_details(int db_fd)
{
    DBResponse *res = NULL;
    DBServerDetails *server_details = malloc(sizeof(DBServerDetails));
    if (server_details == NULL)
    {
        handle_oom();
    }

    server_details->db_fd = db_fd;

    // Read all server params.
    do
    {
        res = read_db_response(db_fd);
        if (res->type != (uint8_t)'S')
        {
            break;
        }

        if (add_server_param(server_details, res->data, res->len) < 0)
        {
            goto cleanup;
        }

        free_DBResponse(res);
        res = NULL;
    } while (1);

    // Must have received message of type 'K' (BackendKeyData) in the previous message.
    if (res->type != (uint8_t)'K')
    {
        goto cleanup;
    }

    if (add_key_data(server_details, res->data, res->len) < 0)
    {
        goto cleanup;
    }

    free_DBResponse(res);
    res = NULL;

    res = read_db_response(db_fd);
    if (res->type != (uint8_t)'Z' || res->len != 1)
    {
        goto cleanup;
    }

    int transaction_status = -1;

    if (res->data[0] == (uint8_t)'I')
    {
        transaction_status = DB_I;
    }
    else if (res->data[0] == (uint8_t)'T')
    {
        transaction_status = DB_T;
    }
    else if (res->data[0] == (uint8_t)'E')
    {
        transaction_status = DB_E;
    }

    if (transaction_status == -1)
    {
        goto cleanup;
    }
    server_details->transaction_status = transaction_status;

    free_DBResponse(res);
    res = NULL;

    return server_details;

cleanup:
    free_DBResponse(res);
    free_DBServerDetails(server_details);
    return NULL;
}

// --- Helpers impl ---
static void free_DBServerParam(DBServerParam *server_params)
{
    if (server_params != NULL)
    {
        free(server_params->name);
        free(server_params->val);
        free(server_params);
    }
}

static void free_DBServerDetails(DBServerDetails *server_details)
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

static int add_key_data(DBServerDetails *server_details, const uint8_t *data, size_t data_len)
{
    if (data_len != 8)
    {
        return -1;
    }

    uint32_t db_pid = (uint32_t)parse_be32_to_sizet(data);
    uint32_t cancellation_key = (uint32_t)parse_be32_to_sizet(data + 4);

    server_details->db_pid = db_pid;
    server_details->cancellation_key = cancellation_key;

    return 0;
}

// Parses a ParameterStatus body: name\0value\0 and prepends it to the list.
static int add_server_param(DBServerDetails *server_details, const uint8_t *data, size_t data_len)
{
    const uint8_t *name_end = memchr(data, '\0', data_len);
    if (name_end == NULL)
    {
        return -1;
    }

    size_t name_len = (size_t)(name_end - data);
    const uint8_t *val = name_end + 1;
    size_t remaining = data_len - name_len - 1;

    const uint8_t *val_end = memchr(val, '\0', remaining);
    if (val_end == NULL || (size_t)(val_end - data) + 1 != data_len)
    {
        return -1;
    }

    DBServerParam *param = malloc(sizeof(DBServerParam));
    if (param == NULL)
    {
        handle_oom();
    }

    param->name_len = name_len;
    param->name = dup_str((const char *)data, name_len);
    param->val_len = remaining - 1;
    param->val = dup_str((const char *)val, param->val_len);

    param->next = server_details->params;
    server_details->params = param;

    return 0;
}
