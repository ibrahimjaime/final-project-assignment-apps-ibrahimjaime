#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include "daemon.h"

void daemonize(void)
{
    /* First fork: exit the parent so the child is no longer a process
     * group leader, which is required before setsid() below. */
    pid_t pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS); /* parent exits; shell gets control back */
    }

    /* Start a new session, detaching from the controlling terminal. */
    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    /* Ignore SIGHUP: a new session leader can otherwise be sent SIGHUP
     * when its session later acquires a controlling terminal. */
    signal(SIGHUP, SIG_IGN);

    /* Second fork: guarantees the daemon is not a session leader, so it
     * can never accidentally reacquire a controlling terminal. */
    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    /* Reset umask and working directory so the daemon doesn't hold a
     * mount point busy or inherit an unexpected file-creation mask. */
    umask(0);
    if (chdir("/") < 0) {
        exit(EXIT_FAILURE);
    }

    /* Close inherited file descriptors, then redirect stdin/stdout/stderr
     * to /dev/null so any stray printf/fprintf doesn't write to a
     * terminal (or, worse, a file descriptor later reused for something
     * else) once the daemon is detached. */
    for (int fd = (int)sysconf(_SC_OPEN_MAX); fd >= 0; fd--) {
        close(fd);
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) {
            close(fd);
        }
    }
}
