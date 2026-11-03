// file: hash.c
# include "evaluation.h"
# include <errno.h>

// Page-local open addressing permits genuinely sparse physical table placement.
struct hash_entry
{
    size_t key; // zero denotes an empty slot
    size_t value; // deterministic payload checked on every lookup
};

static int _prepare(struct evaluation_context * context);
static int _run(struct evaluation_context * context);
static void _release(struct evaluation_context * context);

// Fill each page-local table to 50% occupancy, leaving probe termination slots.
static int _prepare(struct evaluation_context * context)
{
    size_t slots = context->container->base_page_bytes / sizeof(struct hash_entry);
    size_t page;
    size_t key;
    size_t slot;
    struct hash_entry * entries;

    if (slots < 2)
    {
        errno = EINVAL;
        return -1;
    }
    for (page = 0; page < context->page_count; ++page)
    {
        entries = (struct hash_entry * )evaluation_page(context, page);
        for (key = 1; key <= slots / 2; ++key)
        {
            slot = (key * (size_t)2654435761U) % slots;
            while (entries[slot].key != 0)
                slot = (slot + 1) % slots;
            entries[slot].key = key;
            entries[slot].value = key ^ context->pages[page];
        }
    }
    return 0;
}

// Query random keys across the selected tables, retaining all payload checks.
static int _run(struct evaluation_context * context)
{
    size_t slots = context->container->base_page_bytes / sizeof(struct hash_entry);
    size_t iteration;
    size_t page;
    size_t key;
    size_t slot;
    size_t probe;
    struct hash_entry * entries;

    for (iteration = 0; iteration < context->options.iterations; ++iteration)
        for (page = 0; page < context->reuse_count; ++page)
        {
            key = 1 + (size_t)(evaluation_random(&context->random_state) % (slots / 2));
            entries = (struct hash_entry * )evaluation_page(context, page);
            slot = (key * (size_t)2654435761U) % slots;
            for (probe = 0; probe < slots && entries[slot].key != key; ++probe)
                slot = (slot + 1) % slots;
            if (probe == slots || entries[slot].value != (key ^ context->pages[page]))
            {
                errno = EILSEQ;
                return -1;
            }
            context->checksum += entries[slot].value;
            ++context->operations;
        }
    return 0;
}

// The table entries live entirely in the container mapping.
static void _release(struct evaluation_context * context)
{
    (void)context;
}

// A standalone executable, launched once per sample by the shared driver.
int main(int argc, char * argv[])
{
    const struct evaluation_workload workload = { "hash", _prepare, _run, _release };

    return evaluation_main(argc, argv, &workload);
}
