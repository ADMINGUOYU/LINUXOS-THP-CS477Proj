// file: evaluation.c
# define _GNU_SOURCE
# include "evaluation.h"
# include <errno.h>
# include <inttypes.h>
# include <limits.h>
# include <linux/perf_event.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <sys/ioctl.h>
# include <sys/resource.h>
# include <sys/syscall.h>
# include <time.h>
# include <unistd.h>

// [INTERNAL API] ========================
static int _number(const char * text, uint64_t * value);
static int _pages(struct evaluation_context * context);
static int _arena_memory(struct thp_container * container, size_t * rss, size_t * huge);
static int _report(struct evaluation_context * context, const char * name,
                   uint64_t setup, uint64_t cold, uint64_t wait, uint64_t steady,
                   uint64_t drain, uint64_t total, size_t cold_rss, size_t cold_huge,
                   const struct rusage * before, const struct rusage * after,
                   struct evaluation_counter counters[2]);

// Reject signs, trailing text and overflowing unsigned options.
static int _number(const char * text, uint64_t * value)
{
    char * end;
    unsigned long long parsed;

    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > UINT64_MAX)
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

// Require each run's configuration on argv; the Python driver supplies defaults.
int evaluation_options(int argc, char * argv[], struct evaluation_options * options)
{
    struct sys_config config;
    uint64_t value;
    int index;

    *options = (struct evaluation_options){ 0 };
    options->policy = THP_POLICY_SELECTIVE;
    options->pattern = "dense";
    for (index = 1; index < argc; index += 2)
    {
        if (index + 1 >= argc)
        {
            errno = EINVAL;
            return -1;
        }
        if (strcmp(argv[index], "--results") == 0)
            options->results = argv[index + 1];
        else if (strcmp(argv[index], "--pattern") == 0)
            options->pattern = argv[index + 1];
        else if (strcmp(argv[index], "--policy") == 0)
        {
            if (strcmp(argv[index + 1], "disabled") == 0)
                options->policy = THP_POLICY_DISABLED;
            else if (strcmp(argv[index + 1], "always") == 0)
                options->policy = THP_POLICY_ALWAYS;
            else if (strcmp(argv[index + 1], "khugepaged") == 0)
                options->policy = THP_POLICY_KHUGEPAGED;
            else if (strcmp(argv[index + 1], "selective") != 0)
            {
                errno = EINVAL;
                return -1;
            }
        }
        else
        {
            if (_number(argv[index + 1], &value) != 0 || value > SIZE_MAX)
            {
                errno = EINVAL;
                return -1;
            }
            if (strcmp(argv[index], "--regions") == 0)
                options->regions = (size_t)value;
            else if (strcmp(argv[index], "--iterations") == 0)
                options->iterations = (size_t)value;
            else if (strcmp(argv[index], "--threads") == 0)
                options->threads = (size_t)value;
            else if (strcmp(argv[index], "--wait-ms") == 0)
                options->wait_ms = (size_t)value;
            else if (strcmp(argv[index], "--seed") == 0)
                options->seed = value;
            else
            {
                errno = EINVAL;
                return -1;
            }
        }
    }
    if (options->results == NULL || options->regions == 0 || options->iterations == 0 ||
        options->threads == 0 || options->wait_ms > UINT_MAX ||
        (strcmp(options->pattern, "dense") != 0 && strcmp(options->pattern, "sparse") != 0 &&
         strcmp(options->pattern, "random") != 0 && strcmp(options->pattern, "phase") != 0))
    {
        errno = EINVAL;
        return -1;
    }
    // These assignments occur before any memory or handler lifecycle begins.
    if (load_sys_policy(&config) != 0)
        return -1;
    THP_BASE_PAGE_BYTES = config.base_page_bytes;
    THP_REGION_BYTES = config.region_bytes;
    THP_DENSITY_THRESHOLD_PAGES = config.density_threshold_pages;
    THP_RUN_THRESHOLD_PAGES = config.run_threshold_pages;
    THP_COLLAPSE_RETRIES = config.collapse_retries;
    if (THP_BASE_PAGE_BYTES < sizeof(uint64_t) || THP_REGION_BYTES < THP_BASE_PAGE_BYTES ||
        THP_REGION_BYTES % THP_BASE_PAGE_BYTES != 0 ||
        options->regions > SIZE_MAX / THP_REGION_BYTES)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

// CLOCK_MONOTONIC is unaffected by wall-clock synchronisation.
uint64_t evaluation_now(void)
{
    struct timespec time;

    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
    {
        commons_log("evaluation", LOG_LEVEL_ERROR, "CLOCK_MONOTONIC unavailable");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

// SplitMix64: defined unsigned arithmetic and reproducible seeds on every run.
uint64_t evaluation_random(uint64_t * state)
{
    uint64_t value;

    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

// Sparse and phase reuse select four separated pages per region, or fewer if needed.
static int _pages(struct evaluation_context * context)
{
    size_t pages_per_region = context->container->region_bytes / context->container->base_page_bytes;
    size_t selected = pages_per_region < 4 ? pages_per_region : 4;
    size_t index;
    size_t swap_index;
    size_t temporary;
    int sparse = strcmp(context->options.pattern, "sparse") == 0;

    context->page_count = context->options.regions * (sparse ? selected : pages_per_region);
    if (context->page_count > SIZE_MAX / sizeof(*context->pages))
    {
        errno = EOVERFLOW;
        return -1;
    }
    context->pages = malloc(context->page_count * sizeof(*context->pages));
    if (context->pages == NULL)
        return -1;
    for (index = 0; index < context->page_count; ++index)
        context->pages[index] = sparse ? (index / selected) * pages_per_region +
                               (index % selected) * (pages_per_region / selected) : index;
    // Phase initialises densely, then reuses only a small prefix: a false-positive case.
    context->reuse_count = strcmp(context->options.pattern, "phase") == 0 ?
                           selected : context->page_count;
    if (strcmp(context->options.pattern, "random") == 0)
        for (index = context->page_count; index > 1; --index)
        {
            swap_index = (size_t)(evaluation_random(&context->random_state) % index);
            temporary = context->pages[index - 1];
            context->pages[index - 1] = context->pages[swap_index];
            context->pages[swap_index] = temporary;
        }
    return 0;
}

// Indirection keeps sparse pages physically separated within every region.
unsigned char * evaluation_page(struct evaluation_context * context, size_t index)
{
    return (unsigned char * )context->container->base +
           context->pages[index] * context->container->base_page_bytes;
}

// Read process totals independently of the arena-only samples.
int evaluation_memory(size_t * rss_kib, size_t * huge_kib)
{
    FILE * stream = fopen("/proc/self/smaps_rollup", "r");
    char line[512];
    int found = 0;

    if (stream == NULL)
        return -1;
    while (fgets(line, sizeof(line), stream) != NULL)
    {
        if (sscanf(line, "Rss: %zu kB", rss_kib) == 1)
            found |= 1;
        if (sscanf(line, "AnonHugePages: %zu kB", huge_kib) == 1)
            found |= 2;
    }
    if (ferror(stream))
        found = 0;
    fclose(stream);
    if (found != 3)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

// Only whole VMAs contained within the arena can be attributed without guessing.
static int _arena_memory(struct thp_container * container, size_t * rss, size_t * huge)
{
    FILE * stream = fopen("/proc/self/smaps", "r");
    char line[512];
    unsigned long start;
    unsigned long end;
    size_t value;
    size_t covered = 0;
    uintptr_t base = (uintptr_t)container->base;
    int include = 0;
    int failed;

    *rss = 0;
    *huge = 0;
    if (stream == NULL)
        return -1;
    while (fgets(line, sizeof(line), stream) != NULL)
    {
        if (sscanf(line, "%lx-%lx", &start, &end) == 2)
        {
            include = start >= base && end <= base + container->length;
            if (include)
                covered += end - start;
        }
        else if (include && sscanf(line, "Rss: %zu kB", &value) == 1)
            *rss += value;
        else if (include && sscanf(line, "AnonHugePages: %zu kB", &value) == 1)
            *huge += value;
    }
    failed = ferror(stream) || covered != container->length;
    fclose(stream);
    if (failed)
    {
        errno = ENODATA;
        return -1;
    }
    return 0;
}

// Generic dTLB read counters, scoped to the steady-state thread and user space.
void evaluation_counter_open(struct evaluation_counter * counter, uint64_t operation)
{
    struct perf_event_attr attr = { 0 };

    *counter = (struct evaluation_counter){ .fd = -1 };
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HW_CACHE;
    attr.config = PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (operation << 16);
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
# ifdef SYS_perf_event_open
    counter->fd = (int)syscall(SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
# else
    errno = ENOSYS;
# endif
    if (counter->fd < 0)
        counter->error = errno;
}

// Counter failures remain availability metadata, never a reason to rerun a sample.
void evaluation_counter_start(struct evaluation_counter * counter)
{
    if (counter->fd >= 0 && (ioctl(counter->fd, PERF_EVENT_IOC_RESET, 0) != 0 ||
                            ioctl(counter->fd, PERF_EVENT_IOC_ENABLE, 0) != 0))
        counter->error = errno;
}

// Keep raw counts and multiplexing times; consumers must not compare unscaled counts.
void evaluation_counter_stop(struct evaluation_counter * counter)
{
    if (counter->fd < 0)
        return;
    if (ioctl(counter->fd, PERF_EVENT_IOC_DISABLE, 0) != 0 && counter->error == 0)
        counter->error = errno;
    if (read(counter->fd, counter->values, sizeof(counter->values)) != (ssize_t)sizeof(counter->values) &&
        counter->error == 0)
        counter->error = EIO;
    if (counter->values[2] == 0 && counter->error == 0)
        counter->error = ENODATA;
    close(counter->fd);
    counter->fd = -1;
}

// Write raw measurements after stopping the handler, before unmapping the arena.
static int _report(struct evaluation_context * context, const char * name,
                   uint64_t setup, uint64_t cold, uint64_t wait, uint64_t steady,
                   uint64_t drain, uint64_t total, size_t cold_rss, size_t cold_huge,
                   const struct rusage * before, const struct rusage * after,
                   struct evaluation_counter counters[2])
{
    struct thp_handler_stats stats = { 0 };
    struct thp_region region = { 0 };
    size_t rss = 0;
    size_t huge = 0;
    size_t arena_rss = 0;
    size_t arena_huge = 0;
    size_t index;
    int arena_error;
    int failure = 0;
    char * temporary;
    FILE * stream;

    if (evaluation_memory(&rss, &huge) != 0)
        return -1;
    arena_error = _arena_memory(context->container, &arena_rss, &arena_huge) == 0 ? 0 : errno;
    if (context->handler != NULL && thp_handler_snapshot(context->handler, &stats) != 0)
        return -1;
    temporary = malloc(strlen(context->options.results) + sizeof(".tmp"));
    if (temporary == NULL)
        return -1;
    sprintf(temporary, "%s.tmp", context->options.results);
    stream = fopen(temporary, "wx");
    if (stream == NULL)
    {
        free(temporary);
        return -1;
    }
    // Strings here are fixed workload/pattern identifiers, never arbitrary JSON input.
    fprintf(stream, "{\n\"schema_version\":1,\"test\":\"%s\",\"pattern\":\"%s\",\"policy\":%d,"
            "\"regions\":%zu,\"base_page_bytes\":%zu,\"region_bytes\":%zu,"
            "\"iterations\":%zu,\"seed\":%" PRIu64 ",\"populated_pages\":%zu,\"reused_pages\":%zu,\n",
            name, context->options.pattern, context->options.policy, context->options.regions,
            THP_BASE_PAGE_BYTES, THP_REGION_BYTES, context->options.iterations, context->options.seed,
            context->page_count, context->reuse_count);
    fprintf(stream, "\"setup_ns\":%" PRIu64 ",\"cold_ns\":%" PRIu64 ",\"wait_ns\":%" PRIu64
            ",\"steady_ns\":%" PRIu64 ",\"drain_ns\":%" PRIu64 ",\"total_ns\":%" PRIu64
            ",\"operations\":%zu,\"checksum\":%" PRIu64 ",\n",
            setup, cold, wait, steady, drain, total, context->operations, context->checksum);
    fprintf(stream, "\"threads\":%zu,\"density_threshold_pages\":%zu,\"run_threshold_pages\":%zu,"
            "\"collapse_retries\":%zu,\n", context->options.threads, THP_DENSITY_THRESHOLD_PAGES,
            THP_RUN_THRESHOLD_PAGES, THP_COLLAPSE_RETRIES);
    fprintf(stream, "\"cold_rss_kib\":%zu,\"cold_anon_huge_kib\":%zu,\"rss_kib\":%zu,"
            "\"anon_huge_kib\":%zu,\"peak_rss_kib\":%ld,\"arena_memory_errno\":%d,"
            "\"arena_rss_kib\":%zu,\"arena_anon_huge_kib\":%zu,"
            "\"minor_faults\":%ld,\"major_faults\":%ld,\n", cold_rss, cold_huge, rss, huge,
            after->ru_maxrss, arena_error, arena_rss, arena_huge,
            after->ru_minflt - before->ru_minflt, after->ru_majflt - before->ru_majflt);
    fprintf(stream, "\"perf_scope\":\"steady_main_thread_user\",\"perf\":[");
    for (index = 0; index < 2; ++index)
        fprintf(stream, "%s{\"event\":\"%s\",\"errno\":%d,\"raw\":%" PRIu64
                ",\"time_enabled\":%" PRIu64 ",\"time_running\":%" PRIu64 "}",
                index == 0 ? "" : ",", index == 0 ? "dTLB-loads" : "dTLB-load-misses",
                counters[index].error, counters[index].values[0], counters[index].values[1], counters[index].values[2]);
    fprintf(stream, "],\n\"fault_events\":%zu,\"distinct_pages\":%zu,\"stale_events\":%zu,"
            "\"collapse_attempts\":%zu,\"collapse_failures\":%zu,\"collapse_ns\":%" PRIu64
            ",\"promoted_regions\":%zu,\"failed_regions\":%zu,\"runtime_errno\":%d,\"region_stats\":[",
            stats.fault_events, stats.distinct_pages, stats.stale_events, stats.collapse_attempts,
            stats.collapse_failures, stats.collapse_ns, stats.promoted_regions, stats.failed_regions, stats.runtime_errno);
    // Per-region outcomes distinguish a syscall success from actual THP coverage.
    for (index = 0; context->handler != NULL && index < context->options.regions; ++index)
    {
        if (thp_container_region(context->container, index, &region) != 0)
        {
            failure = errno;
            break;
        }
        fprintf(stream, "%s{\"index\":%zu,\"touched_count\":%zu,\"max_contiguous_run\":%zu,"
                "\"state\":%d,\"collapse_attempts\":%zu,\"collapse_ns\":%" PRIu64
                ",\"collapse_result\":%d,\"collapse_errno\":%d}", index == 0 ? "" : ",", index,
                region.touched_count, region.max_contiguous_run, region.state, region.collapse_attempts,
                region.collapse_ns, region.collapse_result, region.collapse_errno);
        thp_container_region_destroy(&region);
    }
    fprintf(stream, "],\"complete\":true}\n");
    if (ferror(stream) && failure == 0)
        failure = EIO;
    if (fclose(stream) != 0 && failure == 0)
        failure = errno;
    if (failure == 0 && rename(temporary, context->options.results) != 0)
        failure = errno;
    if (failure != 0)
        unlink(temporary);
    free(temporary);
    errno = failure;
    return failure == 0 ? 0 : -1;
}

// A complete isolated sample, with no system-setting changes or privileged actions.
int evaluation_main(int argc, char * argv[], const struct evaluation_workload * workload)
{
    struct evaluation_context context = { 0 };
    struct evaluation_counter counters[2];
    struct rusage before;
    struct rusage after;
    struct timespec delay;
    uint64_t start;
    uint64_t setup = 0;
    uint64_t cold = 0;
    uint64_t waited = 0;
    uint64_t steady = 0;
    uint64_t drain = 0;
    uint64_t stamp;
    uint64_t total;
    size_t cold_rss = 0;
    size_t cold_huge = 0;
    size_t index;
    int failure = 0;

    if (evaluation_options(argc, argv, &context.options) != 0)
    {
        commons_log(workload->name, LOG_LEVEL_ERROR, strerror(errno));
        return EXIT_FAILURE;
    }
    context.random_state = context.options.seed;
    for (index = 0; index < 2; ++index)
        evaluation_counter_open(&counters[index], index == 0 ? PERF_COUNT_HW_CACHE_RESULT_ACCESS : PERF_COUNT_HW_CACHE_RESULT_MISS);
    if (getrusage(RUSAGE_SELF, &before) != 0)
        failure = errno;
    start = evaluation_now();
    if (failure == 0 && thp_container_create(&context.container, context.options.regions, context.options.policy) != 0)
        failure = errno;
    if (failure == 0 && context.options.policy == THP_POLICY_SELECTIVE &&
        thp_handler_start(&context.handler, context.container) != 0)
        failure = errno;
    if (failure == 0 && _pages(&context) != 0)
        failure = errno;
    setup = evaluation_now() - start;

    // Cold work is separate from steady-state reuse and the optional promotion wait.
    stamp = evaluation_now();
    if (failure == 0 && workload->prepare(&context) != 0)
        failure = errno;
    cold = evaluation_now() - stamp;
    if (failure == 0 && evaluation_memory(&cold_rss, &cold_huge) != 0)
        failure = errno;
    stamp = evaluation_now();
    if (failure == 0 && context.options.policy == THP_POLICY_KHUGEPAGED &&
        thp_container_enable_khugepaged(context.container) != 0)
        failure = errno;
    delay.tv_sec = (time_t)(context.options.wait_ms / 1000);
    delay.tv_nsec = (long)(context.options.wait_ms % 1000) * 1000000L;
    while (failure == 0 && nanosleep(&delay, &delay) != 0)
        if (errno != EINTR)
            failure = errno;
    waited = evaluation_now() - stamp;
    for (index = 0; index < 2; ++index)
        evaluation_counter_start(&counters[index]);
    stamp = evaluation_now();
    // [RUN WORKLOAD] ===============================
    if (failure == 0 && workload->run(&context) != 0)
        failure = errno;
    // [RUN WORKLOAD] ENDs ==========================
    steady = evaluation_now() - stamp;
    for (index = 0; index < 2; ++index)
        evaluation_counter_stop(&counters[index]);

    // Drain promotion work before sampling final coverage and region statistics.
    stamp = evaluation_now();
    if (context.handler != NULL && thp_handler_stop(context.handler) != 0 && failure == 0)
        failure = errno;
    drain = evaluation_now() - stamp;
    total = evaluation_now() - start;
    if (getrusage(RUSAGE_SELF, &after) != 0 && failure == 0)
        failure = errno;
    if (failure == 0 && _report(&context, workload->name, setup, cold, waited, steady, drain, total,
                               cold_rss, cold_huge, &before, &after, counters) != 0)
        failure = errno;
    workload->release(&context);
    free(context.pages);
    if (thp_handler_destroy(&context.handler) != 0 && failure == 0)
        failure = errno;
    if (thp_container_destroy(context.container) != 0 && failure == 0)
        failure = errno;
    commons_log(workload->name, failure == 0 ? LOG_LEVEL_OK : LOG_LEVEL_ERROR,
                failure == 0 ? "Sample complete" : strerror(failure));
    return failure == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
