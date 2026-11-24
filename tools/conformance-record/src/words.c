/*
 * jbc-words LIST.jbc: the words a list sends to the GPU, one a line,
 * "GP0 w", "DMA w" (GP0 by DMA) or "GP1 w", with "LOOP" where the main
 * loop starts; nothing else (no reads, waits, checks or call names).
 * record.sh compares two recordings of a program with it (programs.txt,
 * the third field).
 */
#include <jbc_format.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *b = NULL;
    size_t n = 0, cap = 0, got, i, words;
    uint32_t *w;

    if (argc != 2) {
        fprintf(stderr, "usage: jbc-words LIST.jbc\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "jbc-words: cannot open %s\n", argv[1]);
        return 2;
    }
    do {
        if (n == cap) {
            cap = cap ? cap * 2 : 1 << 16;
            b = realloc(b, cap);
            if (b == NULL)
                return 2;
        }
        got = fread(b + n, 1, cap - n, f);
        n += got;
    } while (got > 0);
    fclose(f);
    words = n / 4;
    w = malloc((words ? words : 1) * sizeof *w);
    if (w == NULL || n % 4 != 0 || words < 6 || memcmp(b, JBC_MAGIC, 8) != 0) {
        fprintf(stderr, "jbc-words: %s is not a conformance list\n", argv[1]);
        return 2;
    }
    for (i = 0; i < words; ++i)
        w[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 |
               (uint32_t)b[4 * i + 3] << 24;
    for (i = w[3] / 4; i < words;) {
        unsigned code = JBC_OP_CODE(w[i]);
        size_t cnt = JBC_OP_COUNT(w[i]), k;
        const char *kind = code == JBC_GP0 ? "GP0" : code == JBC_GP0_DMA ? "DMA" : "GP1";

        if (i + 1 + cnt > words) {
            fprintf(stderr, "jbc-words: %s: an op runs past the end\n", argv[1]);
            return 2;
        }
        if (code == JBC_GP0 || code == JBC_GP0_DMA || code == JBC_GP1)
            for (k = 0; k < cnt; ++k)
                printf("%s %08lx\n", kind, (unsigned long)w[i + 1 + k]);
        else if (code == JBC_LOOP)
            printf("LOOP\n");
        i += 1 + cnt;
    }
    free(w);
    free(b);
    return 0;
}
