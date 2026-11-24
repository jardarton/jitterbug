/*
 * The demo's files, in the program (examples/demo/README.md, "How the
 * assets are made"): every .jbt and .jbm of assets/conversions.txt, as
 * bytes aligned to 4, made into demo_files.c by assets/embed.cmake at
 * build time.
 */
#ifndef DEMO_FILES_H
#define DEMO_FILES_H

#include <stddef.h>
#include <stdint.h>

typedef struct dm_file {
    const char *name; /* as conversions.txt names it: "water.jbt" */
    const unsigned char *data;
    size_t size;
} dm_file;

extern const dm_file dm_files[];
extern const int dm_file_count;

#endif
