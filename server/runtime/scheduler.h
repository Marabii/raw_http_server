#pragma once

#include <sys/types.h>
#include <stdint.h>
#include <ucontext.h>

typedef struct Task Task; // Forward declaration

typedef enum
{
    TASK_CREATED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_WAITING_IO,
    TASK_WAITING_POOL,
    TASK_DEAD
} TaskState;

// This is what event.data.ptr must point to.
typedef struct
{
    int fd;
    Task *waiting_task; // The task currently blocked on this fd.
} ContextWrapper;

typedef struct Task
{
    ucontext_t ctx;
    void *stack; // 64kb, there's a risk for stack overflow...
    TaskState state;
    ContextWrapper *context_wrapper;
    struct Task *next;
} Task;

typedef struct
{
    int epoll_fd;
    ucontext_t loop_ctx; // The loop context to yield back to when encountering EAGAIN.
    Task *current_task;
    // This is a ready queue for the different tasks.
    Task *ready_head;
    Task *ready_tail;
} Scheduler;

extern Scheduler scheduler;

void schedule_task(Task *task);
Task *pop_ready_task();
Task *create_http_client_task(ContextWrapper *context_wrapper);
ssize_t async_read(ContextWrapper *context_wrapper, uint8_t *buf, size_t len);
int async_write(ContextWrapper *context_wrapper, const uint8_t *buf, size_t len);
void free_Task(Task *task);
void free_ContextWrapper(ContextWrapper *context_wrapper);