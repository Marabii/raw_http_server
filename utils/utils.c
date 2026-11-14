#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "utils/utils.h"

// Handle out of memory error:
void handle_oom()
{
    fprintf(stderr, "Out of memory!");
    exit(-1);
}

// Returns null terminated char*
char *read_file(char *file_path, size_t *sz)
{
    FILE *fp = fopen(file_path, "r");

    if (fp == NULL)
    {
        perror("Error reading file");
        return NULL;
    }

    if (!fp)
    {
        return NULL;
    }

    int fd = fileno(fp);
    struct stat st;

    if (fstat(fd, &st))
    {
        perror("Couldn't retrieve the size of the file");
        return NULL;
    }

    *sz = (size_t)st.st_size;

    char *buffer = malloc(*sz + 1);
    if (buffer == NULL)
    {
        handle_oom();
    }

    size_t bytes_read = fread(buffer, 1, (size_t)(*sz), fp);
    fclose(fp);
    if (bytes_read != (size_t)(*sz))
    {
        free(buffer);
        return NULL;
    }

    buffer[*sz] = '\0';

    return buffer;
}