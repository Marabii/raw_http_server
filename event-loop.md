# Everything about the event-loop.

## What is a file descriptor (FD)?

In Linux, a file descriptor is an integer >= 0 that uniquely identifies an open file or an (I/O) resource within a running process. Each process has its own FD table. 
Linux follows the philosophy that everything is a file. This translates to file descriptors pointing not just to text files on the hard drive, but also to sockets, pipes, hardware devices, directories...

When a program wants to read or write data, it asks the kernel to open the resource, the kernel grants access and hands back an integer like 3 (The FD). 
Reads and writes become simple calls: `read(3, data, size)`, `write(3, data, size)`
The kernel knows that 3 points to the resource we opened earlier. 

By convention:
- FD 0 -> stdin
- FD 1 -> stdout
- FD 2 -> stderr

By default, operations like `read()` and `write()` are blocking, meaning:
- In the case of `read()`:
  - When the FD is for a regular hard disk file and it is empty (or we reached the end), the `read()` sys call simply returns 0 as it encountered EOF.
  - When the FD is for a stream like a socket connection or a pipe, and it hasn't received any data yet, the kernel pauses that thread until it receives data then resumes execution. (If `read()` returns 0 on a socket, it means the remote peer closed the connection).
- In the case of `write()`, things are a bit more complicated. For streams like sockets and pipes, the kernel reserves a dedicated write buffer for the FD (often around 64kb to 256kb, though sockets scale dynamically), whereas regular disk files write directly into the system-wide Page Cache in RAM. Calling the `write()` system call attempts to copy all the data into the available buffer space so that the kernel finishes the rest in the background (either committing the data to the hard disk or sending it over the network). For a socket or pipe, if the network/consumer is slow enough that the buffer fills up faster than it is freed, the kernel—just like with `read()`—pauses the thread until enough buffer space is freed to finish copying the data.

We can make their behavior non-blocking (Note: `O_NONBLOCK` works on streams like sockets and pipes, while Linux generally ignores it on regular disk files), in which case:
- `read()` calls fail immediately with an output of -1 and the error number set to `EAGAIN` (or `EWOULDBLOCK`) in case there is no data to read yet.
- `write()` calls fail with the same `-1` (`EAGAIN` / `EWOULDBLOCK`) error if the buffer is 100% full. If there is some available space, but not enough for the whole batch of data, the `write()` sys call copies whatever fits and immediately returns the number of bytes that were actually written (a partial write) so that the next `write()` sys call can advance its data pointer by that much.

A few caveats to using `write()` to send data over the network:
- Network speeds are much, MUCH slower than RAM speeds at which data is being copied to that socket's write buffer. This results in the buffer often getting full if the data we're trying to send is larger than the buffer size.
- Most day-to-day server requests are based on TCP. This means the kernel cannot free the memory in the buffer to start handling the next batch until it receives the `ACK` signal from the client. Here's how this works:
  - Imagine we have a TCP connection between a client and a server. The server is trying to send some data (larger than 256kb, the size of the write buffer) to the client.
  - Network packets are often 1.5kb. The server's kernel reads 1.5kb off of the buffer (This is packet 1) and transmits it over the network.
  - The client receives it, but doesn't send the `ACK` signal back immediately. It uses a "Delayed ACK" timer: it usually sends one every 2 in-order packets, or after ~40ms if a second packet never arrives.
  - The client's kernel can immediately make the first packet available to user space since everything up until now came in order.
  - The server keeps sending the other packets (2, 3, 4, 5) in order. Imagine if the second packet was lost in transit, but the client did receive packets 3, 4, and 5.
  - The client's kernel cannot transfer packets 3, 4, and 5 to user space via the `read()` sys call since TCP guarantees that incoming bytes must be delivered in order.
  - Instead, the client holds packets 3, 4, and 5 in kernel space. Because an out-of-order packet arrived (a gap was detected), the client cancels the 40ms timer and immediately fires off a Duplicate `ACK` for every out-of-order packet that lands.
  - Inside those Duplicate `ACK` packets, the client attaches `SACK` metadata (Stands for `Selective Acknowledgment`—it allows the server to know exactly which packets arrived past the gap so the server only resends what was actually lost. In a traditional `ACK` setup without `SACK`, the client could only acknowledge packet 1, which could cause the server to wastefully resend packets 3, 4, and 5 even though they arrived safely).
  - So as packets 3, 4, and 5 arrive one after another, the client immediately fires back:
    - Dup `ACK` 1 (after packet 3 arrives): *"I got up to packet 1, `SACK`: I also have 3"*
    - Dup `ACK` 2 (after packet 4 arrives): *"I got up to packet 1, `SACK`: I also have 3-4"*
    - Dup `ACK` 3 (after packet 5 arrives): *"I got up to packet 1, `SACK`: I also have 3-5"*
  - The server knows packet 1 arrived safely, so it frees packet 1 from the write buffer. However, the server doesn't retransmit packet 2 on the very first Dup `ACK` (in case packet 2 was just slightly delayed on the wire), nor does it assume packets 4 and 5 are lost when it gets the first `SACK` for packet 3 (it knows 4 and 5 are still in flight).
  - Instead, the server waits until it receives 3 Duplicate `ACK`s (Fast Retransmit). By the time that 3rd Dup `ACK` arrives, the server's kernel sees from the `SACK` metadata that packets 3 through 5 arrived safely, and only packet 2 was lost.
  - The server retransmits only packet 2. The client receives it, plugs the gap, and now finally has the full contiguous range from 2 to 5 (packet 1 having already been read by user space) ready for the next `read()` call.
  - The client sends a single cumulative `ACK` covering up to packet 5, and the server frees that entire range from the write buffer.
  - The server's kernel doesn't immediately notify the event loop that the FD is writable every single time a tiny 1.5kb packet is freed from a full write buffer. Doing so would consume a lot of CPU resources due to waking up the process for constant tiny `write()` sys calls. While POSIX defines a configurable `SO_SNDLOWAT` (Send Low-Water Mark, which defaults to 1 byte), Linux internally uses a write-wakeup threshold typically around 50% of the write buffer. Once a buffer fills up, the kernel only marks the FD as ready to accept new data again when roughly 50% of empty space has opened up.


## How the event-loop actually works:

When the server first starts, it listens to port 80 if it is a simple HTTP server or port 443 in case it is HTTPS (or an unprivileged port like 8080 during development).
A file descriptor is reserved for just this listening socket. To find it:
First run:
```bash
ls -l /proc/{Server PID}/fd

```

It is going to list out a number of file descriptors and their types associated with the server's PID.
We're looking for a socket FD, so remember those. They look like this: `'socket:[565039]'` — the number within is called the `inode`, which is the kernel's unique ID for that specific socket resource.

Then run:

```bash
cat /proc/net/tcp

```

This returns a table that can be used to map socket `inode`s to their local IP addresses and ports, which are represented in hexadecimal (with the IP in little-endian byte order).
If the server is running on `127.0.0.1:8080`, look for `'0100007F:1F90'` (`0100007F` = `127.0.0.1` backwards, and `1F90` = `8080` in hex). If it's listening on all interfaces (`0.0.0.0:8080`), look for `'00000000:1F90'`.

Now that we have a listening socket FD, the kernel handles the TCP 3-way handshake (`SYN` -> `SYN-ACK` -> `ACK`) in the background. Every time a client completes that 3-way handshake, the kernel places the connection in the listening socket's "Accept Queue" and marks the listening FD as readable. We then call `accept()` on the listening FD, which pops that connection off the queue and returns a brand-new FD dedicated to that specific client connection so we can read from it and write to it.

The event loop looks like this in pseudocode:

```python
while true:
    ready = wait_for_events(all_my_sockets)   # blocks until something happens
    for each event in ready:
        if event is "new connection on the listening socket":
            accept it, set to non-blocking, register the new socket for READ monitoring
        elif event is "socket has data to read":
            read it, parse it, maybe produce a response
        elif event is "socket can accept more output":
            write more of the pending response

```

In Linux, `wait_for_events` maps to `epoll`. What happens is that you first create an instance of `epoll` using the `epoll_create1` sys call—this returns another file descriptor for that `epoll` instance itself. We then use the `epoll_ctl` sys call to add a specific FD (the listening socket, or the new client connection we just accepted) to `epoll`'s interest list (the list of watched FDs). The `epoll_wait` sys call can then be used to get a list of file descriptors that are ready; it blocks (puts the calling thread to sleep) if there are no FDs currently ready.

Let's follow along a POST request from start to finish in an event-loop model:

* The client initiates a connection by sending a `SYN` to port 80/443. The kernel replies with `SYN-ACK`, and the client sends back `ACK`.
* Only once that 3-way handshake finishes does `epoll_wait` wake up! Our code calls `accept()` on the listening FD to get the new client socket FD, sets it to non-blocking (`O_NONBLOCK`), and adds it to `epoll`'s interest list via `epoll_ctl` watching for readable events (`EPOLLIN`).
* In one of the next loop iterations, the new client FD is marked as ready because the socket's receive buffer now has some data to read (HTTP headers, POST body...).
* We use the `read()` system call on the client FD in a loop to start draining data from the kernel buffer into user space. If the data being sent is large enough (or arrives across multiple packets), `read()` will eventually return `-1` with `EAGAIN` as we read faster than the client is sending data over the network. In this case, we save the state/data we have received so far for this FD and move on to handling other events in the loop.
* As the loop continues running and new packets arrive, `epoll_wait` wakes us up again for this FD, and we read and append more of what the client is sending until we successfully read all the incoming data. We know that we got there by doing the following:
  * We allocate a reasonable chunk of memory (typically a 4kb to 64kb buffer) and call the `read()` sys call into it.
  * If `read()` returns `0`, it means the client closed the connection early (`EOF`) before finishing the request, so we close the FD and discard the buffer.
  * Otherwise, `read()` returns a value larger than `0` (the number of bytes copied into our buffer).
  * In the accumulated data we received, we look for the special HTTP end-of-headers marker `\r\n\r\n`. Everything before it is headers, and everything immediately after it (even inside that very same 4kb `read()` chunk!) is the start of the actual body.
  * If we haven't found `\r\n\r\n` yet, either the headers are still arriving over the network or they are larger than 4kb. We wait for the next readable event and call `read()` again until we find `\r\n\r\n`, OR until our accumulated header buffer hits a hard ceiling (e.g., 8kb–16kb) where we immediately close the connection and throw a `431 Request Header Fields Too Large` error to protect against memory exhaustion (Slowloris/OOM) attacks.
  * Once we have the headers, we parse the `Content-Length` header (or check for `Transfer-Encoding: chunked`), which tells us the exact size in bytes of the request body.
  * We use `Content-Length` minus the number of body bytes we already captured after `\r\n\r\n` to figure out how much body data we're still missing.
  * Whenever `epoll` tells us the FD is readable again, we continue calling `read()` until we capture all the remaining bytes.
  * Once the missing byte count hits 0, we have the full request! We stop reading and start executing our business logic.

* Executing business logic:
  * most often than not, business logic involves making some sort of IO bound work like a db request. The question becomes, when the db finally responds, how do you know which coroutine/virtual thread to resume and which FD for which user to eventually send the data to ?
  * The answer comes from the fact epoll_ctl allows you to store any raw pointer to some data in user space you want.
    * Whether we use coroutines or virtual threads, we have a specific memory address where the coroutine struct/thread stack in heap data lives (say address 0x7f8a0040)
    * We add to that heap memory the user's FD so that we always know which FD to send the response to eventually.
    * We make the db call, a write to one of the free FDs of the connection pool of the database (or we put it in a queue until a db FD frees)
    * We then make a `read` sys call, if it returns -1 `EAGAIN`, we just wait to a next iteration of the event loop that wakes the db's FD.
    * At this stage, we made the db call, it is going to take a while, in order to keep the event loop busy and serve other requests, we must pause the coroutine/park the virtual thread here.
    * We attach the heap address of the data that would enable us to resume execution later (0x7f8a0040) and attach it to the chosen db FD 
    * In some future iteration of the event loop where the db finally replies, we can immediately dereference the resume point memory address which we can get by inspecting the db's epoll FD data.
    * We can read the response, run business logic and prepare a response to the user.

* After executing business logic, the server prepares the HTTP response bytes in user space.
* We now have to send the response data back to that same client FD using `write()`:
* Notice we did NOT register the FD for write-readiness (`EPOLLOUT`) at the start—because an empty write buffer is *always* writable, which would cause `epoll_wait` to spin in an infinite loop at 100% CPU!
* Instead, as soon as the response is ready, we immediately call `write(FD, data, size)` directly.
* If the response is small enough to fit into the socket's write buffer (e.g., 256kb) all at once, `write()` copies the whole thing and returns `size`. We are completely done!
* If the response is huge (or the buffer is already full), `write()` copies whatever fits and returns a partial byte count (or `-1` with `EAGAIN` if 0 bytes fit).
* Only when that happens do we store the remaining unwritten bytes in user space and call `epoll_ctl` to start watching that FD for write-readiness (`EPOLLOUT`).
* Everything we explained before about TCP data transfer, `ACK`s, `SACK`s, and the 50% write-wakeup threshold applies here: once enough `ACK`s arrive from the client to free ~50% of the kernel write buffer, `epoll_wait` wakes up with an `EPOLLOUT` event, we `write()` the next chunk, and once the final byte is written, we unregister `EPOLLOUT` (or close the FD if not using `Keep-Alive`).