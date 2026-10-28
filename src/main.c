#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <syslog.h>
#include "logger.h"
#include "daemon.h"
#include "pidfile.h"
#include "serial_reader.h"
#include "telemetry.h"
#include "ring_buffer.h"

#define PIDFILE_PATH "/var/run/data-logger.pid"

/*
 * Fixed USB device path.
 *
 * Do NOT use "/dev/ttyUSB0" here -- enumeration order can assign a
 * different number across reboots, reconnects, or a different USB
 * port. Since this is a real USB-to-serial adapter (not a virtual
 * port), Linux/udev already creates a stable symlink for it under
 * /dev/serial/by-id/, keyed off the adapter's vendor/product string
 * and serial number, e.g.:
 *
 *   $ ls -l /dev/serial/by-id/
 *   usb-FTDI_FT232R_USB_UART_A1B2C3D4-if00-port0 -> ../../ttyUSB0
 *
 * Look that up once on each machine you test on and set the default
 * below or override it at runtime with -p.
 */
#define SERIAL_DEVICE_PATH \
    "/dev/ttyACM0"

/*
 * Signal handling pattern:
 * handlers only touch volatile sig_atomic_t variables; all real work
 * (printing, logging, cleanup) happens in the main loop.
 *
 * SIGUSR1/SIGUSR2 use counters instead of boolean flags so that two
 * signals delivered back-to-back are still reported as two messages
 * (a plain flag would collapse them into one).
 *
 * The counters are only modified while the main thread is inside
 * sigsuspend() (all four signals are blocked the rest of the time),
 * so the main loop can read and reset them without races.
 */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_usr1_count = 0; /* START */
static volatile sig_atomic_t g_usr2_count = 0; /* STOP  */

static void handle_terminate(int signo)
{
    (void)signo;
    g_running = 0;
}

static void handle_usr1(int signo)
{
    (void)signo;
    g_usr1_count++;
}

static void handle_usr2(int signo)
{
    (void)signo;
    g_usr2_count++;
}

static int install_one_handler(int signo, void (*handler)(int))
{
    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: let sigsuspend()/syscalls wake up */

    return sigaction(signo, &sa, NULL);
}

static int install_signal_handlers(void)
{
    if (install_one_handler(SIGINT,  handle_terminate) != 0) return -1;
    if (install_one_handler(SIGTERM, handle_terminate) != 0) return -1;
    if (install_one_handler(SIGUSR1, handle_usr1)      != 0) return -1;
    if (install_one_handler(SIGUSR2, handle_usr2)      != 0) return -1;
    return 0;
}

/*
 * Blocks the signals we care about for the calling thread and returns
 * the previous mask in *old_mask (to be handed to sigsuspend()).
 *
 * Must be called BEFORE any thread is created (i.e. before
 * sr_reader_start()): new threads inherit the blocked mask, so these
 * process-directed signals are always delivered to the main thread,
 * and only while it sits in sigsuspend(). This also closes the classic
 * race between "check the flag" and "go to sleep".
 */
static int block_signals(sigset_t *old_mask)
{
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGUSR2);

    return sigprocmask(SIG_BLOCK, &block, old_mask);
}

static void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s [-d] [-p <serial-device-path>]\n", prog_name);
    fprintf(stderr, "  -d   run as a background daemon\n");
    fprintf(stderr, "  -p   serial device path (default: %s)\n", SERIAL_DEVICE_PATH);
    fprintf(stderr, "       e.g. -p /dev/serial/by-id/usb-...-if00-port0\n");
    fprintf(stderr, "       (list yours with: ls -l /dev/serial/by-id/)\n");
}

/*
 * Prints to stdout (visible when running in the foreground) and to
 * syslog (the only place visible when running with -d).
 */
static void announce(int priority, const char *msg)
{
    printf("%s\n", msg);
    fflush(stdout);
    syslog(priority, "%s", msg);
}

/*
 * Called from the serial reader's dedicated thread whenever a
 * complete JSON telemetry line has arrived. Parses it and, on
 * success, hands it off to the thread-safe RAM ring buffer
 * for the persistence thread to drain later. SQLite
 * persistence itself is a separate issue that will add a consumer
 * thread calling rb_dequeue() on the same buffer.
 *
 * user_data is the ring_buffer_t* passed in via sr_reader_create()
 * below -- see main().
 */
static void on_telemetry_line(const char *line, size_t len, void *user_data)
{
    ring_buffer_t *rb = (ring_buffer_t *)user_data;
    telemetry_record_t rec;
    char err[128];
    bool result = telemetry_parse_line(line, len,
                                   &rec, err, sizeof(err));

    if (!result)
    {
        syslog(LOG_DEBUG, "telemetry read error: %s", err);
        return;
    }

    syslog(LOG_DEBUG, "telemetry read successful | "
                "ts: %.3f ms | "
                "att(p: %.2f, r: %.2f, y: %.2f) | "
                "spd(lon: %.2f, lat: %.2f, v: %.2f) | "
                "pos(lon: %.6f, lat: %.6f)", 
    rec.ts,
    rec.attitude.pitch, rec.attitude.roll, rec.attitude.yaw,
    rec.speed.vel_lon,  rec.speed.vel_lat,  rec.speed.v,
    rec.position.lon,   rec.position.lat);

    /* Never blocks, never fails -- see ring_buffer.h for the
     * documented drop-oldest policy if the persistence thread
     * (Sprint 2) ever falls behind. */
    rb_enqueue(rb, &rec);
}

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    const char *device_path = SERIAL_DEVICE_PATH;
    int opt;

    while ((opt = getopt(argc, argv, "dp:")) != -1) {
        switch (opt) {
        case 'd':
            daemon_mode = 1;
            break;
        case 'p':
            device_path = optarg;
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

    if (install_signal_handlers() != 0) {
        syslog(LOG_ERR, "failed to install signal handlers");
        logger_shutdown();
        return EXIT_FAILURE;
    }

    /* Block our signals before any thread exists.
     * They are only unblocked atomically inside sigsuspend() below. */
    sigset_t orig_mask;
    if (block_signals(&orig_mask) != 0) {
        syslog(LOG_ERR, "failed to block signals");
        logger_shutdown();
        return EXIT_FAILURE;
    }

    /* Startup banner: PID + usage hint. */
    {
        char banner[160];
        snprintf(banner, sizeof(banner), "data-logger started, PID %ld",
                 (long)getpid());
        announce(LOG_INFO, banner);
        snprintf(banner, sizeof(banner),
                 "  kill -USR1 %ld  -> START | kill -USR2 %ld  -> STOP | "
                 "kill -TERM %ld  -> exit",
                 (long)getpid(), (long)getpid(), (long)getpid());
        announce(LOG_INFO, banner);
    }

    if (pidfile_write(PIDFILE_PATH) != 0) {
        syslog(LOG_WARNING, "could not write pidfile %s", PIDFILE_PATH);
    }

    /* In-memory telemetry buffer. Lives for the whole daemon
     * lifetime: created before the serial reader starts producing into
     * it, destroyed after the reader thread has been stopped so nothing
     * can still be enqueuing into it. */
    ring_buffer_t telemetry_rb;
    if (rb_init(&telemetry_rb) != 0) {
        syslog(LOG_ERR, "failed to initialize telemetry ring buffer");
        pidfile_remove(PIDFILE_PATH);
        logger_shutdown();
        return EXIT_FAILURE;
    }

    sr_config_t serial_cfg;
    sr_config_default(&serial_cfg);
    snprintf(serial_cfg.device_path, sizeof(serial_cfg.device_path), "%s",
              device_path);
    /* Adjust these if they don't match the acquisition device's spec: */
    serial_cfg.baud_rate = 115200;
    serial_cfg.data_bits = 8;
    serial_cfg.parity = SR_PARITY_NONE;
    serial_cfg.stop_bits = 1;

    syslog(LOG_INFO, "using serial device '%s'", device_path);

    sr_reader_t *serial_reader = sr_reader_create(&serial_cfg, on_telemetry_line,
                                                   &telemetry_rb);
    if (!serial_reader) {
        syslog(LOG_ERR, "failed to create serial reader for '%s'", device_path);
        rb_destroy(&telemetry_rb);
        pidfile_remove(PIDFILE_PATH);
        logger_shutdown();
        return EXIT_FAILURE;
    }

    if (sr_reader_start(serial_reader) != 0) {
        syslog(LOG_ERR, "failed to start serial reader thread");
        sr_reader_destroy(serial_reader);
        rb_destroy(&telemetry_rb);
        pidfile_remove(PIDFILE_PATH);
        logger_shutdown();
        return EXIT_FAILURE;
    }

    /* Main loop: sleeps in sigsuspend() (zero CPU while idle) and wakes
     * only when one of our signals arrives. The serial reader thread
     * (decoupled, per the architecture) does the actual I/O, enqueuing
     * parsed records into telemetry_rb. The persistence thread
     * hooks in here, draining telemetry_rb with rb_dequeue() and
     * writing to SQLite.
     *
     * Invariant: at the top of each iteration our signals are blocked,
     * so the counters/flag cannot change between the checks and the
     * next sigsuspend() -- no lost wake-ups. */
    while (g_running) {
        /* Atomically unblocks the signals and sleeps; returns after a
         * handler has run, with the signals blocked again. */
        sigsuspend(&orig_mask);

        /* One message per signal received. START/STOP are only
         * reported for now: no state machine yet. */
        while (g_usr1_count > 0) {
            g_usr1_count--;
            announce(LOG_INFO, "SIGUSR1 received: START");
        }
        while (g_usr2_count > 0) {
            g_usr2_count--;
            announce(LOG_INFO, "SIGUSR2 received: STOP");
        }
    }

    announce(LOG_INFO, "termination signal received, shutting down");

    sr_reader_stop(serial_reader);
    sr_reader_destroy(serial_reader);

    /* Safe to destroy now: the reader thread that could enqueue into
     * telemetry_rb has been stopped above. */
    rb_destroy(&telemetry_rb);

    pidfile_remove(PIDFILE_PATH);
    logger_shutdown();
    return EXIT_SUCCESS;
}