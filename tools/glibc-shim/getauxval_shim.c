/* Kobo's glibc (2.15) predates getauxval() (added in glibc 2.16).
 * Rust's std for armv7-unknown-linux-gnueabihf references it unconditionally.
 * Implement it ourselves by reading /proc/self/auxv. */
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>

unsigned long getauxval(unsigned long type) {
    static long auxv[128];
    static int loaded = 0;
    if (!loaded) {
        int fd = open("/proc/self/auxv", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, auxv, sizeof(auxv) - 2 * sizeof(long));
            (void)n;
            close(fd);
        }
        loaded = 1;
    }
    for (int i = 0; auxv[i] != 0; i += 2) {
        if ((unsigned long)auxv[i] == type) return (unsigned long)auxv[i + 1];
    }
    return 0;
}
