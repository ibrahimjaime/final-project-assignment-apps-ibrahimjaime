#ifndef LOGGER_H
#define LOGGER_H

/*
 * Thin wrapper around the Linux syslog(3) API.
 *
 * Conventions follow the prior aesdsocket assignment: openlog() is called
 * once at process startup with LOG_PID so each line in the journal /
 * /var/log/syslog can be traced back to a specific run of the daemon,
 * and closelog() is called once at clean shutdown.
 */

void logger_init(const char *ident);
void logger_shutdown(void);

#endif /* LOGGER_H */
