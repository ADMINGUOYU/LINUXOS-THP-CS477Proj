// file: allocation.c
// glibc versus TCMalloc using ordinary malloc()/free() under the same host THP settings
// nothing to do with custom thp settings

# define _GNU_SOURCE
# include "evaluation.h"
# include <dlfcn.h>
# include <errno.h>
# include <inttypes.h>
# include <linux/perf_event.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <sys/resource.h>
# include <unistd.h>

// Allocator-owned memory deliberately bypasses thp_container and its selective policy.
static size_t _size(uint64_t * state);
static void _populate(unsigned char * address, size_t size, unsigned char marker);
static int _allocator(void);

// Mix tiny and multi-page objects; the same seed yields identical allocation requests.
static size_t _size(uint64_t * state)
{
    return (size_t)64 << (evaluation_random(state) % 8);
}

// Touch every base page and the tail so malloc reservations become resident memory.
static void _populate(unsigned char * address, size_t size, unsigned char marker)
{
    size_t offset;

    for (offset = 0; offset < size; offset += THP_BASE_PAGE_BYTES)
        address[offset] = marker;
    address[size - 1] = marker;
}

// Confirm malloc actually comes from the requested preload, rather than labelling a fallback.
static int _allocator(void)
{
    Dl_info info;
    const char * expected = getenv("EVALUATION_ALLOCATOR_LIBRARY");
    char * actual_path;
    char * expected_path;
    int matches;

    if (dladdr(dlsym(RTLD_DEFAULT, "malloc"), &info) == 0 || info.dli_fname == NULL)
        return -1;
    if (expected == NULL)
        return strstr(info.dli_fname, "libc.so") != NULL ? 0 : -1;
    actual_path = realpath(info.dli_fname, NULL);
    expected_path = realpath(expected, NULL);
    matches = actual_path != NULL && expected_path != NULL && strcmp(actual_path, expected_path) == 0;
    free(actual_path);
    free(expected_path);
    return matches ? 0 : -1;
}

// One allocator per process; Python sets LD_PRELOAD only on the TCMalloc child.
int main(int argc, char * argv[])
{
    struct evaluation_options options;
    struct evaluation_counter counters[2];
    struct rusage usage;
    struct rusage before;
    unsigned char * * pointers;
    size_t * sizes;
    size_t count;
    size_t index;
    size_t iteration;
    size_t rss = 0;
    size_t huge = 0;
    size_t cold_rss = 0;
    size_t cold_huge = 0;
    uint64_t state;
    uint64_t checksum = 0;
    uint64_t begin;
    uint64_t stamp;
    uint64_t cold;
    uint64_t steady;
    uint64_t total;
    size_t operations = 0;
    FILE * stream;
    char * temporary;
    int failure = 0;

    if (evaluation_options(argc, argv, &options) != 0)
    {
        commons_log("allocation", LOG_LEVEL_ERROR, strerror(errno));
        return EXIT_FAILURE;
    }
    if (options.policy == THP_POLICY_SELECTIVE || options.policy == THP_POLICY_KHUGEPAGED ||
        _allocator() != 0)
    {
        commons_log("allocation", LOG_LEVEL_ERROR,
                    "Use disabled/always host policies and a verified glibc or TCMalloc provider");
        return EXIT_FAILURE;
    }
    // The live-object count derives from the same region-sized memory budget.
    count = options.regions * THP_REGION_BYTES / THP_BASE_PAGE_BYTES;
    if (count > SIZE_MAX / sizeof(*pointers) || count > SIZE_MAX / sizeof(*sizes))
        return EXIT_FAILURE;
    pointers = calloc(count, sizeof(*pointers));
    sizes = calloc(count, sizeof(*sizes));
    if (pointers == NULL || sizes == NULL)
    {
        free(pointers);
        free(sizes);
        return EXIT_FAILURE;
    }
    state = options.seed;
    for (index = 0; index < 2; ++index)
        evaluation_counter_open(&counters[index], index == 0 ? PERF_COUNT_HW_CACHE_RESULT_ACCESS : PERF_COUNT_HW_CACHE_RESULT_MISS);
    if (getrusage(RUSAGE_SELF, &before) != 0)
        failure = errno;
    begin = evaluation_now();
    for (index = 0; failure == 0 && index < count; ++index)
    {
        sizes[index] = _size(&state);
        pointers[index] = malloc(sizes[index]);
        if (pointers[index] == NULL)
            failure = ENOMEM;
        else
            _populate(pointers[index], sizes[index], (unsigned char)index);
    }
    cold = evaluation_now() - begin;
    if (failure == 0 && evaluation_memory(&cold_rss, &cold_huge) != 0)
        failure = errno;
    for (index = 0; index < 2; ++index)
        evaluation_counter_start(&counters[index]);
    stamp = evaluation_now();
    // Churn objects in place, checking old payloads before every free.
    for (iteration = 0; failure == 0 && iteration < options.iterations; ++iteration)
        for (index = 0; failure == 0 && index < count; ++index)
        {
            if (pointers[index][sizes[index] - 1] != (unsigned char)index)
                failure = EILSEQ;
            checksum += pointers[index][sizes[index] - 1];
            free(pointers[index]);
            sizes[index] = _size(&state);
            pointers[index] = malloc(sizes[index]);
            if (pointers[index] == NULL)
                failure = ENOMEM;
            else
                _populate(pointers[index], sizes[index], (unsigned char)index);
            ++operations;
        }
    steady = evaluation_now() - stamp;
    for (index = 0; index < 2; ++index)
        evaluation_counter_stop(&counters[index]);
    total = evaluation_now() - begin;
    if (getrusage(RUSAGE_SELF, &usage) != 0 && failure == 0)
        failure = errno;
    if (failure == 0 && evaluation_memory(&rss, &huge) != 0)
        failure = errno;
    // Sample memory while objects remain live, then release every allocation.
    for (index = 0; index < count; ++index)
        free(pointers[index]);
    free(pointers);
    free(sizes);
    temporary = malloc(strlen(options.results) + sizeof(".tmp"));
    if (temporary == NULL && failure == 0)
        failure = ENOMEM;
    if (failure == 0)
    {
        sprintf(temporary, "%s.tmp", options.results);
        stream = fopen(temporary, "wx");
        if (stream == NULL)
            failure = errno;
        else
        {
            fprintf(stream, "{\"schema_version\":1,\"test\":\"allocation\",\"pattern\":\"churn\","
                    "\"policy\":%d,\"allocator_verified\":true,\"regions\":%zu,\"iterations\":%zu,"
                    "\"seed\":%" PRIu64 ",\"live_objects\":%zu,\"cold_ns\":%" PRIu64
                    ",\"steady_ns\":%" PRIu64 ",\"total_ns\":%" PRIu64 ",\"operations\":%zu,"
                    "\"checksum\":%" PRIu64 ",\"cold_rss_kib\":%zu,\"cold_anon_huge_kib\":%zu,"
                    "\"rss_kib\":%zu,\"anon_huge_kib\":%zu,\"peak_rss_kib\":%ld,"
                    "\"minor_faults\":%ld,\"major_faults\":%ld,\"perf_scope\":\"steady_main_thread_user\",\"perf\":[",
                    options.policy, options.regions, options.iterations, options.seed, count, cold, steady, total,
                    operations, checksum, cold_rss, cold_huge, rss, huge, usage.ru_maxrss,
                    usage.ru_minflt - before.ru_minflt, usage.ru_majflt - before.ru_majflt);
            for (index = 0; index < 2; ++index)
                fprintf(stream, "%s{\"event\":\"%s\",\"errno\":%d,\"raw\":%" PRIu64
                        ",\"time_enabled\":%" PRIu64 ",\"time_running\":%" PRIu64 "}", index == 0 ? "" : ",",
                        index == 0 ? "dTLB-loads" : "dTLB-load-misses", counters[index].error,
                        counters[index].values[0], counters[index].values[1], counters[index].values[2]);
            fprintf(stream, "],\"complete\":true}\n");
            if (ferror(stream))
                failure = EIO;
            if (fclose(stream) != 0 && failure == 0)
                failure = errno;
            if (failure == 0 && rename(temporary, options.results) != 0)
                failure = errno;
            if (failure != 0)
                unlink(temporary);
        }
    }
    free(temporary);
    commons_log("allocation", failure == 0 ? LOG_LEVEL_OK : LOG_LEVEL_ERROR,
                failure == 0 ? "Sample complete" : strerror(failure));
    return failure == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
