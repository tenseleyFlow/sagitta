#include <unistd.h>

int main(void)
{
    static const char byte = 'x';

    if (write(STDOUT_FILENO, &byte, 1U) != 1)
        _exit(125);
    _exit(0);
}
