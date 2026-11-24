#include <jitterbug.h>

#define JB_STR_(x) #x
#define JB_STR(x) JB_STR_(x)

unsigned jb_version(void)
{
    return JB_VERSION;
}

const char *jb_version_string(void)
{
    return JB_STR(JB_VERSION_MAJOR) "." JB_STR(JB_VERSION_MINOR) "." JB_STR(JB_VERSION_PATCH);
}
