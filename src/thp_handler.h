// file: thp_handler.h
# ifndef THP_HANDLER_H
# define THP_HANDLER_H

# include <stddef.h>
# include <stdint.h>
# include "thp_containar.h"

// [CONFIGURATION] =======================
/** 
 *  TODO: sync these with the json configuration file 
 *  Currently these macros are the sole geometry configuration; JSON is not read.
*/
// Counts are base pages, combined with OR; retries are additional EAGAIN attempts.
# define THP_DENSITY_THRESHOLD_PAGES ((size_t)384)
# define THP_RUN_THRESHOLD_PAGES ((size_t)256)
# define THP_COLLAPSE_RETRIES ((size_t)3)
// [CONFIGURATION] ENDs ==================

// [STATISTICS] ==========================
// Counters describe observed first touches and explicit collapse syscalls only.
// A successful syscall does not guarantee a THP remains resident indefinitely.
struct thp_handler_stats
{
    size_t fault_events;
    size_t distinct_pages;
    size_t stale_events;
    size_t collapse_attempts;
    size_t collapse_failures; // failed MADV_COLLAPSE attempts, including retries
    size_t promoted_regions;
    size_t failed_regions; // includes MADV_HUGEPAGE advice failures
    uint64_t collapse_ns; // time inside MADV_COLLAPSE, including failed attempts
    int runtime_errno; // nonzero means tracking failed; discard experiment results
};

// Opaque thread/queue owner; the containar module owns mapping and metadata.
struct thp_handler;

// [PUBLIC API] ==========================
// Pass a newly registered, untouched containar. Output must initially be NULL.
// Requires THP_POLICY_SELECTIVE; other policies return -1/ENOTSUP.
// Thresholds are base-page counts, combined with OR. Retries follow EAGAIN only.
// No JSON/configuration loading or global sysctl changes occur in either module.
int thp_handler_start(struct thp_handler ** output, struct thp_containar * containar);
// Obtain counters while the service threads are running or after stop.
int thp_handler_snapshot(struct thp_handler * handler, struct thp_handler_stats * stats);
// First join all application threads accessing the mapping, then stop the handler.
// Stops registration and drains queued collapses; does not unmap the containar.
// Idempotent. Returns -1/errno for fatal tracking errors even after a clean stop.
int thp_handler_stop(struct thp_handler * handler);
// Stops, joins and frees the handler, then sets *handler to NULL and detaches it.
// On a tracking error resources are still freed if joins succeeded; returns -1.
// Destroy the containar afterwards. Lifecycle calls must be serialised by the owner.
int thp_handler_destroy(struct thp_handler ** handler);

// A fatal handler error closes userfaultfd to release blocked application threads.
// Those threads can finish using ordinary faults, but resulting measurements are invalid.
// No cancellation: an in-progress synchronous kernel collapse must return before join.
// Typical ownership sequence (check all return values):
//   thp_containar_create(&containar, region_count, THP_POLICY_SELECTIVE);
//   thp_handler_start(&handler, containar);
//   ... userspace workload accesses containar->base; join all workload threads ...
//   thp_handler_stop(handler);
//   thp_handler_snapshot(handler, &stats);
//   thp_containar_region(containar, index, &region);
//   thp_handler_destroy(&handler);
//   thp_containar_destroy(containar);
# endif // THP_HANDLER_H
