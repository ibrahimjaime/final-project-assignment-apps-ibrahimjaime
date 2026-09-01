#ifndef DAEMON_H
#define DAEMON_H

/*
 * Detach the calling process from its controlling terminal and become a
 * background daemon: double-fork, start a new session, redirect the
 * standard file descriptors to /dev/null, and reset the working
 * directory and umask. Call this before opening any resources (log
 * handles, sockets, files) that should belong to the daemon process
 * rather than the original foreground process.
 */
void daemonize(void);

#endif /* DAEMON_H */
