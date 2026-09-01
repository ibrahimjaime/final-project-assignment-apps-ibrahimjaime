#ifndef PIDFILE_H
#define PIDFILE_H

/* Write the calling process's PID to `path`. Returns 0 on success, -1 on
 * failure (e.g. permission denied). Used by the start-stop script to find
 * the running daemon's PID for STOP/RESTART. */
int pidfile_write(const char *path);

/* Remove the PID file at `path`, if present. Safe to call even if it
 * doesn't exist. */
void pidfile_remove(const char *path);

#endif /* PIDFILE_H */
