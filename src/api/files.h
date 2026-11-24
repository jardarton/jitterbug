/*
 * What the file loaders share (jbt.c, jbm.c): little-endian reads from
 * bytes, a section's bounds against the file, and reading a whole file
 * into memory from the context's allocator.
 */
#ifndef JBI_API_FILES_H
#define JBI_API_FILES_H

#include <stddef.h>
#include <stdint.h>

#include <jitterbug.h>

static inline unsigned jbi_le16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static inline uint32_t jbi_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A section of a file: at offset, bytes long. */
typedef struct jbi_section {
    const char *name;
    uint32_t offset;
    uint64_t bytes;
} jbi_section;

/*
 * Checks sections n against a file of size bytes with a header of header
 * bytes: each present one (bytes above 0) at an offset that is a multiple
 * of 4, at or after the header, and wholly within the file; and no two
 * overlapping. fn names the caller in the detail (jbi_api_fail).
 */
jb_result jbi_check_sections(jb_context *jb, const char *fn, const jbi_section *s, int n,
                             size_t size, size_t header);

/*
 * Reads the file at path into memory from the context's allocator: *data
 * (free it with jbi_free(&jb->alloc, ...)) and *size. Refuses a path that
 * cannot be opened or read and a file larger than JBI_FILE_MAX with
 * JB_ERROR_INVALID_ARGUMENT, and fails for no memory with
 * JB_ERROR_OUT_OF_MEMORY, both through jbi_api_fail with fn; *data is NULL
 * then.
 */
jb_result jbi_read_file(jb_context *jb, const char *fn, const char *path, unsigned char **data,
                        size_t *size);

#endif
