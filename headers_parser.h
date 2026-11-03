#pragma once
#include "http_server.h"

Headers *parse_headers(const char *raw_headers, const size_t size);
void print_http_request(const Headers *h);