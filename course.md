# Socket programming in C/Linux:

## What is it ?

It's a method of enabling communication between 2 nodes over the network.

## TCP socket lifecycle:

### Server side of things:

#### 1- Calling the `socket()` sys call:

```c
#include <sys/socket.h>

int server_fd = socket(int domain, int type, int protocol);
```

- The domain argument specifies the communication domain being used, could be one of:
  * `AF_UNIX` (or `AF_LOCAL`, same thing) for local communication.
  * `AF_INET` for IPv4
  * `AF_INET6` for IPv6
  * ...

- The type argument specifies the communication semantics: UDP, TCP, ...
  * `SOCK_STREAM` for TCP
  * `SOCK_DGRAM` for UDP

- The protocol argument usually set to 0.

#### 2- Configuring the socket defaults with `setsocketopt()` sys call:

Used to modify the default parameters set when creating a socket.

```c
#include <sys/socket.h>

int setsockopt(int socket, int level, int option_name, const void *option_value, socklen_t option_len);
```

- The socket argument is just the file descriptor of the socket you want to modify.

- The level argument is usually set to `SOL_SOCKET`.

- The option_name argument specifies which setting we are trying to change.

- The option_value argument is a void pointer. It sets the value to the option we want to set. The reason why it is a void pointer is that this sys call has to work on a wide
  variety of options.

- Because option_value is a void pointer, we must tell the kernel how many bytes to read to access that pointer.

A default behavior that is almost always overridden with this sys call is `SO_REUSEADDR`. When the server binds to a specific port, stops, then starts again and tries to bind to the same port. It might get an error 'Address is already in use'. The reason is that when the server dies, the kernel doesn't let that port be used again for around 60 to 120 seconds. It is a protective measure against slow packets meant to arrive at the old server but end up arriving at the new one. `SO_REUSEADDR` tells the kernel to allow the new server to immediately bind to the same port.

```c
int option = 1;
setsockopt(3, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
```

#### 3- Calling the `bind()` sys call:

Even though at this point we created a socket, it is still not mapped to a particular port yet. That is the job of the `bind()` sys call:

```c
#include <sys/socket.h>

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```
One of the most confusing parts about this sys call is the `sockaddr` struct argument. We never fill out `sockaddr` itself, instead, we create another struct like `sockaddr_in` for ipv4 and
`sockaddr_in6` for ipv6, then we cast it into `sockaddr`. The reason is mostly historical as old C didn't support void pointers. The designers created carrier struct which gets immediately overridden by the actual struct needed.

Here's how to do call it:

PS: network addresses and ports must be laid out in big endian order.

```c
#include <sys/socket.h>

struct sockaddr_in address;
struct in_addr ipv4_addr;

int res = inet_pton(AF_INET, "127.0.0.1", &ipv4_addr); // Converts the address to big endian.
if (res == 1)
{
    printf("IPv4 converted successfully (binary: 0x%08x)\n", ipv4_addr.s_addr);
}
else if (res == 0)
{
    fprintf(stderr, "Invalid IPv4 address format\n");
}
else
{
    perror("inet_pton");
}

memset(&address, 0, sizeof(address)); // Creates an empty struct with null bytes, some get filled, the rest serves as padding.
address.sin_family = AF_INET; // Specifies the domain: ipv4, ipv6,...
address.sin_addr = ipv4_addr;
address.sin_port = htons(8080); // Converts port to big endian.
```

#### 4- Calling the `accept()` sys call:

When a client finishes the 3-way handshake with the server, the connection sits in a pending queue waiting to be accepted.

```c
#include <sys/socket.h>

int accept(int sockfd, struct sockaddr *_Nullable restrict addr,
          socklen_t *_Nullable restrict addrlen);
```

- `sockfd`: The file descriptor of the listening socket.
- `sockaddr`: Same struct we passed to the `bind()` sys call.
- `addrlen`: `sizeof(address)`

It returns the file descriptor of the new connection.