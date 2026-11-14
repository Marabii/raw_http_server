#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <ucontext.h>
#include <errno.h>
#include <sys/epoll.h>

#include "utils/utils.h"
#include "server/runtime/scheduler.h"
#include "server/runtime/event_loop.h"
#include "server/http/http.h"

#define STACK_SIZE (64 * 1024) // How much stack each task is given.

// Global scheduler:
Scheduler scheduler = {0};

// --- Helpers forward declaration ---
static void task_trampoline_http_client(Task *task);

// --- Main logic ---
void schedule_task(Task *task)
{
    if (task->state == TASK_READY)
    {
        return; // The task is already enqueued.
    }

    task->state = TASK_READY;
    task->next = NULL;

    if (scheduler.ready_tail != NULL)
    {
        scheduler.ready_tail->next = task;
    }
    else
    {
        scheduler.ready_head = task;
    }

    scheduler.ready_tail = task;
}

Task *pop_ready_task()
{
    Task *task = scheduler.ready_head;

    if (task == NULL)
    {
        return NULL;
    }

    scheduler.ready_head = task->next;

    if (scheduler.ready_head == NULL)
    {
        scheduler.ready_tail = NULL;
    }

    task->next = NULL;
    return task;
}

Task *create_http_client_task(ContextWrapper *context_wrapper)
{
    Task *task = calloc(1, sizeof(Task));
    void *stack = malloc(STACK_SIZE);
    if (stack == NULL || task == NULL)
    {
        handle_oom();
    }
    task->stack = stack;

    if (getcontext(&task->ctx) == -1)
    {
        perror("Failed to get context");
        goto cleanup;
    }

    task->ctx.uc_stack.ss_sp = task->stack;
    task->ctx.uc_stack.ss_size = STACK_SIZE;
    task->ctx.uc_link = &scheduler.loop_ctx;
    task->context_wrapper = context_wrapper;

    makecontext(&task->ctx, (void (*)(void))task_trampoline_http_client, 1, task);
    schedule_task(task);
    return task;

cleanup:
    free_Task(task);
    return NULL;
}

// We call this only once in the entire program for every request
// at the end of the scheduler loop (before the event loop)
// Calling this from anywhere else will cause the scheduler to
// attempt running a freed task, reading freed memory and corrupting everything.
void free_Task(Task *task)
{
    if (task != NULL)
    {
        free(task->stack);
        free(task);
    }
}

// We call this when we no longer expect to send or receive anything from
// cw -> fd
void free_ContextWrapper(ContextWrapper *cw)
{
    if (cw != NULL)
    {
        if (cw->fd > 2)
        {
            close(cw->fd); // Automatically closes the file descriptor too.
        }

        // the task can only be freed once it finishes.
        // that is handled in the 'Run all tasks within ready queue' block.
        free(cw);
    }
}

// returns 0 if the client dropped the connection.
// returns > 0 the number of read bytes.
// returns -1 if there was an error.
// EAGAIN automatically yields back to the event loop.
ssize_t async_read(ContextWrapper *context_wrapper, uint8_t *buf, size_t len)
{
    for (;;)
    {
        ssize_t read_res = read(context_wrapper->fd, buf, len);
        if (read_res >= 0)
        {
            return read_res;
        }

        // read_res == -1, check if it is EAGAIN:
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Yield back to the event loop:
            context_wrapper->waiting_task = scheduler.current_task;
            context_wrapper->waiting_task->state = TASK_WAITING_IO;
            swapcontext(&context_wrapper->waiting_task->ctx, &scheduler.loop_ctx);

            context_wrapper->waiting_task = NULL;

            continue; // try reading again.
        }
        else
        {
            perror("Failed to read data");
            return read_res;
        }
    }
}

// Sends len bytes from buf.
// Returns:
// 1 means everything went well.
// 0 means the client dropped the connection.
// -1 means something went wrong.
int async_write(ContextWrapper *context_wrapper, const uint8_t *buf, size_t len)
{
    size_t bytes_written = 0;
    int was_epollout_set = 0;

    while (bytes_written < len)
    {
        ssize_t write_res = write(context_wrapper->fd, buf + bytes_written, len - bytes_written);
        if (write_res > 0)
        {
            bytes_written += write_res;
            if (bytes_written == len)
            {
                // Remove EPOLLOUT if it was set:
                if (was_epollout_set)
                {
                    union epoll_data ed;
                    ed.ptr = context_wrapper;

                    struct epoll_event ev;
                    ev.events = EPOLLIN;
                    ev.data = ed;

                    if (epoll_ctl(get_server_instance().epoll_fd, EPOLL_CTL_MOD, context_wrapper->fd, &ev) == -1)
                    {
                        perror("Failed to remove EPOLLOUT from fd");
                        // Still return normally..
                    }

                    was_epollout_set = 0;
                }
                return 1;
            }
        }

        else if (write_res == 0)
        {
            return 0;
        }

        else
        {
            // write_res == -1, check if it is EAGAIN:
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // Mark as EPOLLOUT:
                union epoll_data ed;
                ed.ptr = context_wrapper;

                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLOUT;
                ev.data = ed;

                was_epollout_set = 1;

                if (epoll_ctl(get_server_instance().epoll_fd, EPOLL_CTL_MOD, context_wrapper->fd, &ev) == -1)
                {
                    perror("Failed to mark file descriptor as EPOLLOUT");
                    return -1;
                }

                // Yield back to the event loop:
                context_wrapper->waiting_task = scheduler.current_task;
                context_wrapper->waiting_task->state = TASK_WAITING_IO;
                swapcontext(&context_wrapper->waiting_task->ctx, &scheduler.loop_ctx);

                context_wrapper->waiting_task = NULL;

                continue; // try writing again
            }
            else
            {
                perror("Failed to write data");
                return -1;
            }
        }
    }

    return 1; // Should be impossible to reach... It is here just to make the compiler happy.
}

// --- Helpers ---
static void task_trampoline_http_client(Task *task)
{
    handle_http_client(task->context_wrapper);
    task->state = TASK_DEAD;
    // Function ends -> uc_link automatically swaps back to scheduler.loop_ctx
}