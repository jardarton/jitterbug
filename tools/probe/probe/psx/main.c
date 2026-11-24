/*
 * The probe on the console: set up the machine, identify it, then serve
 * sessions for ever.
 */
#include "probe.h"

void psx_init(void);
int main(void);

int main(void)
{
    psx_init();
    probe_init();
    for (;;)
        (void)probe_serve(0);
}
