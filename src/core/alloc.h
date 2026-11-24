/*
 * Where a context's memory comes from: the allocator callbacks of the
 * public jb_config (include/jitterbug.h), or the C library's when none are
 * given. Every allocation the library makes itself goes through here (the
 * context, both VRAMs, the primitive queue, the worker pool, the public
 * API's buffers); the thread library's own memory, such as thread stacks,
 * does not.
 */
#ifndef JBI_CORE_ALLOC_H
#define JBI_CORE_ALLOC_H

#include <stddef.h>

typedef struct jbi_allocator {
    /* Both set, or alloc NULL for calloc/free. alloc returns memory
       aligned for any object, or NULL; free is never given NULL. */
    void *(*alloc)(void *user, size_t size);
    void (*free)(void *user, void *ptr);
    void *user;
} jbi_allocator;

/* count * size zeroed bytes (count, size > 0), or NULL without memory or
   when the product overflows. a NULL means calloc. */
void *jbi_calloc(const jbi_allocator *a, size_t count, size_t size);

/* Frees what jbi_calloc returned with the same allocator. NULL is ignored. */
void jbi_free(const jbi_allocator *a, void *ptr);

#endif
