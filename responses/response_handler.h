#pragma once

#include <stdio.h>

char *format_response(size_t *size);
char *read_html_file(FILE *fp, size_t *sz);