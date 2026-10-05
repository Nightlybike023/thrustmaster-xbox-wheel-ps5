#include "instance_guard.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>

int instance_claim(const char *path, int replace) {
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) goto failed;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto failed; }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) goto failed;
    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_SETLK, &lock) != 0) {
        if (!replace || (errno != EAGAIN && errno != EACCES)) goto failed;
        struct flock owner = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
        if (fcntl(fd, F_GETLK, &owner) != 0) goto failed;
        if (owner.l_type != F_UNLCK) {
            if (owner.l_pid <= 1 || owner.l_pid == getpid()) { errno = EBUSY; goto failed; }
            if (kill(owner.l_pid, SIGTERM) != 0 && errno != ESRCH) goto failed;
        }
        int acquired = 0;
        for (int i = 0; i < 25; i++) {
            if (fcntl(fd, F_SETLK, &lock) == 0) { acquired = 1; break; }
            if (errno != EAGAIN && errno != EACCES) goto failed;
            usleep(100000);
        }
        if (!acquired) { errno = EBUSY; goto failed; }
    }
    char pid[32];
    int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
    if (ftruncate(fd, 0) != 0) goto failed;
    ssize_t written = pwrite(fd, pid, (size_t)n, 0);
    if (written != n) { if (written >= 0) errno = EIO; goto failed; }
    return fd;
failed:;
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}
