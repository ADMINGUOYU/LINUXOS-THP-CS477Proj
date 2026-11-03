// file: thp_container.c
# define _GNU_SOURCE
# include "thp_container.h"

// Linux UAPI and libraries (C99)
# include <errno.h>
# include <fcntl.h>
# include <linux/userfaultfd.h>
# include <stdlib.h>
# include <string.h>
# include <sys/ioctl.h>
# include <sys/mman.h>
# include <sys/syscall.h>
# include <unistd.h>

// Older headers may not define this stable Linux UAPI flag.
# ifndef UFFD_USER_MODE_ONLY
# define UFFD_USER_MODE_ONLY 1
# endif

// [CONFIGURATION] =======================
size_t THP_BASE_PAGE_BYTES = 4096UL;
size_t THP_REGION_BYTES = 2097152UL;
// [CONFIGURATION] ENDs ==================

// [INTERNAL API] (forward declarations) ==
static int _allocate_mapping(struct thp_container * container);
static int _register_mapping(struct thp_container * container);
static int _page_touched(const struct thp_region * region, size_t page_index);

// Over-allocate and trim both ends to own an exactly aligned anonymous mapping.
static int _allocate_mapping(struct thp_container * container)
{
    size_t total = container->length + container->region_bytes;
    void * mapping;
    uintptr_t aligned;
    size_t prefix;
    size_t suffix;
    int memory_advice = container->policy == THP_POLICY_ALWAYS ? MADV_HUGEPAGE : MADV_NOHUGEPAGE;
    int failure;

    mapping = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        return -1;
    // First touches remain base pages unless the policy requests THP immediately.
    // Selective mode retains NOHUGEPAGE until the worker selects a region.
    if (madvise(mapping, total, memory_advice) != 0)
    {
        failure = errno;
        munmap(mapping, total);
        errno = failure;
        return -1;
    }
    aligned = ((uintptr_t)mapping + container->region_bytes - 1) &
              ~(uintptr_t)(container->region_bytes - 1);
    prefix = aligned - (uintptr_t)mapping;
    suffix = total - prefix - container->length;
    if (prefix != 0 && munmap(mapping, prefix) != 0)
    {
        failure = errno;
        munmap(mapping, total);
        errno = failure;
        return -1;
    }
    if (suffix != 0 && munmap((void * )(aligned + container->length), suffix) != 0)
    {
        failure = errno;
        munmap((void * )aligned, container->length + suffix);
        errno = failure;
        return -1;
    }
    container->base = (void * )aligned;
    return 0;
}

// Negotiate missing-fault support and register only the aligned data mapping.
static int _register_mapping(struct thp_container * container)
{
    struct uffdio_api api =
    {
        .api = UFFD_API
    };
    struct uffdio_register registration =
    {
        0
    };

    // Do not silently elevate privileges or fall back to a different fault mode.
# ifdef SYS_userfaultfd
    container->uffd = (int)syscall(SYS_userfaultfd,
                                  O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
# else
    errno = ENOSYS;
# endif
    if (container->uffd < 0)
        return -1;
    if (ioctl(container->uffd, UFFDIO_API, &api) != 0)
        return -1;
    if (!(api.ioctls & (UINT64_C(1) << _UFFDIO_REGISTER)) ||
        !(api.ioctls & (UINT64_C(1) << _UFFDIO_UNREGISTER)))
    {
        errno = ENOTSUP;
        return -1;
    }
    registration.range.start = (uintptr_t)container->base;
    registration.range.len = container->length;
    registration.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(container->uffd, UFFDIO_REGISTER, &registration) != 0)
        return -1;
    if (!(registration.ioctls & (UINT64_C(1) << _UFFDIO_ZEROPAGE)) ||
        !(registration.ioctls & (UINT64_C(1) << _UFFDIO_WAKE)))
    {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

// Test a bit without crossing a word boundary or shifting by 64 bits.
static int _page_touched(const struct thp_region * region, size_t page_index)
{
    return (region->bitmap[page_index / THP_BITMAP_WORD_BITS] &
            (UINT64_C(1) << (page_index % THP_BITMAP_WORD_BITS))) != 0;
}

// [PUBLIC API] ==========================
// Construct mapping and external metadata, publishing ownership only on success.
int thp_container_create(struct thp_container ** output, size_t region_count,
                         enum thp_policy_mode policy)
{
    struct thp_container * container;
    int failure;

    size_t region_bytes = THP_REGION_BYTES;
    size_t base_page_bytes = THP_BASE_PAGE_BYTES;
    size_t index;
    size_t bitmap_words;

    // Alignment masks require power-of-two geometry and whole base-page regions.
    if (base_page_bytes == 0 || region_bytes == 0 ||
        (base_page_bytes & (base_page_bytes - 1)) != 0 ||
        (region_bytes & (region_bytes - 1)) != 0 ||
        region_bytes < base_page_bytes || region_bytes % base_page_bytes != 0)
    {
        errno = EINVAL;
        return -1;
    }
    bitmap_words = THP_BITMAP_WORD_COUNT;
    if (bitmap_words > SIZE_MAX / sizeof(uint64_t))
    {
        errno = EOVERFLOW;
        return -1;
    }
    // Reject unknown modes before acquiring any resources.
    if (policy != THP_POLICY_DISABLED && policy != THP_POLICY_ALWAYS &&
        policy != THP_POLICY_KHUGEPAGED && policy != THP_POLICY_SELECTIVE)
    {
        errno = EINVAL;
        return -1;
    }
    // Validate ownership and allocation sizes before creating the mapping.
    if (output == NULL || *output != NULL || region_count == 0 ||
        region_count > (SIZE_MAX - region_bytes) / region_bytes ||
        (policy == THP_POLICY_SELECTIVE && region_count > SIZE_MAX / sizeof(struct thp_region)))
    {
        errno = EINVAL;
        return -1;
    }
    if (sysconf(_SC_PAGESIZE) != (long)base_page_bytes)
    {
        errno = ENOTSUP;
        return -1;
    }
    container = calloc(1, sizeof(*container));
    if (container == NULL)
        return -1;
    container->uffd = -1;
    container->policy = policy;
    container->length = region_count * region_bytes;
    container->region_count = region_count;
    container->base_page_bytes = base_page_bytes;
    container->region_bytes = region_bytes;
    failure = pthread_mutex_init(&container->mutex, NULL);
    if (failure != 0)
    {
        free(container);
        errno = failure;
        return -1;
    }
    // Only selective mode needs external metadata and userfaultfd registration.
    // calloc places metadata outside the mmap range registered with userfaultfd.
    if (policy == THP_POLICY_SELECTIVE)
        container->regions = calloc(region_count, sizeof(*container->regions));
    // Allocate every bitmap separately from the registered application mapping.
    failure = policy == THP_POLICY_SELECTIVE && container->regions == NULL ? ENOMEM : 0;
    for (index = 0; failure == 0 && policy == THP_POLICY_SELECTIVE && index < region_count; ++index)
    {
        container->regions[index].bitmap_word_count = bitmap_words;
        container->regions[index].bitmap = calloc(bitmap_words, sizeof(uint64_t));
        if (container->regions[index].bitmap == NULL)
            failure = ENOMEM;
    }
    if (failure != 0)
    {
        thp_container_destroy(container);
        errno = failure;
        return -1;
    }
    if (_allocate_mapping(container) != 0 ||
        (policy == THP_POLICY_SELECTIVE && _register_mapping(container) != 0))
    {
        failure = errno;
        thp_container_destroy(container);
        errno = failure;
        return -1;
    }
    *output = container;
    return 0;
}

// Make the populated mapping eligible for asynchronous kernel promotion.
int thp_container_enable_khugepaged(struct thp_container * container)
{
    // Keep this phase transition exclusive to the delayed kernel policy.
    if (container == NULL || container->policy != THP_POLICY_KHUGEPAGED)
    {
        errno = EINVAL;
        return -1;
    }
    // No registration, worker or explicit collapse is involved in this policy.
    return madvise(container->base, container->length, MADV_HUGEPAGE);
}

// Update only the run joined by a new first touch; preserve distinct-page counts.
int thp_container_touch(struct thp_container * container, size_t region_index,
                        size_t page_index)
{
    struct thp_region * region;
    size_t left = page_index;
    size_t right = page_index;

    if (container == NULL || region_index >= container->region_count ||
        page_index >= container->region_bytes / container->base_page_bytes)
    {
        errno = EINVAL;
        return -1;
    }
    // Ordinary kernel faults are not tracked by this module.
    if (container->policy != THP_POLICY_SELECTIVE)
    {
        errno = ENOTSUP;
        return -1;
    }
    region = &container->regions[region_index];
    if (_page_touched(region, page_index))
        return 0;
    region->bitmap[page_index / THP_BITMAP_WORD_BITS] |=
        UINT64_C(1) << (page_index % THP_BITMAP_WORD_BITS);
    ++region->touched_count;
    // Runs may cross multiple bitmap words, including the 63/64 boundary.
    while (left > 0 && _page_touched(region, left - 1))
        --left;
    while (right + 1 < container->region_bytes / container->base_page_bytes && _page_touched(region, right + 1))
        ++right;
    if (right - left + 1 > region->max_contiguous_run)
        region->max_contiguous_run = right - left + 1;
    return 1;
}

// Return a consistent copy while service threads may still be updating the region.
int thp_container_region(struct thp_container * container, size_t index,
                         struct thp_region * snapshot)
{
    uint64_t * bitmap;
    size_t bitmap_bytes;

    if (container == NULL || snapshot == NULL || snapshot->bitmap != NULL || index >= container->region_count)
    {
        errno = EINVAL;
        return -1;
    }
    // Do not present absent measurements as zero-valued tracking results.
    if (container->policy != THP_POLICY_SELECTIVE)
    {
        errno = ENOTSUP;
        return -1;
    }
    pthread_mutex_lock(&container->mutex);
    // Copy the bitmap while locked so the snapshot never aliases live metadata.
    bitmap_bytes = container->regions[index].bitmap_word_count * sizeof(*bitmap);
    bitmap = malloc(bitmap_bytes);
    if (bitmap == NULL)
    {
        pthread_mutex_unlock(&container->mutex);
        errno = ENOMEM;
        return -1;
    }
    memcpy(bitmap, container->regions[index].bitmap, bitmap_bytes);
    *snapshot = container->regions[index];
    snapshot->bitmap = bitmap;
    pthread_mutex_unlock(&container->mutex);
    return 0;
}

// Release region-owned storage, leaving the struct ready for another snapshot.
void thp_container_region_destroy(struct thp_region * region)
{
    if (region == NULL)
        return;
    // Clear the pointer and metadata together so repeated cleanup is harmless.
    free(region->bitmap);
    *region = (struct thp_region){ 0 };
}

// Closing the last uffd reference wakes waiters and unregisters remaining ranges.
void thp_container_release_registration(struct thp_container * container)
{
    size_t index;

    if (container->uffd >= 0)
    {
        close(container->uffd);
        container->uffd = -1;
    }
    if (container->regions != NULL)
        for (index = 0; index < container->region_count; ++index)
            if (container->regions[index].state == THP_TRACKING ||
                container->regions[index].state == THP_UNREGISTERING)
                container->regions[index].state = THP_TRACKING_STOPPED;
}

// Never unmap memory while its handler or application threads can still use it.
int thp_container_destroy(struct thp_container * container)
{
    size_t index;

    if (container == NULL)
        return 0;
    pthread_mutex_lock(&container->mutex);
    if (container->handler_attached)
    {
        pthread_mutex_unlock(&container->mutex);
        errno = EBUSY;
        return -1;
    }
    thp_container_release_registration(container);
    pthread_mutex_unlock(&container->mutex);
    if (container->base != NULL && munmap(container->base, container->length) != 0)
        return -1;
    // Release external bitmaps, including partially constructed containers.
    if (container->regions != NULL)
        for (index = 0; index < container->region_count; ++index)
            thp_container_region_destroy(&container->regions[index]);
    free(container->regions);
    pthread_mutex_destroy(&container->mutex);
    free(container);
    return 0;
}
