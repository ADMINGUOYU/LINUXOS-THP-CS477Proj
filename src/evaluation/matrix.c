// file: matrix.c
# include "evaluation.h"
# include <errno.h>
# include <stdlib.h>

// Each selected page is one row; the small input vector is outside the arena.
static int _prepare(struct evaluation_context * context);
static int _run(struct evaluation_context * context);
static void _release(struct evaluation_context * context);

// Populate all selected rows, including rows that the phase workload never reuses.
static int _prepare(struct evaluation_context * context)
{
    size_t columns = context->container->base_page_bytes / sizeof(double);
    size_t row;
    size_t column;
    double * values;
    double * vector = malloc(columns * sizeof(*vector));

    context->scratch = vector;
    if (vector == NULL)
        return -1;
    for (column = 0; column < columns; ++column)
        vector[column] = 1.0 / (double)(column + 1);
    for (row = 0; row < context->page_count; ++row)
    {
        values = (double * )evaluation_page(context, row);
        for (column = 0; column < columns; ++column)
            values[column] = (double)(column + 1);
    }
    return 0;
}

// Compute matrix-vector dot products; every row has the exact integer sum columns.
static int _run(struct evaluation_context * context)
{
    size_t columns = context->container->base_page_bytes / sizeof(double);
    size_t iteration;
    size_t row;
    size_t column;
    double sum;
    double * values;
    const double * vector = context->scratch;

    for (iteration = 0; iteration < context->options.iterations; ++iteration)
        for (row = 0; row < context->reuse_count; ++row)
        {
            values = (double * )evaluation_page(context, row);
            sum = 0.0;
            for (column = 0; column < columns; ++column)
                sum += values[column] * vector[column];
            if (sum < (double)columns - 0.01 || sum > (double)columns + 0.01)
            {
                errno = EILSEQ;
                return -1;
            }
            context->checksum += (uint64_t)sum;
            ++context->operations;
        }
    return 0;
}

// Release the vector even if initial population failed.
static void _release(struct evaluation_context * context)
{
    free(context->scratch);
}

// A standalone executable, launched once per sample by the shared driver.
int main(int argc, char * argv[])
{
    const struct evaluation_workload workload = { "matrix", _prepare, _run, _release };

    return evaluation_main(argc, argv, &workload);
}
