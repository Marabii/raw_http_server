#pragma once

#include "server/http/http.h"

HttpResponse *shorten_url(ContextWrapper *context_wrapper, HttpRequest *request);
HttpResponse *read_original_url(ContextWrapper *context_wrapper, HttpRequest *request);