#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server/http/http.h"
#include "utils/utils.h"

static Route *routes = NULL;
static size_t routes_count = 0;
static size_t routes_cap = 0;

// --- Helpers forward declaration ---
static int path_equals(const char *route_path, const char *req_path);

// --- Main Logic ---

// Returns -1 if there was an issue or 0 if successful
int add_route(HttpVerb verb, const char *path, RouteHandler handler)
{
    if (path == NULL || handler == NULL)
    {
        fprintf(stderr, "add_route: path and handler must not be NULL\n");
        return -1;
    }

    // Replace the handler if this (verb, path) is already registered.
    for (size_t i = 0; i < routes_count; i++)
    {
        if (routes[i].verb == verb && strcmp(routes[i].path, path) == 0)
        {
            routes[i].handler = handler;
            return 0;
        }
    }

    if (routes_count == routes_cap)
    {
        size_t new_cap = (routes_cap == 0) ? 10 : routes_cap * 2;
        Route *new_routes = realloc(routes, new_cap * sizeof(Route));
        if (new_routes == NULL)
        {
            handle_oom();
        }
        routes = new_routes;
        routes_cap = new_cap;
    }

    routes[routes_count++] = (Route){.path = path, .verb = verb, .handler = handler};

    return 0;
}

RouteMatch find_route(HttpVerb verb, const char *path, Route *out)
{
    int path_exists = 0;

    for (size_t i = 0; i < routes_count; i++)
    {
        if (path_equals(routes[i].path, path) == -1)
        {
            continue;
        }

        if (routes[i].verb == verb)
        {
            if (out != NULL)
            {
                *out = routes[i];
            }
            return ROUTE_FOUND;
        }
        path_exists = 1;
    }

    return path_exists ? ROUTE_METHOD_NOT_ALLOWED : ROUTE_NOT_FOUND;
}

// --- Helpers ---

// returns 0 if matched, otherwise -1.
static int path_equals(const char *route_path, const char *req_path)
{
    if (!route_path || !req_path)
    {
        return -1;
    }

    const char *route_ptr = route_path;
    const char *req_ptr = req_path;

    while (*route_ptr != '\0' && *req_ptr != '\0' && *req_ptr != '?')
    {
        if (*route_ptr == ':')
        {
            // Advance route_ptr past the parameter name (until '/' or end of string)
            while (*route_ptr != '\0' && *route_ptr != '/')
            {
                route_ptr++;
            }

            // Ensure the path variable matches at least one non-delimiter character
            if (*req_ptr == '/' || *req_ptr == '?' || *req_ptr == '\0')
            {
                return -1;
            }

            // Advance req_ptr past the matched value segment
            while (*req_ptr != '\0' && *req_ptr != '/' && *req_ptr != '?')
            {
                req_ptr++;
            }
        }
        else
        {
            if (*route_ptr != *req_ptr)
            {
                return -1;
            }
            route_ptr++;
            req_ptr++;
        }
    }

    if (*route_ptr == '\0' && (*req_ptr == '\0' || *req_ptr == '?'))
    {
        return 0;
    }

    return -1;
}