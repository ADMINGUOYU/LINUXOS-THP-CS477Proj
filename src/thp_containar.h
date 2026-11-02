// file: thp_containar.h
# ifndef THP_CONTAINAR_H
# define THP_CONTAINAR_H

# include <stddef.h>
# include <stdint.h>
# include <limits.h>
# include <pthread.h>

// [CONFIGURATION] =======================
/** 
 *  TODO: sync these with the json configuration file 
 *  Currently these macros are the sole geometry configuration; JSON is not read.
*/
# define THP_BASE_PAGE_BYTES ((size_t)4096)
# define THP_REGION_BYTES ((size_t)2097152)
# define THP_PAGES_PER_REGION (THP_REGION_BYTES / THP_BASE_PAGE_BYTES)
// Derived bitmap geometry: eight uint64_t words for 512 base pages.
# define THP_BITMAP_WORD_BITS (sizeof(uint64_t) * CHAR_BIT)
# define THP_BITMAP_WORD_COUNT ((THP_PAGES_PER_REGION + THP_BITMAP_WORD_BITS - 1) / THP_BITMAP_WORD_BITS)
// [CONFIGURATION] ENDs ==================

// [METADATA] ============================
// State transitions are protected by the container mutex (C99)
// NOTE: as state transitions (maintained by threaded handlers)
enum thp_region_state
{
    THP_TRACKING,
    THP_UNREGISTERING,
    THP_QUEUED_FOR_COLLAPSE,
    THP_COLLAPSING,
    THP_PROMOTED,
    THP_COLLAPSE_FAILED,
    THP_TRACKING_STOPPED
};

// Stored outside the registered mapping. Counts stop when registration is removed.
struct thp_region
{
    uint64_t bitmap[THP_BITMAP_WORD_COUNT]; // 1 bit per base page
    size_t touched_count;
    size_t max_contiguous_run;
    enum thp_region_state state;
    size_t collapse_attempts;
    uint64_t collapse_ns;
    int collapse_result; // final madvise return value, or -1 for advice failure
    int collapse_errno; // zero on success; errno of the last failed operation
};

// Module-owned fields: callers may access base/length, but must not modify fields.
// The handler uses this mutex for every metadata access and descriptor transition.
struct thp_containar
{
    void * base;                 // start of the owned anonymous mapping, aligned to region_bytes
    size_t length;               // total mapping size in bytes: region_count * region_bytes
    size_t region_count;         // number of independently tracked huge-page regions
    size_t base_page_bytes;      // base-page size in bytes, checked against the host at creation
    size_t region_bytes;         // bytes per tracked region, initialised from THP_REGION_BYTES
    struct thp_region * regions; // owned metadata array with region_count entries, outside the mapping
    pthread_mutex_t mutex;       // protects metadata, handler attachment and descriptor transitions; also used by the handler queue
    int uffd;                    // owned userfaultfd for missing-page registration; -1 when closed
    int handler_attached;        // 1 while a handler owns attachment, including after stop; blocks destruction until detached
};

// [PUBLIC API] ==========================
// Allocate and register private anonymous memory; output must initially be NULL.
// region count is the number of THP_REGION_BYTES to allocate
// Return 0 on success, -1 with errno on failure.
// Start a handler before touching base, otherwise missing faults will block.
int thp_containar_create(struct thp_containar ** output, size_t region_count);
// Snapshot one region without racing the service threads.
int thp_containar_region(struct thp_containar * containar, size_t index,
                         struct thp_region * snapshot);
// Close userfaultfd to unregister all remaining ranges and release blocked faults.
// Handler-only while attached; caller must hold containar->mutex.
void thp_containar_release_registration(struct thp_containar * containar);
// Record a distinct page and update the longest run; caller must hold the mutex.
// Returns 1 for a new touch, 0 for a duplicate, -1 for an invalid index.
int thp_containar_touch(struct thp_containar * containar, size_t region_index,
                        size_t page_index);
// Destroy only after stopping application access and destroying its handler.
// Returns EBUSY if a handler is still attached; NULL is harmless.
int thp_containar_destroy(struct thp_containar * containar);

// Lifecycle calls are owner-thread operations, not concurrent with one another.
// Do not fork, unmap, remap, discard pages or change arena advice while active.
// UFFD_USER_MODE_ONLY requires userspace first touches before kernel I/O access.
# endif // THP_CONTAINAR_H
