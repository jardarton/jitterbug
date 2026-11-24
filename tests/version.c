/* The library reports the version its header declares. */
#include <jitterbug.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    char expected[32];
    int failed = 0;

    if (jb_version() != (unsigned)JB_VERSION) {
        printf("jb_version() is 0x%06x, the header says 0x%06x\n", jb_version(),
               (unsigned)JB_VERSION);
        failed = 1;
    }
    if (JB_VERSION_NUMBER(1, 2, 3) != 0x010203) {
        printf("JB_VERSION_NUMBER(1, 2, 3) is 0x%06x\n", (unsigned)JB_VERSION_NUMBER(1, 2, 3));
        failed = 1;
    }
    sprintf(expected, "%d.%d.%d", JB_VERSION_MAJOR, JB_VERSION_MINOR, JB_VERSION_PATCH);
    if (strcmp(jb_version_string(), expected) != 0) {
        printf("jb_version_string() is \"%s\", expected \"%s\"\n", jb_version_string(), expected);
        failed = 1;
    }
    return failed;
}
