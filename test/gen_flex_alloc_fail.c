/* Replace only allocations in the FLEX generator, not CLI/group allocations. */
#include <stdlib.h>

static void *fail_flex_malloc(size_t size)
{
    (void)size;
    return NULL;
}

#define malloc fail_flex_malloc
#include "../gen_flex.c"
