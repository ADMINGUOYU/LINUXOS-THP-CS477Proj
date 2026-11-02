// file: thp_containar.c
# define _GNU_SOURCE
# include "thp_containar.h"

// Linux UAPI and libraries (C99)
# include <errno.h>
# include <fcntl.h>
# include <linux/userfaultfd.h>
# include <stdlib.h>
# include <sys/ioctl.h>
# include <sys/mman.h>
# include <sys/syscall.h>
# include <unistd.h>

// Older headers may not define this stable Linux UAPI flag.
# ifndef UFFD_USER_MODE_ONLY
# define UFFD_USER_MODE_ONLY 1
# endif

// [INTERNAL API] (forward declarations) ==
static int _allocate_mapping(struct thp_containar * containar);
static int _register_mapping(struct thp_containar * containar);
static int _page_touched(const struct thp_region * region, size_t page_index);

// Over-allocate and trim both ends to own an exactly aligned anonymous mapping.
static int _allocate_mapping(struct thp_containar * containar)
{
    size_t total = containar->length + containar->region_bytes;
    void * mapping;
    uintptr_t aligned;
    size_t prefix;
    size_t suffix;
    int failure;

    mapping = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        return -1;
    // First touches must remain 4 KiB until the worker selects a region.
    if (madvise(mapping, total, MADV_NOHUGEPAGE) != 0)
    {
        failure = errno;
        munmap(mapping, total);
        errno = failure;
        return -1;
    }
    aligned = ((uintptr_t)mapping + containar->region_bytes - 1) &
              ~(uintptr_t)(containar->region_bytes - 1);
    prefix = aligned - (uintptr_t)mapping;
    suffix = total - prefix - containar->length;
    if (prefix != 0 && munmap(mapping, prefix) != 0)
    {
        failure = errno;
        munmap(mapping, total);
        errno = failure;
        return -1;
    }
    if (suffix != 0 && munmap((void * )(aligned + containar->length), suffix) != 0)
    {
        failure = errno;
        munmap((void * )aligned, containar->length + suffix);
        errno = failure;
        return -1;
    }
    containar->base = (void * )aligned;
    return 0;
}

// Negotiate missing-fault support and register only the aligned data mapping.
static int _register_mapping(struct thp_containar * containar)
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
    containar->uffd = (int)syscall(SYS_userfaultfd,
                                  O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
# else
    errno = ENOSYS;
# endif
    if (containar->uffd < 0)
        return -1;
    if (ioctl(containar->uffd, UFFDIO_API, &api) != 0)
        return -1;
    if (!(api.ioctls & (UINT64_C(1) << _UFFDIO_REGISTER)) ||
        !(api.ioctls & (UINT64_C(1) << _UFFDIO_UNREGISTER)))
    {
        errno = ENOTSUP;
        return -1;
    }
    registration.range.start = (uintptr_t)containar->base;
    registration.range.len = containar->length;
    registration.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(containar->uffd, UFFDIO_REGISTER, &registration) != 0)
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
int thp_containar_create(struct thp_containar ** output, size_t region_count)
{
    struct thp_containar * containar;
    int failure;

    size_t region_bytes = THP_REGION_BYTES;
    size_t base_page_bytes = THP_BASE_PAGE_BYTES;

    // Alignment masks require power-of-two geometry and whole base-page regions.
    if (base_page_bytes == 0 || region_bytes == 0 ||
        (base_page_bytes & (base_page_bytes - 1)) != 0 ||
        (region_bytes & (region_bytes - 1)) != 0 ||
        region_bytes < base_page_bytes || region_bytes % base_page_bytes != 0)
    {
        errno = EINVAL;
        return -1;
    }
    // Validate ownership and allocation sizes before creating the mapping.
    if (output == NULL || *output != NULL || region_count == 0 ||
        region_count > (SIZE_MAX - region_bytes) / region_bytes ||
        region_count > SIZE_MAX / sizeof(struct thp_region))
    {
        errno = EINVAL;
        return -1;
    }
    if (sysconf(_SC_PAGESIZE) != (long)base_page_bytes)
    {
        errno = ENOTSUP;
        return -1;
    }
    containar = calloc(1, sizeof(*containar));
    if (containar == NULL)
        return -1;
    containar->uffd = -1;
    containar->length = region_count * region_bytes;
    containar->region_count = region_count;
    containar->base_page_bytes = base_page_bytes;
    containar->region_bytes = region_bytes;
    failure = pthread_mutex_init(&containar->mutex, NULL);
    if (failure != 0)
    {
        free(containar);
        errno = failure;
        return -1;
    }
    // calloc places metadata outside the mmap range registered with userfaultfd.
    containar->regions = calloc(region_count, sizeof(*containar->regions));
    if (containar->regions == NULL || _allocate_mapping(containar) != 0 ||
        _register_mapping(containar) != 0)
    {
        failure = errno;
        thp_containar_destroy(containar);
        errno = failure;
        return -1;
    }
    *output = containar;
    return 0;
}

// Update only the run joined by a new first touch; preserve distinct-page counts.
int thp_containar_touch(struct thp_containar * containar, size_t region_index,
                        size_t page_index)
{
    struct thp_region * region;
    size_t left = page_index;
    size_t right = page_index;

    if (containar == NULL || region_index >= containar->region_count ||
        page_index >= THP_PAGES_PER_REGION)
    {
        errno = EINVAL;
        return -1;
    }
    region = &containar->regions[region_index];
    if (_page_touched(region, page_index))
        return 0;
    region->bitmap[page_index / THP_BITMAP_WORD_BITS] |=
        UINT64_C(1) << (page_index % THP_BITMAP_WORD_BITS);
    ++region->touched_count;
    // Runs may cross multiple bitmap words, including the 63/64 boundary.
    while (left > 0 && _page_touched(region, left - 1))
        --left;
    while (right + 1 < THP_PAGES_PER_REGION && _page_touched(region, right + 1))
        ++right;
    if (right - left + 1 > region->max_contiguous_run)
        region->max_contiguous_run = right - left + 1;
    return 1;
}

// Return a consistent copy while service threads may still be updating the region.
int thp_containar_region(struct thp_containar * containar, size_t index,
                         struct thp_region * snapshot)
{
    if (containar == NULL || snapshot == NULL || index >= containar->region_count)
    {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&containar->mutex);
    *snapshot = containar->regions[index];
    pthread_mutex_unlock(&containar->mutex);
    return 0;
}

// Closing the last uffd reference wakes waiters and unregisters remaining ranges.
void thp_containar_release_registration(struct thp_containar * containar)
{
    size_t index;

    if (containar->uffd >= 0)
    {
        close(containar->uffd);
        containar->uffd = -1;
    }
    if (containar->regions != NULL)
        for (index = 0; index < containar->region_count; ++index)
            if (containar->regions[index].state == THP_TRACKING ||
                containar->regions[index].state == THP_UNREGISTERING)
                containar->regions[index].state = THP_TRACKING_STOPPED;
}

// Never unmap memory while its handler or application threads can still use it.
int thp_containar_destroy(struct thp_containar * containar)
{
    if (containar == NULL)
        return 0;
    pthread_mutex_lock(&containar->mutex);
    if (containar->handler_attached)
    {
        pthread_mutex_unlock(&containar->mutex);
        errno = EBUSY;
        return -1;
    }
    thp_containar_release_registration(containar);
    pthread_mutex_unlock(&containar->mutex);
    if (containar->base != NULL && munmap(containar->base, containar->length) != 0)
        return -1;
    free(containar->regions);
    pthread_mutex_destroy(&containar->mutex);
    free(containar);
    return 0;
}
