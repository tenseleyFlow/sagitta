#include <limits.h>

int main(void)
{
    volatile int maximum = INT_MAX;
    volatile int one = 1;

    /* Deliberate UB: SAN=1 must stop here instead of returning success. */
    return maximum + one == 0;
}
