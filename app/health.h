#pragma once

#include "server/http/http.h"

HttpResponse *health_check(ContextWrapper *context_wrapper, HttpRequest *request);