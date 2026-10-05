#include "instance_guard.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    char path[512], link[512];
    snprintf(path, sizeof(path), "%s/instance.lock", argv[1]);
    snprintf(link, sizeof(link), "%s/instance.link", argv[1]);
    /* A stale file naming a live unrelated process must not signal it. */
    pid_t unrelated = fork(); assert(unrelated >= 0);
    if (!unrelated) { for (;;) pause(); }
    FILE *f = fopen(path, "w"); assert(f);
    fprintf(f, "%d\n", (int)unrelated); fclose(f);
    int fd = instance_claim(path, 1); assert(fd >= 0);
    assert(kill(unrelated, 0) == 0); close(fd);
    assert(kill(unrelated, SIGTERM) == 0);
    int status; assert(waitpid(unrelated, &status, 0) == unrelated);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    /* Replacement follows the current kernel lock owner, not the file text. */
    int ready[2]; assert(pipe(ready) == 0);
    pid_t owner = fork(); assert(owner >= 0);
    if (!owner) {
        close(ready[0]);
        int held = instance_claim(path, 0); assert(held >= 0);
        assert(write(ready[1], "1", 1) == 1); close(ready[1]);
        for (;;) pause();
    }
    close(ready[1]); char c;
    assert(read(ready[0], &c, 1) == 1); close(ready[0]);
    assert(instance_claim(path, 0) < 0 && kill(owner, 0) == 0);
    fd = instance_claim(path, 1); assert(fd >= 0);
    assert(waitpid(owner, &status, 0) == owner);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    close(fd);
    assert(symlink(path, link) == 0);
    assert(instance_claim(link, 1) < 0);
    assert(instance_claim(argv[1], 1) < 0);
    unlink(link); unlink(path);
    puts("PASS: stale PIDs never signaled, exclusive ownership, verified replacement, directory/symlink refusal");
    return 0;
}
