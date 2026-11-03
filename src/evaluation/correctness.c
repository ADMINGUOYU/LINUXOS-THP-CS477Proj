// file: correctness.c
# define _POSIX_C_SOURCE 200809L
# include "evaluation.h"
# include <errno.h>
# include <stdlib.h>
# include <time.h>

// A condition-variable gate starts competing first touches without a C data race.
struct race_group
{
    struct evaluation_context * context; // mapping and page order shared by workers
    pthread_mutex_t mutex; // protects gate opening
    pthread_cond_t ready; // workers wait here until all are created
    int start; // opens even after partial thread creation, so cleanup never deadlocks
};

struct race_worker
{
    struct race_group * group; // shared start gate
    size_t index; // owns a separate byte on each page
    int failure; // written only by this worker, read after join
};

static void * _touch(void * argument);
static int _prepare(struct evaluation_context * context);
static int _run(struct evaluation_context * context);
static void _release(struct evaluation_context * context);

// Concurrent faults target the same pages, while writes target disjoint bytes.
static void * _touch(void * argument)
{
    struct race_worker * worker = argument;
    struct evaluation_context * context = worker->group->context;
    size_t page;
    size_t iteration;
    unsigned char * address;
    unsigned char expected = (unsigned char)(worker->index + 1);

    pthread_mutex_lock(&worker->group->mutex);
    while (!worker->group->start)
        pthread_cond_wait(&worker->group->ready, &worker->group->mutex);
    pthread_mutex_unlock(&worker->group->mutex);
    for (iteration = 0; iteration < context->options.iterations; ++iteration)
        for (page = 0; page < context->page_count; ++page)
        {
            address = evaluation_page(context, page) + worker->index;
            if (*address != (iteration == 0 ? 0 : expected))
                worker->failure = EILSEQ;
            *address = expected;
        }
    return NULL;
}

// Exercise competing faults, repeated accesses and ownership guards before reporting.
static int _prepare(struct evaluation_context * context)
{
    struct race_group group = { .context = context };
    struct race_worker * workers;
    pthread_t * threads;
    struct thp_handler * duplicate = NULL;
    size_t created = 0;
    size_t index;
    int failure;
    int result;

    if (context->options.threads > 255 || context->options.threads > THP_BASE_PAGE_BYTES)
    {
        errno = EINVAL;
        return -1;
    }
    // An attached selective handler must prevent duplicate attachment and destruction.
    if (context->handler != NULL)
    {
        if (thp_handler_start(&duplicate, context->container) != -1 || errno != EBUSY ||
            thp_container_destroy(context->container) != -1 || errno != EBUSY)
        {
            commons_log("correctness", LOG_LEVEL_ERROR, "Container ownership guard failed");
            exit(EXIT_FAILURE);
        }
    }
    failure = pthread_mutex_init(&group.mutex, NULL);
    if (failure != 0)
    {
        errno = failure;
        return -1;
    }
    failure = pthread_cond_init(&group.ready, NULL);
    if (failure != 0)
    {
        pthread_mutex_destroy(&group.mutex);
        errno = failure;
        return -1;
    }
    workers = calloc(context->options.threads, sizeof(*workers));
    threads = calloc(context->options.threads, sizeof(*threads));
    if (workers == NULL || threads == NULL)
        failure = ENOMEM;
    for (index = 0; failure == 0 && index < context->options.threads; ++index)
    {
        workers[index].group = &group;
        workers[index].index = index;
        failure = pthread_create(&threads[index], NULL, _touch, &workers[index]);
        if (failure == 0)
            ++created;
    }
    // Release every successfully created thread, including partial-start failure paths.
    pthread_mutex_lock(&group.mutex);
    group.start = 1;
    pthread_cond_broadcast(&group.ready);
    pthread_mutex_unlock(&group.mutex);
    for (index = 0; index < created; ++index)
    {
        result = pthread_join(threads[index], NULL);
        if (result != 0)
        {
            commons_log("correctness", LOG_LEVEL_ERROR, "Cannot safely release an unjoined worker");
            exit(EXIT_FAILURE);
        }
        if (failure == 0 && workers[index].failure != 0)
            failure = workers[index].failure;
    }
    free(workers);
    free(threads);
    pthread_cond_destroy(&group.ready);
    pthread_mutex_destroy(&group.mutex);
    errno = failure;
    return failure == 0 ? 0 : -1;
}

// Verify bytes after promotion and validate snapshot invariants after fault accounting.
static int _run(struct evaluation_context * context)
{
    struct thp_region region = { 0 };
    size_t page;
    size_t thread;
    size_t index;
    size_t bit;
    size_t count;
    size_t run;
    size_t longest;
    size_t expected;
    size_t pages_per_region = THP_PAGES_PER_REGION;
    uint64_t deadline;
    struct timespec pause = { .tv_nsec = 1000000L };
    int settled;
    int failure = 0;

    // Never inspect another worker's byte until all workers have joined.
    for (page = 0; page < context->page_count; ++page)
        for (thread = 0; thread < context->options.threads; ++thread)
        {
            if (evaluation_page(context, page)[thread] != (unsigned char)(thread + 1))
                failure = EILSEQ;
            context->checksum += evaluation_page(context, page)[thread];
            ++context->operations;
        }
    for (index = 0; failure == 0 && context->handler != NULL && index < context->options.regions; ++index)
    {
        expected = 0;
        for (page = 0; page < context->page_count; ++page)
            if (context->pages[page] / pages_per_region == index)
                ++expected;
        // ZEROPAGE wakes the application before the handler updates its bitmap.
        deadline = evaluation_now() + UINT64_C(2000000000);
        do
        {
            thp_container_region_destroy(&region);
            if (thp_container_region(context->container, index, &region) != 0)
                return -1;
            settled = region.state != THP_TRACKING || region.touched_count == expected;
            if (!settled)
                nanosleep(&pause, NULL);
        }
        while (!settled && evaluation_now() < deadline);
        count = 0;
        run = 0;
        longest = 0;
        for (bit = 0; bit < pages_per_region; ++bit)
        {
            if ((region.bitmap[bit / THP_BITMAP_WORD_BITS] &
                 (UINT64_C(1) << (bit % THP_BITMAP_WORD_BITS))) != 0)
            {
                ++count;
                ++run;
                if (run > longest)
                    longest = run;
            }
            else
                run = 0;
        }
        if (!settled || count != region.touched_count || longest != region.max_contiguous_run || count > expected)
            failure = EILSEQ;
        if (region.state == THP_TRACKING && (count != expected ||
            count >= THP_DENSITY_THRESHOLD_PAGES || longest >= THP_RUN_THRESHOLD_PAGES))
            failure = EILSEQ;
        if (region.state != THP_TRACKING && count < THP_DENSITY_THRESHOLD_PAGES && longest < THP_RUN_THRESHOLD_PAGES)
            failure = EILSEQ;
        // A detached snapshot survives destruction and its cleanup is idempotent.
        thp_container_region_destroy(&region);
        thp_container_region_destroy(&region);
        if (region.bitmap != NULL || region.touched_count != 0)
            failure = EILSEQ;
    }
    errno = failure;
    return failure == 0 ? 0 : -1;
}

// The preparation workers release their own allocations before returning.
static void _release(struct evaluation_context * context)
{
    (void)context;
    thp_container_region_destroy(NULL);
}

// Correctness timings are diagnostic only and must not enter benchmark summaries.
int main(int argc, char * argv[])
{
    const struct evaluation_workload workload = { "correctness", _prepare, _run, _release };

    return evaluation_main(argc, argv, &workload);
}
