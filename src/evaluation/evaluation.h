// file: evaluation.h
# ifndef EVALUATION_H
# define EVALUATION_H

# include "commons.h"
# include "thp_container.h"
# include "thp_handler.h"
# include <stdint.h>

// One process is one sample; all sizes and the random seed are explicit arguments.
struct evaluation_options
{
    const char * results; // private result path supplied by the Python driver
    const char * pattern; // dense, sparse, random, or phase
    enum thp_policy_mode policy; // mapping policy, independent of host settings
    size_t regions; // number of aligned regions
    size_t iterations; // steady-state repetitions within this sample
    size_t threads; // correctness workers (benchmark kernels are single-threaded)
    size_t wait_ms; // observation interval after population, equal across policies
    uint64_t seed; // reproducible access order
};

// The event names and read format are Linux perf ABI values, not host settings.
struct evaluation_counter
{
    int fd; // owned descriptor or -1 if unavailable
    int error; // availability/read error, emitted instead of a fabricated zero
    uint64_t values[3]; // raw count, time enabled, time running
};

// Workload state; helper arrays are outside the measured container mapping.
struct evaluation_context
{
    struct evaluation_options options; // parsed invocation
    struct thp_container * container; // owned mapping
    struct thp_handler * handler; // present only for selective mode
    size_t * pages; // physical page indices in the population order
    size_t page_count; // number of populated pages
    size_t reuse_count; // number revisited in the steady phase
    void * scratch; // optional workload-owned allocation
    uint64_t random_state; // local deterministic generator state
    uint64_t checksum; // observable result prevents dead-code elimination
    size_t operations; // logical steady-state operations
};

// Callbacks return 0 or -1/errno. Release scratch even after a partial prepare.
struct evaluation_workload
{
    const char * name; // fixed result identifier
    int (*prepare)(struct evaluation_context * context);
    int (*run)(struct evaluation_context * context);
    void (*release)(struct evaluation_context * context);
};

// Shared main implements lifecycle, timing, memory samples and atomic result output.
int evaluation_main(int argc, char * argv[], const struct evaluation_workload * workload);
// Parse arguments and apply merged policy configuration before any container exists.
int evaluation_options(int argc, char * argv[], struct evaluation_options * options);
// Monotonic wall-clock timestamp; reports a fatal clock error rather than false timing.
uint64_t evaluation_now(void);
// Deterministic PRNG, with no global libc random state.
uint64_t evaluation_random(uint64_t * state);
// Resolve one selected page without touching the mapping.
unsigned char * evaluation_page(struct evaluation_context * context, size_t index);
// Process-wide current memory usage, in KiB; failures return -1/errno.
int evaluation_memory(size_t * rss_kib, size_t * huge_kib);

// Optional user-space main-thread counters; error fields identify unavailable events.
void evaluation_counter_open(struct evaluation_counter * counter, uint64_t operation);
void evaluation_counter_start(struct evaluation_counter * counter);
void evaluation_counter_stop(struct evaluation_counter * counter);

# endif // EVALUATION_H
