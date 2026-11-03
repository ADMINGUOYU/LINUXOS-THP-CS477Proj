// file: thp_handler.c
# define _GNU_SOURCE
# include "thp_handler.h"

// Linux UAPI and libraries (C99)
# include <errno.h>
# include <linux/userfaultfd.h>
# include <poll.h>
# include <stdlib.h>
# include <sys/eventfd.h>
# include <sys/ioctl.h>
# include <sys/mman.h>
# include <time.h>
# include <unistd.h>

// Synchronous collapse was introduced in Linux 6.1; older kernels return an error.
# ifndef MADV_COLLAPSE
# define MADV_COLLAPSE 25
# endif

// [INTERNAL STATE] ======================
// The container mutex protects the queue, counters and all region state changes.
// Only the fault thread uses uffd after startup. The worker never resolves faults.
struct thp_handler
{
    struct thp_container * container;
    size_t * queue; // one entry per region; a region is queued at most once
    size_t queue_head;
    size_t queue_tail;
    pthread_cond_t work_ready;
    pthread_t fault_thread;
    pthread_t promotion_thread;
    int stop_fd;
    int fault_started;
    int promotion_started;
    int worker_stop;
    int stopped;
    struct thp_handler_stats stats;
};

// [INTERNAL API] (forward declarations) ==
static uint64_t _elapsed_ns(const struct timespec * start, const struct timespec * end);
static int _handle_fault(struct thp_handler * handler, const struct uffd_msg * message);
static void * _fault_main(void * argument);
static void * _promotion_main(void * argument);

// CLOCK_MONOTONIC timestamps avoid wall-clock corrections during measurements.
static uint64_t _elapsed_ns(const struct timespec * start, const struct timespec * end)
{
    return (uint64_t)(end->tv_sec - start->tv_sec) * UINT64_C(1000000000) +
           (uint64_t)end->tv_nsec - (uint64_t)start->tv_nsec;
}

// Resolve one event, update its bitmap, and unregister before publishing promotion work.
static int _handle_fault(struct thp_handler * handler, const struct uffd_msg * message)
{
    struct thp_container * container = handler->container;
    struct thp_region * region;
    struct uffdio_zeropage zero =
    {
        0
    };
    struct uffdio_range range;
    uintptr_t address;
    size_t offset;
    size_t region_index;
    size_t page_index;
    int failure = 0;
    int result;

    // Unexpected events cannot safely be treated as missing-page faults.
    if (message->event != UFFD_EVENT_PAGEFAULT ||
        (message->arg.pagefault.flags & (UFFD_PAGEFAULT_FLAG_WP | UFFD_PAGEFAULT_FLAG_MINOR)))
        return EPROTO;
    address = (uintptr_t)message->arg.pagefault.address;
    if (address < (uintptr_t)container->base || address - (uintptr_t)container->base >= container->length)
        return EFAULT;
    offset = address - (uintptr_t)container->base;
    region_index = offset / container->region_bytes;
    page_index = (offset % container->region_bytes) / container->base_page_bytes;

    pthread_mutex_lock(&container->mutex);
    ++handler->stats.fault_events;
    region = &container->regions[region_index];
    // Unregister wakes pending faults; old queued messages require no further ioctl.
    if (region->state != THP_TRACKING)
    {
        ++handler->stats.stale_events;
        pthread_mutex_unlock(&container->mutex);
        return 0;
    }
    zero.range.start = address & ~(uintptr_t)(container->base_page_bytes - 1);
    zero.range.len = container->base_page_bytes;
    do
        result = ioctl(container->uffd, UFFDIO_ZEROPAGE, &zero);
    while (result < 0 && errno == EINTR);
    if (result < 0)
    {
        failure = errno;
        // A duplicate event may refer to a page already resolved by an earlier event.
        if (failure == EEXIST)
        {
            do
                result = ioctl(container->uffd, UFFDIO_WAKE, &zero.range);
            while (result < 0 && errno == EINTR);
            failure = result == 0 ? 0 : errno;
        }
    }
    if (failure == 0)
    {
        result = thp_container_touch(container, region_index, page_index);
        if (result < 0)
            failure = errno;
        else if (result > 0)
            ++handler->stats.distinct_pages;
    }

    // The mutex makes the state transition and queue publication atomic to the worker.
    if (failure == 0 && (region->touched_count >= THP_DENSITY_THRESHOLD_PAGES ||
                         region->max_contiguous_run >= THP_RUN_THRESHOLD_PAGES))
    {
        region->state = THP_UNREGISTERING;
        range.start = (uintptr_t)container->base + region_index * container->region_bytes;
        range.len = container->region_bytes;
        do
            result = ioctl(container->uffd, UFFDIO_UNREGISTER, &range);
        while (result < 0 && errno == EINTR);
        if (result < 0)
            failure = errno;
        else
        {
            region->state = THP_QUEUED_FOR_COLLAPSE;
            handler->queue[handler->queue_tail] = region_index;
            ++handler->queue_tail;
            pthread_cond_signal(&handler->work_ready);
        }
    }
    pthread_mutex_unlock(&container->mutex);
    return failure;
}

// Poll both the nonblocking userfaultfd and a dedicated shutdown eventfd.
static void * _fault_main(void * argument)
{
    struct thp_handler * handler = argument;
    struct thp_container * container = handler->container;
    struct pollfd descriptors[] =
    {
        { .fd = container->uffd, .events = POLLIN },
        { .fd = handler->stop_fd, .events = POLLIN }
    };
    struct uffd_msg message;
    ssize_t bytes;
    int failure = 0;

    // One message per iteration keeps shutdown responsive under a sustained fault load.
    for (;;)
    {
        if (poll(descriptors, sizeof(descriptors) / sizeof(descriptors[0]), -1) < 0)
        {
            if (errno == EINTR)
                continue;
            failure = errno;
            break;
        }
        if (descriptors[1].revents & POLLIN)
            break;
        if ((descriptors[0].revents | descriptors[1].revents) & (POLLERR | POLLHUP | POLLNVAL))
        {
            failure = EIO;
            break;
        }
        if (!(descriptors[0].revents & POLLIN))
            continue;
        bytes = read(container->uffd, &message, sizeof(message));
        if (bytes < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (bytes != (ssize_t)sizeof(message))
        {
            failure = bytes < 0 ? errno : EIO;
            break;
        }
        failure = _handle_fault(handler, &message);
        if (failure != 0)
            break;
    }
    // Never leave application threads blocked when the service fails or shuts down.
    pthread_mutex_lock(&container->mutex);
    if (failure != 0 && handler->stats.runtime_errno == 0)
        handler->stats.runtime_errno = failure;
    thp_container_release_registration(container);
    pthread_mutex_unlock(&container->mutex);
    return NULL;
}

// Collapse queued regions outside the metadata lock so fault resolution can continue.
static void * _promotion_main(void * argument)
{
    struct thp_handler * handler = argument;
    struct thp_container * container = handler->container;
    struct thp_region * region;
    struct timespec started;
    struct timespec ended;
    void * address;
    size_t index;
    size_t retries;
    uint64_t elapsed;
    int failure;
    int result;
    int clock_failure;

    for (;;)
    {
        pthread_mutex_lock(&container->mutex);
        while (handler->queue_head == handler->queue_tail && !handler->worker_stop)
            pthread_cond_wait(&handler->work_ready, &container->mutex);
        if (handler->queue_head == handler->queue_tail && handler->worker_stop)
        {
            pthread_mutex_unlock(&container->mutex);
            break;
        }
        index = handler->queue[handler->queue_head];
        ++handler->queue_head;
        region = &container->regions[index];
        region->state = THP_COLLAPSING;
        pthread_mutex_unlock(&container->mutex);

        // Clearing VM_NOHUGEPAGE is legal only after this region is unregistered.
        address = (unsigned char * )container->base + index * container->region_bytes;
        result = madvise(address, container->region_bytes, MADV_HUGEPAGE);
        failure = result == 0 ? 0 : errno;
        retries = 0;
        if (failure == 0)
        {
            for (;;)
            {
                clock_failure = clock_gettime(CLOCK_MONOTONIC, &started) != 0 ? errno : 0;
                result = madvise(address, container->region_bytes, MADV_COLLAPSE);
                failure = result == 0 ? 0 : errno;
                if (clock_gettime(CLOCK_MONOTONIC, &ended) != 0)
                    clock_failure = errno;
                elapsed = clock_failure == 0 ? _elapsed_ns(&started, &ended) : 0;

                // Track failed attempts too; clock failures invalidate timing results.
                pthread_mutex_lock(&container->mutex);
                ++region->collapse_attempts;
                region->collapse_ns += elapsed;
                ++handler->stats.collapse_attempts;
                handler->stats.collapse_ns += elapsed;
                if (failure != 0)
                    ++handler->stats.collapse_failures;
                if (clock_failure != 0 && handler->stats.runtime_errno == 0)
                    handler->stats.runtime_errno = clock_failure;
                pthread_mutex_unlock(&container->mutex);
                if (failure != EAGAIN || retries == THP_COLLAPSE_RETRIES)
                    break;
                ++retries;
            }
        }
        // Selected regions remain eligible for background THP promotion after failure.
        // Observe real THP coverage separately; this state records the explicit syscall.
        pthread_mutex_lock(&container->mutex);
        region->collapse_result = result;
        region->collapse_errno = failure;
        region->state = failure == 0 ? THP_PROMOTED : THP_COLLAPSE_FAILED;
        if (failure == 0)
            ++handler->stats.promoted_regions;
        else
            ++handler->stats.failed_regions;
        pthread_mutex_unlock(&container->mutex);
    }
    return NULL;
}

// [PUBLIC API] ==========================
// Attach exactly one service to a registered container, then start both threads.
int thp_handler_start(struct thp_handler ** output, struct thp_container * container)
{
    struct thp_handler * handler;
    int failure;

    if (output == NULL || *output != NULL || container == NULL ||
        THP_DENSITY_THRESHOLD_PAGES == 0 || THP_DENSITY_THRESHOLD_PAGES > THP_PAGES_PER_REGION ||
        THP_RUN_THRESHOLD_PAGES == 0 || THP_RUN_THRESHOLD_PAGES > THP_PAGES_PER_REGION)
    {
        errno = EINVAL;
        return -1;
    }
    // Ordinary kernel policies must never acquire fault or promotion threads.
    if (container->policy != THP_POLICY_SELECTIVE)
    {
        errno = ENOTSUP;
        return -1;
    }
    // Claim the container before allocating thread state; it cannot be destroyed now.
    pthread_mutex_lock(&container->mutex);
    if (container->handler_attached || container->uffd < 0)
    {
        failure = container->handler_attached ? EBUSY : EINVAL;
        pthread_mutex_unlock(&container->mutex);
        errno = failure;
        return -1;
    }
    container->handler_attached = 1;
    pthread_mutex_unlock(&container->mutex);
    handler = calloc(1, sizeof(*handler));
    if (handler == NULL)
    {
        pthread_mutex_lock(&container->mutex);
        container->handler_attached = 0;
        pthread_mutex_unlock(&container->mutex);
        return -1;
    }
    handler->container = container;
    handler->stop_fd = -1;
    failure = pthread_cond_init(&handler->work_ready, NULL);
    if (failure != 0)
    {
        free(handler);
        pthread_mutex_lock(&container->mutex);
        container->handler_attached = 0;
        pthread_mutex_unlock(&container->mutex);
        errno = failure;
        return -1;
    }

    // The queue is bounded by region count because no region can be queued twice.
    handler->queue = calloc(container->region_count, sizeof(*handler->queue));
    failure = handler->queue == NULL ? ENOMEM : 0;
    if (failure == 0)
    {
        handler->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (handler->stop_fd < 0)
            failure = errno;
    }
    if (failure == 0)
    {
        failure = pthread_create(&handler->promotion_thread, NULL, _promotion_main, handler);
        handler->promotion_started = failure == 0;
    }
    if (failure == 0)
    {
        failure = pthread_create(&handler->fault_thread, NULL, _fault_main, handler);
        handler->fault_started = failure == 0;
    }
    if (failure != 0)
    {
        thp_handler_destroy(&handler);
        errno = failure;
        return -1;
    }
    *output = handler;
    return 0;
}

// Snapshot counters under the same lock used for queue and region state changes.
int thp_handler_snapshot(struct thp_handler * handler, struct thp_handler_stats * stats)
{
    if (handler == NULL || stats == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&handler->container->mutex);
    *stats = handler->stats;
    pthread_mutex_unlock(&handler->container->mutex);
    return 0;
}

// Stop the producer before requesting that the consumer drain and exit.
int thp_handler_stop(struct thp_handler * handler)
{
    uint64_t signal_value = 1;
    int failure;

    if (handler == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (!handler->stopped)
    {
        if (handler->fault_started)
        {
            // An eventfd wakes poll without cancellation or periodic polling delays.
            while (write(handler->stop_fd, &signal_value, sizeof(signal_value)) < 0)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN)
                    break;
                return -1;
            }
            failure = pthread_join(handler->fault_thread, NULL);
            if (failure != 0)
            {
                errno = failure;
                return -1;
            }
            handler->fault_started = 0;
        }
        if (handler->promotion_started)
        {
            pthread_mutex_lock(&handler->container->mutex);
            handler->worker_stop = 1;
            pthread_cond_signal(&handler->work_ready);
            pthread_mutex_unlock(&handler->container->mutex);
            failure = pthread_join(handler->promotion_thread, NULL);
            if (failure != 0)
            {
                errno = failure;
                return -1;
            }
            handler->promotion_started = 0;
        }
        handler->stopped = 1;
    }
    if (handler->stats.runtime_errno != 0)
    {
        errno = handler->stats.runtime_errno;
        return -1;
    }
    return 0;
}

// Free the service only after successful joins, preserving runtime errors for callers.
int thp_handler_destroy(struct thp_handler ** owner)
{
    struct thp_handler * handler;
    int failure;

    if (owner == NULL)
    {
        errno = EINVAL;
        return -1;
    }
    handler = *owner;
    if (handler == NULL)
        return 0;
    failure = thp_handler_stop(handler) == 0 ? 0 : errno;
    if (handler->fault_started || handler->promotion_started)
    {
        errno = failure;
        return -1;
    }
    if (handler->stop_fd >= 0)
        close(handler->stop_fd);
    pthread_cond_destroy(&handler->work_ready);
    free(handler->queue);
    pthread_mutex_lock(&handler->container->mutex);
    handler->container->handler_attached = 0;
    pthread_mutex_unlock(&handler->container->mutex);
    free(handler);
    *owner = NULL;
    if (failure != 0)
    {
        errno = failure;
        return -1;
    }
    return 0;
}
