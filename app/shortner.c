#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "server/http/http.h"
#include "server/core/bytebuf.h"
#include "server/pg/pg.h"
#include "utils/utils.h"
#include "app/shortner.h"

#define SHORT_URL_LEN 10

// URL shortner
// Run:
// CREATE TABLE urls (id SERIAL PRIMARY KEY, long_url TEXT NOT NULL, short VARCHAR(10) NOT NULL);
// CREATE INDEX short_url_index ON urls (short);

static void generate_short_url(char *out)
{
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    for (size_t i = 0; i < SHORT_URL_LEN; i++)
    {
        out[i] = charset[arc4random_uniform(sizeof(charset) - 1)];
    }
    out[SHORT_URL_LEN] = '\0';
}

HttpResponse *shorten_url(ContextWrapper *context_wrapper, HttpRequest *request)
{
    (void)context_wrapper;

    if (request->body_len == 0)
    {
        HttpResponse *response = create_HttpResponse(400);
        const char *response_str = "Request body is missing";
        set_response_body(response, response_str, strlen(response_str));
        return response;
    }

    char *long_url = dup_str((const char *)request->body, request->body_len);

    char *shortened = malloc(SHORT_URL_LEN + 1); // +1 for the '\0' terminator
    if (shortened == NULL)
    {
        handle_oom();
    }
    generate_short_url(shortened);

    // Query is: INSERT INTO urls (long_url, short) VALUES (long_url, shortened);
    ByteBuffer *query_bb = bb_create(50);
    bb_write_string(query_bb, "INSERT INTO urls (long_url, short) VALUES ('", 0, 0);
    bb_write_string(query_bb, long_url, 0, 0);
    bb_write_string(query_bb, "', '", 0, 0);
    bb_write_string(query_bb, shortened, 0, 0);
    bb_write_string(query_bb, "');", 0, 0);

    char *query = bb_detach_cstr(query_bb);

    SqlTable *result = run_query_async(query);

    free(query);

    if (result == NULL)
    {
        HttpResponse *response = create_HttpResponse(500);
        const char *response_str = "Failed to run query";
        set_response_body(response, response_str, strlen(response_str));
        free(shortened);
        free(long_url);
        return response;
    }

    ByteBuffer *response_msg_buff = bb_create(50);
    bb_write_string(response_msg_buff, "Success, shortened URL: ", 0, 0);
    bb_write_string(response_msg_buff, long_url, 0, 0);
    bb_write_string(response_msg_buff, " To: ", 0, 0);
    bb_write_string(response_msg_buff, shortened, 0, 0);

    char *response_msg = bb_detach_cstr(response_msg_buff);
    HttpResponse *response = create_HttpResponse(200);
    set_response_body(response, response_msg, strlen(response_msg));

    free(response_msg);
    free(shortened);
    free(long_url);
    free_SqlTable(result);

    return response;
}

HttpResponse *read_original_url(ContextWrapper *context_wrapper, HttpRequest *request)
{
    (void)context_wrapper;

    const char *short_id = get_path_var_val(request->path, "id");
    if (short_id == NULL || short_id[0] == '\0')
    {
        HttpResponse *response = create_HttpResponse(400);
        const char *response_str = "Missing required id path param";
        set_response_body(response, response_str, strlen(response_str));
        return response;
    }

    ByteBuffer *bb_query = bb_create(50);
    bb_write_string(bb_query, "SELECT long_url FROM urls WHERE short = '", 0, 0);
    bb_write_string(bb_query, short_id, 0, 0);
    bb_write_string(bb_query, "';", 0, 0);

    char *query = bb_detach_cstr(bb_query);

    SqlTable *result = run_query_async(query);

    free(query);

    if (result == NULL || result->row_count <= 0)
    {
        HttpResponse *response = create_HttpResponse(404);
        const char *response_str = "Provided short id not found";
        set_response_body(response, response_str, strlen(response_str));
        free_SqlTable(result);
        return response;
    }

    char *long_url = result->cells[0].value.as_text.data;

    ByteBuffer *response_msg_bb = bb_create(50);
    bb_write_string(response_msg_bb, "Success!, original URL is: ", 0, 0);
    bb_write_string(response_msg_bb, long_url, 0, 0);
    char *response_msg = bb_detach_cstr(response_msg_bb);

    HttpResponse *response = create_HttpResponse(200);
    set_response_body(response, response_msg, strlen(response_msg));

    free(response_msg);
    free_SqlTable(result);
    return response;
}