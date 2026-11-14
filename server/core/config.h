#pragma once

#define MAXIMUM_ALLOWED_PAYLOAD 256000 // 256 kb

#define MAX_EVENTS 10 // Maximum number of file descriptors epoll_wait can report at once.

// Read and write buffer sizes:
#define READ_BUFFER 4096
#define WRITE_BUFFER 4096