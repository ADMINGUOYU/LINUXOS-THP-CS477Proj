// file: chase.c
# include "evaluation.h"
# include <errno.h>

// Nodes contain logical indices; a load determines the next page to access.
static int _prepare(struct evaluation_context * context);
static int _run(struct evaluation_context * context);
static void _release(struct evaluation_context * context);

// Build a cycle; phase populates every page but keeps the hot cycle in a tiny prefix.
static int _prepare(struct evaluation_context * context)
{
    size_t index;
    size_t * next;

    for (index = 0; index < context->page_count; ++index)
    {
        next = (size_t * )evaluation_page(context, index);
        *next = (index + 1) % context->reuse_count;
    }
    return 0;
}

// The dependent load chain exposes translation latency with little arithmetic.
static int _run(struct evaluation_context * context)
{
    size_t iteration;
    size_t step;
    size_t index = 0;

    for (iteration = 0; iteration < context->options.iterations; ++iteration)
        for (step = 0; step < context->reuse_count; ++step)
        {
            index = *(const size_t * )evaluation_page(context, index);
            if (index >= context->reuse_count)
            {
                errno = EILSEQ;
                return -1;
            }
            context->checksum += index;
            ++context->operations;
        }
    if (index != 0)
    {
        errno = EILSEQ;
        return -1;
    }
    return 0;
}

// The container owns every node; no workload-specific allocation is needed.
static void _release(struct evaluation_context * context)
{
    (void)context;
}

// A standalone executable, launched once per sample by the shared driver.
int main(int argc, char * argv[])
{
    const struct evaluation_workload workload = { "chase", _prepare, _run, _release };

    return evaluation_main(argc, argv, &workload);
}
