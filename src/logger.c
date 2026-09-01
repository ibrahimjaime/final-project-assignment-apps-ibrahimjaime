#include <syslog.h>
#include "logger.h"

void logger_init(const char *ident)
{
    /* LOG_PID: prefix each message with the PID.
     * LOG_CONS: fall back to console if the daemon can't reach syslogd.
     * LOG_DAEMON: this is a background service, not an interactive tool. */
    openlog(ident, LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "data-logger starting up");
}

void logger_shutdown(void)
{
    syslog(LOG_INFO, "data-logger shutting down");
    closelog();
}
