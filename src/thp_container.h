// file: thp_container.h
# ifndef THP_CONTAINER_H
# define THP_CONTAINER_H

# include <stddef.h>
# include <stdint.h>
# include <limits.h>
# include <pthread.h>

// [CONFIGURATION] =======================
// Set these globals from sys_config before creating containers or starting threads.
// Do not modify configuration until all containers and handlers are destroyed.
extern size_t THP_BASE_PAGE_BYTES;
extern size_t THP_REGION_BYTES;
# define THP_PAGES_PER_REGION (THP_REGION_BYTES / THP_BASE_PAGE_BYTES)
// Derived geometry stays as expressions, not independently editable settings.
// Default bitmap geometry: eight uint64_t words for 512 base pages.
# define THP_BITMAP_WORD_BITS (sizeof(uint64_t) * CHAR_BIT)
# define THP_BITMAP_WORD_COUNT (THP_PAGES_PER_REGION / THP_BITMAP_WORD_BITS + (THP_PAGES_PER_REGION % THP_BITMAP_WORD_BITS != 0))
// [CONFIGURATION] ENDs ==================

// [METADATA] ============================
// Per-mapping policy;
// NOTE: selecting a mode does not change system THP settings
// TODO: we now plan to tackle modes in the driving script
enum thp_policy_mode
{
    THP_POLICY_DISABLED,   // ordinary base-page faults, MADV_NOHUGEPAGE throughout
    THP_POLICY_ALWAYS,     // MADV_HUGEPAGE before first touch, ordinary kernel faults
    THP_POLICY_KHUGEPAGED, // base-page population, then explicitly enable MADV_HUGEPAGE (afterward promotion)
    THP_POLICY_SELECTIVE   // userfaultfd tracking and handler-driven promotion (the promoted mode)
};

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

// Selective-only metadata, stored outside the registered mapping.
// Counts stop when registration is removed.
struct thp_region
{
    uint64_t * bitmap;        // owned array with bitmap_word_count words; 1 bit per base page
    size_t bitmap_word_count; // allocated word count, also carried in snapshots
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
struct thp_container
{
    void * base;                 // start of the owned anonymous mapping, aligned to region_bytes
    size_t length;               // total mapping size in bytes: region_count * region_bytes
    size_t region_count;         // number of huge-page-sized regions in the mapping
    size_t base_page_bytes;      // base-page size in bytes, checked against the host at creation
    size_t region_bytes;         // bytes per tracked region, initialised from THP_REGION_BYTES
    enum thp_policy_mode policy; // fixed at creation; governs mapping advice and registration
    struct thp_region * regions; // selective-only (our policy) owned metadata outside the mapping; NULL for other policies
    pthread_mutex_t mutex;       // protects metadata, handler attachment and descriptor transitions; also used by the handler queue
    int uffd;                    // selective-only (our policy) userfaultfd; -1 when closed or using another policy
    int handler_attached;        // 1 while a handler owns attachment, including after stop; blocks destruction until detached
};

// [PUBLIC API] ==========================
// Allocate private anonymous memory; register only for THP_POLICY_SELECTIVE.
// Output must initially be NULL (i.e. struct thp_container * container = NULL;) used to receive the new pointer on success
// Geometry comes from the configuration globals.
// region count is the number of THP_REGION_BYTES to allocate
// Return 0 on success, -1 with errno on failure.
// In selective mode, start a handler before touching base or missing faults block.
// Other modes use ordinary kernel faults and need no handler.
// MADV_HUGEPAGE requests eligibility; allocation still depends on system settings.
int thp_container_create(struct thp_container ** output, size_t region_count,
                         enum thp_policy_mode policy);
// After completing base-page population, enable THP_POLICY_KHUGEPAGED scanning.
// Owner-thread operation: finish/join population before calling. Repeats are harmless.
// Returns 0 on successful advice, -1/errno on failure; other policies give EINVAL.
// Does not wait for promotion or invoke MADV_COLLAPSE. Populate all pages first:
// untouched pages become eligible for huge-page faults after this advice change.
int thp_container_enable_khugepaged(struct thp_container * container);
// Snapshot one region without racing the service threads.
// Pass a zero-initialised snapshot with bitmap == NULL; success allocates its bitmap.
// Caller must call thp_container_region_destroy after use. Failure leaves the snapshot unchanged.
// Tracking snapshots are selective-only; other policies return -1/ENOTSUP.
int thp_container_region(struct thp_container * container, size_t index,
                         struct thp_region * snapshot);
// Free an owned bitmap and reset the region; does not free the region struct itself.
// Accepts NULL or a zero-initialised/already destroyed region; repeated calls are safe.
// For caller-owned snapshots only; live container metadata is cleaned up by container destroy.
void thp_container_region_destroy(struct thp_region * region);
// Close userfaultfd to unregister all remaining ranges and release blocked faults.
// Handler-only while attached; caller must hold container->mutex.
void thp_container_release_registration(struct thp_container * container);
// Record a distinct page and update the longest run; caller must hold the mutex.
// Returns 1 for a new touch, 0 for a duplicate, -1 for an invalid index.
// Other policies return -1/ENOTSUP because they have no tracking metadata.
int thp_container_touch(struct thp_container * container, size_t region_index,
                        size_t page_index);
// Destroy only after stopping application access and destroying its handler.
// Returns EBUSY if a handler is still attached; NULL is harmless.
int thp_container_destroy(struct thp_container * container);

// Lifecycle calls are owner-thread operations, not concurrent with one another.
// Do not fork, unmap, remap, discard pages or change arena advice while active,
// except through the khugepaged helper at the population boundary.
// Selective mode: UFFD_USER_MODE_ONLY requires userspace touches before kernel I/O.
# endif // THP_CONTAINER_H
