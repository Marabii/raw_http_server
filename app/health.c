#include <string.h>

#include "server/http/http.h"
#include "app/health.h"

HttpResponse *health_check(ContextWrapper *context_wrapper, HttpRequest *request)
{
    (void)context_wrapper;
    (void)request;

    HttpResponse *response = create_HttpResponse(200);
    const char *response_str = "Server is OK!";
    set_response_body(response, response_str, strlen(response_str));
    return response;
}