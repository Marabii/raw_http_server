#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../utils/utils.h"

char *read_env_file(FILE *fp, size_t *sz)
{
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

    char *buffer = (char *)malloc(*sz);
    if (buffer == NULL)
    {
        handle_oom();
    }

    size_t bytes_read = fread(buffer, 1, (size_t)(*sz), fp);
    if (bytes_read != (size_t)(*sz) && ferror(fp))
    {
        free(buffer);
        return NULL;
    }

    return buffer;
}