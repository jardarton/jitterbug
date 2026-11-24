/* Allocation through a context's allocator (alloc.h). */
#include "core/alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *jbi_calloc(const jbi_allocator *a, size_t count, size_t size)
{
    void *p;

    if (count != 0 && size > SIZE_MAX / count)
        return NULL;
    if (a == NULL || a->alloc == NULL)
        return calloc(count, size);
    p = (a->alloc)(a->user, count * size);
    if (p != NULL)
        memset(p, 0, count * size);
    return p;
}

void jbi_free(const jbi_allocator *a, void *ptr)
{
    if (ptr == NULL)
        return;
    if (a == NULL || a->alloc == NULL)
        free(ptr);
    else
        (a->free)(a->user, ptr);
}
