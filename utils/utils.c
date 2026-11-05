#include <stdio.h>
#include <stdlib.h>

// Handle out of memory error:
void handle_oom()
{
    fprintf(stderr, "Out of memory!");
    exit(-1);
}