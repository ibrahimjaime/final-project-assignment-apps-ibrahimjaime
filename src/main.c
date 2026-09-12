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

/* Graceful-shutdown pattern:
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
    fprintf(stderr, "Usage: %s [-d] [-p <serial-device-path>]\n", prog_name);
    fprintf(stderr, "  -d   run as a background daemon\n");
    fprintf(stderr, "  -p   serial device path (default: %s)\n", SERIAL_DEVICE_PATH);
    fprintf(stderr, "       e.g. -p /dev/serial/by-id/usb-...-if00-port0\n");
    fprintf(stderr, "       (list yours with: ls -l /dev/serial/by-id/)\n");
}

/*
 * Called from the serial reader's dedicated thread whenever a
 * complete JSON telemetry line has arrived. For now this just logs
 * it at debug level to prove end-to-end reception; RAM buffering,
 * JSON parsing (cJSON), and SQLite persistence are separate issues
 * that will replace/extend this callback.
 */
static void on_telemetry_line(const char *line, size_t len, void *user_data)
{
    (void)user_data;
    telemetry_record_t rec;
    char err[128];
    bool result = telemetry_parse_line(line, len,
                                   &rec, err, sizeof(err));

    if (!result)
    {
        syslog(LOG_DEBUG, "telemetry read error: %s", err);
    } else {
        syslog(LOG_DEBUG, "telemetry read successful | "
                    "ts: %.3f ms | "
                    "att(p: %.2f, r: %.2f, y: %.2f) | "
                    "spd(lon: %.2f, lat: %.2f, v: %.2f) | "
                    "pos(lon: %.6f, lat: %.6f)", 
        rec.ts,
        rec.attitude.pitch, rec.attitude.roll, rec.attitude.yaw,
        rec.speed.vel_lon,  rec.speed.vel_lat,  rec.speed.v,
        rec.position.lon,   rec.position.lat);
    }

    return;
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
    install_signal_handlers();

    if (pidfile_write(PIDFILE_PATH) != 0) {
        syslog(LOG_WARNING, "could not write pidfile %s", PIDFILE_PATH);
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

    sr_reader_t *serial_reader = sr_reader_create(&serial_cfg, on_telemetry_line, NULL);
    if (!serial_reader) {
        syslog(LOG_ERR, "failed to create serial reader for '%s'", device_path);
        pidfile_remove(PIDFILE_PATH);
        logger_shutdown();
        return EXIT_FAILURE;
    }

    if (sr_reader_start(serial_reader) != 0) {
        syslog(LOG_ERR, "failed to start serial reader thread");
        sr_reader_destroy(serial_reader);
        pidfile_remove(PIDFILE_PATH);
        logger_shutdown();
        return EXIT_FAILURE;
    }

    /* Main loop: idles while the serial reader thread (decoupled,
     * per the architecture) does the actual I/O. RAM buffering,
     * parsing, and persistence hook in here on top of
     * on_telemetry_line in later issues. */
    while (g_running) {
        sleep(1);
    }

    sr_reader_stop(serial_reader);
    sr_reader_destroy(serial_reader);

    pidfile_remove(PIDFILE_PATH);
    logger_shutdown();
    return EXIT_SUCCESS;
}
