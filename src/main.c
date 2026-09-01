#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <syslog.h>
#include "logger.h"
#include "daemon.h"
#include "pidfile.h"

#define PIDFILE_PATH "/var/run/data-logger.pid"

/* Same graceful-shutdown pattern used in the aesdsocket assignment:
 * a single volatile flag flipped by the signal handler, checked by
 * the main loop, so cleanup always happens on the main thread. */
static volatile sig_atomic_t g_running = 1;

static void handle_signal(int signo)
{
    (void)signo;
    g_running = 0;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s [-d]\n", prog_name);
    fprintf(stderr, "  -d   run as a background daemon\n");
}

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    int opt;

    while ((opt = getopt(argc, argv, "d")) != -1) {
        switch (opt) {
        case 'd':
            daemon_mode = 1;
            break;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (daemon_mode) {
        daemonize();
    }

    /* openlog() after daemonize(): the PID syslog tags each message with
     * (via LOG_PID) is the daemon's real PID, not the pre-fork parent's. */
    logger_init("data-logger");
    install_signal_handlers();

    if (pidfile_write(PIDFILE_PATH) != 0) {
        syslog(LOG_WARNING, "could not write pidfile %s", PIDFILE_PATH);
    }

    /* Placeholder main loop for this issue. Ingestion (termios/USB),
     * parsing (cJSON), RAM buffering, and SQLite persistence are
     * separate issues built on top of this scaffold; this loop only
     * proves the daemon starts, logs, idles, and shuts down cleanly on
     * SIGINT/SIGTERM. */
    while (g_running) {
        sleep(1);
    }

    pidfile_remove(PIDFILE_PATH);
    logger_shutdown();
    return EXIT_SUCCESS;
}
