/*
 * jbc-fnv FILE: the file's size in bytes and its FNV-1a, 64 bits, over its
 * bytes (offset basis CBF29CE484222325h, prime 100000001B3h), as
 * "BYTES HASH" with the hash in 16 lowercase hex digits. record.sh writes
 * each list's and reference's into OUT/fixtures-identity.txt, the columns
 * of tests/conformance/fixtures.txt the flake check compares
 * (tests/conformance/FORMAT.md, "Fixture tables").
 */
#include <stdint.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char buf[65536];
    uint64_t h = 0xcbf29ce484222325u;
    unsigned long long bytes = 0;
    size_t got, k;

    if (argc != 2) {
        fprintf(stderr, "usage: jbc-fnv FILE\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "jbc-fnv: cannot open %s\n", argv[1]);
        return 2;
    }
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        for (k = 0; k < got; ++k) {
            h ^= buf[k];
            h *= 0x100000001b3u;
        }
        bytes += got;
    }
    if (ferror(f)) {
        fprintf(stderr, "jbc-fnv: cannot read %s\n", argv[1]);
        fclose(f);
        return 2;
    }
    fclose(f);
    printf("%llu %016llx\n", bytes, (unsigned long long)h);
    return 0;
}
