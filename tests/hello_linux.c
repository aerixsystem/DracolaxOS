/* tests/hello_linux.c
 * Minimal musl/static test binary for the Linux ABI compatibility layer.
 * Build: musl-gcc -m32 -static -o tests/bin/hello tests/hello_linux.c
 * Run:   draco> exec /ramfs/hello
 */
#include <unistd.h>
#include <sys/utsname.h>

int main(void) {
    write(1, "hello from linux abi\n", 21);

    /* Test uname */
    struct utsname u;
    if (uname(&u) == 0) {
        write(1, "uname: ", 7);
        write(1, u.sysname, __builtin_strlen(u.sysname));
        write(1, " ", 1);
        write(1, u.release, __builtin_strlen(u.release));
        write(1, "\n", 1);
    }

    write(1, "linux abi test: PASS\n", 21);
    return 0;
}
