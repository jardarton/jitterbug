/*
 * The loaders' shared parts (files.h).
 */
#include <stdio.h>
#include <string.h>

#include "api/api.h"
#include "api/files.h"
#include "api/formats.h"
#include "core/alloc.h"

jb_result jbi_check_sections(jb_context *jb, const char *fn, const jbi_section *s, int n,
                             size_t size, size_t header)
{
    int i, k;

    for (i = 0; i < n; i++) {
        if (s[i].bytes == 0)
            continue;
        if (s[i].offset % 4 != 0)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: the %s are at offset %lu, not a multiple of 4", fn,
                                s[i].name, (unsigned long)s[i].offset);
        if (s[i].offset < header)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: the %s are at offset %lu, inside the %lu-byte header", fn,
                                s[i].name, (unsigned long)s[i].offset, (unsigned long)header);
        if (s[i].bytes > (uint64_t)size || s[i].offset > (uint64_t)size - s[i].bytes)
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: the %s (%llu bytes at offset %lu) run past the end of the "
                                "%lu-byte file",
                                fn, s[i].name, (unsigned long long)s[i].bytes,
                                (unsigned long)s[i].offset, (unsigned long)size);
    }
    for (i = 0; i < n; i++)
        for (k = i + 1; k < n; k++) {
            uint64_t a0 = s[i].offset, a1 = a0 + s[i].bytes;
            uint64_t b0 = s[k].offset, b1 = b0 + s[k].bytes;

            if (s[i].bytes > 0 && s[k].bytes > 0 && a0 < b1 && b0 < a1)
                return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                    "%s: the %s and the %s overlap", fn, s[i].name, s[k].name);
        }
    return JB_OK;
}

jb_result jbi_read_file(jb_context *jb, const char *fn, const char *path, unsigned char **data,
                        size_t *size)
{
    unsigned char *buf;
    size_t n = 0, cap = 0;
    FILE *f;

    *data = NULL;
    *size = 0;
    f = fopen(path, "rb");
    if (f == NULL)
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: cannot open \"%s\"", fn, path);
    /* The size from seeking, as a first guess: the read below decides. */
    if (fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end > 0 && (unsigned long)end > (unsigned long)JBI_FILE_MAX) {
            /* Past the limit, or not a file (a directory on Linux seeks to
               LONG_MAX): look at the byte after the limit before taking
               any memory. A byte there is too large; an error, as a
               directory's read gives, cannot be read; no byte at all, a
               size that was wrong, is read as a stream of unknown size. */
            int c = fseek(f, (long)JBI_FILE_MAX, SEEK_SET) == 0 ? fgetc(f) : EOF;
            if (c != EOF) {
                fclose(f);
                return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                    "%s: \"%s\" is larger than %lu bytes", fn, path,
                                    (unsigned long)JBI_FILE_MAX);
            }
            if (ferror(f)) {
                fclose(f);
                return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: cannot read \"%s\"",
                                    fn, path);
            }
            end = 0;
        }
        if (end > 0)
            cap = (size_t)end;
        if (fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: cannot read \"%s\"", fn,
                                path);
        }
    }
    /* The first byte before any memory: a path that opens but cannot be
       read (a directory, whatever size it seeks to) takes none. */
    {
        int c = fgetc(f);
        if (c == EOF && ferror(f)) {
            fclose(f);
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: cannot read \"%s\"", fn,
                                path);
        }
        if (c != EOF && ungetc(c, f) == EOF) {
            fclose(f);
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: cannot read \"%s\"", fn,
                                path);
        }
    }
    /* One byte more than the file, to see its end in the same read. */
    buf = jbi_calloc(&jb->alloc, cap + 1, 1);
    if (buf == NULL) {
        fclose(f);
        return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY, "%s: no memory to read \"%s\" (%lu bytes)",
                            fn, path, (unsigned long)cap);
    }
    for (;;) {
        size_t got = fread(buf + n, 1, cap + 1 - n, f);
        n += got;
        if (n < cap + 1)
            break; /* the end, or an error (below) */
        /* Longer than it seemed (a growing file, or one that cannot seek):
           more room, up to the limit. */
        if (cap >= JBI_FILE_MAX) {
            fclose(f);
            jbi_free(&jb->alloc, buf);
            return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT,
                                "%s: \"%s\" is larger than %lu bytes", fn, path,
                                (unsigned long)JBI_FILE_MAX);
        }
        {
            size_t more = cap < 65536 ? 65536 : cap;
            unsigned char *bigger;
            more = cap + more < JBI_FILE_MAX ? cap + more : JBI_FILE_MAX;
            bigger = jbi_calloc(&jb->alloc, more + 1, 1);
            if (bigger == NULL) {
                fclose(f);
                jbi_free(&jb->alloc, buf);
                return jbi_api_fail(jb, JB_ERROR_OUT_OF_MEMORY,
                                    "%s: no memory to read \"%s\"", fn, path);
            }
            memcpy(bigger, buf, n);
            jbi_free(&jb->alloc, buf);
            buf = bigger;
            cap = more;
        }
    }
    if (ferror(f)) {
        fclose(f);
        jbi_free(&jb->alloc, buf);
        return jbi_api_fail(jb, JB_ERROR_INVALID_ARGUMENT, "%s: cannot read \"%s\"", fn, path);
    }
    fclose(f);
    *data = buf;
    *size = n;
    return JB_OK;
}
