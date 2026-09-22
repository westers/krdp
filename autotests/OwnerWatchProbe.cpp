// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#include "VirtualSessionOwnerWatch.h"
#include <sys/syscall.h>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    alarm(5);
    int fd = int(syscall(SYS_pidfd_open, getppid(), 0));
    if (fd < 0) return 1;
    if (!strcmp(argv[1], "parent-exit")) {
        // The monitor child gets a pidfd for this disposable parent, not the
        // test runner. Fork before creating any thread.
        close(fd); fd = int(syscall(SYS_pidfd_open, getpid(), 0));
        if (fd < 0) return 1;
        int ready[2];
        if (pipe2(ready, O_CLOEXEC)) return 1;
        const auto child = fork();
        if (child < 0) return 1;
        if (child > 0) {
            close(ready[1]);
            char marker; ssize_t count;
            do { count = read(ready[0], &marker, 1); } while (count < 0 && errno == EINTR);
            return count == 1 && marker == 'R' ? 0 : 1;
        }
        close(ready[0]);
        alarm(5);
        KRdp::VirtualSessionOwnerWatch watch(fd);
        watch.retained();
        // Keep stdout inherited so the runner can verify the monitor PID.
        char pid[32]; const auto count = snprintf(pid, sizeof(pid), "%d\n", int(getpid()));
        if (write(1, pid, count) != count) return 1;
        if (write(ready[1], "R", 1) != 1) return 1;
        close(ready[1]);
        for (;;) pause();
    }
    KRdp::VirtualSessionOwnerWatch watch(fd, 60);
    if (!strcmp(argv[1], "retained")) { watch.retained(); usleep(200000); return 0; }
    if (!strcmp(argv[1], "closing")) { watch.retained(); watch.closing(60); }
    for (;;) pause();
}
