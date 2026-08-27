#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stddef.h>
#include <termios.h>
#include <unistd.h>

static int write_all(const void *data, size_t len)
{
    const unsigned char *p = data;

    while (len != 0U) {
        ssize_t n = write(STDOUT_FILENO, p, len);

        if (n > 0) {
            p += (size_t)n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    static const char frame[] = "\033[?2026hX\033[?2026l";
    struct termios raw;
    unsigned char byte;

    if (tcgetattr(STDIN_FILENO, &raw) != 0)
        return 1;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        return 1;
    /* Readiness frame: the parent must not inject a sample while the slave
     * is still canonical, because that would benchmark line discipline. */
    if (write_all(frame, sizeof(frame) - 1U) != 0)
        return 1;

    for (;;) {
        ssize_t n = read(STDIN_FILENO, &byte, 1U);

        if (n == 1) {
            if (write_all(frame, sizeof(frame) - 1U) != 0)
                return 1;
        } else if (n == 0) {
            return 0;
        } else if (errno != EINTR) {
            return 1;
        }
    }
}
