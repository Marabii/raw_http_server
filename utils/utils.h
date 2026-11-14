#pragma once

#include <stddef.h>
#include <stdint.h>

// Used to handle out of memory errors.
void handle_oom();

char *read_file(char *file_path, size_t *sz);