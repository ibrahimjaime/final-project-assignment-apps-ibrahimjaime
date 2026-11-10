# Data Logging System

A Raspberry Pi 4 daemon that ingests vehicle telemetry over USB and persists
it locally. The daemon reads JSON telemetry from a serial device, parses it,
buffers it in a RAM ring buffer, and saves it to SQLite in batches. Logging
is controlled at runtime with POSIX signals (`SIGUSR1` = START, `SIGUSR2` =
STOP). See [SQLite persistence](#sqlite-persistence) for how data is stored
and how to inspect it.

## Repository layout

```
.
├── Makefile              # single entry point: `make`, `make clean`
├── README.md             # this file
├── include/              # public headers, shared across src/
│   ├── daemon.h            # daemonize(): detach from terminal, new session
│   ├── logger.h            # syslog wrapper
│   ├── pidfile.h           # write/remove /var/run/data-logger.pid
│   ├── serial_reader.h
│   ├── telemetry.h
│   ├── ring_buffer.h       # thread-safe circular RAM buffer (drop-oldest)
│   ├── db.h                # SQLite module: db_init/db_insert_batch/db_clear/db_close
│   └── writer.h            # persistence thread: START/STOP/EXIT commands
├── src/                  # implementation files
│   ├── main.c              # entry point, arg parsing, signal handling, main loop
│   ├── daemon.c             # double-fork daemonization
│   ├── logger.c             # syslog wrapper implementation
│   ├── pidfile.c            # PID file implementation
│   ├── serial_reader.c      # Device reader thread
│   ├── telemetry.c          # JSON parse
│   ├── ring_buffer.c        # circular buffer (+ sequence numbers, clear)
│   ├── db.c                 # SQLite access: schema, batched inserts, wipe
│   └── writer.c             # persistence thread (batching, flush, overrun detection)
├── db/
│   └── schema.sql           # SQLite schema; compiled into the binary at build time
├── scripts/               # target-side service management
│   └── data-logger-start-stop.sh   # start/stop/restart/status, aesdsocket-style
├── vendor/
│   └── cjson #json parser library
└── tests/
    └── test_telemetry.c   #automated test for telemetry parser functionality
```

## Why this is a daemon

The logger has to run unattended on the Pi for as long as the board is
powered — detached from whatever SSH session started it, restartable by an
init system, and controllable by PID rather than by an attached terminal.
`-d` forks twice, starts a new session, detaches stdin/stdout/stderr, and
writes its PID to `/var/run/data-logger.pid` so `scripts/data-logger-start-stop.sh`
(and the SIGUSR1/SIGUSR2 START/STOP control) can find and signal it.

`make` compiles every `.c` file under `src/` against the headers in
`include/` and produces a single binary, `data-logger`, at the repo root.
Object files land in `build/`, which `make clean` removes along with the
binary.

## Prerequisites

- A Linux build host (this was built and tested on Ubuntu 24.04).
- `gcc` and `make` for a native/local build.
- SQLite development files for a native build:
  `sudo apt install libsqlite3-dev` (and `sqlite3` for the command-line
  client used to inspect the database). The Makefile links with `-lsqlite3`.
- **For cross-compiling to the Raspberry Pi 4:** a Yocto Project SDK for the
  Pi 4 target. If you already built a Yocto image for the Raspberry Pi 4 in
  an earlier course assignment, reuse that same `poky` checkout and build
  directory — you only need to add the SDK population step below, not start
  over.

## Native build (for local development/testing)

```sh
make
./data-logger        # Ctrl+C to stop; runs an idle loop and logs via syslog
make clean
```

This is the fastest way to confirm the code compiles and the
startup/shutdown logging works before deploying to hardware. The daemon
needs a writable directory for the database, so for local runs point it at
a temporary file, e.g. `./data-logger -b /tmp/telemetry.db -p <device>`.

## Cross-compile for the Raspberry Pi 4 (Yocto SDK)

These steps assume Yocto/Poky **from scratch**. If you already have a
working `poky` + `meta-raspberrypi` setup from prior coursework, skip to
step 3.

1. **Fetch Poky and the Raspberry Pi layer** (matching the release you used
   in previous assignments — check `git branch` in your existing checkout
   if you're reusing one):

   ```sh
   git clone git://git.yoctoproject.org/poky
   cd poky
   git clone git://git.yoctoproject.org/meta-raspberrypi
   ```

2. **Configure the build for the Raspberry Pi 4** and build a minimal
   image once, so the toolchain and sysroot exist:

   ```sh
   source oe-init-build-env
   # in conf/local.conf: MACHINE = "raspberrypi4-64"
   # in conf/bblayers.conf: add the path to meta-raspberrypi
   bitbake core-image-minimal
   ```

3. **Generate a standalone cross-compilation SDK** from that same build
   directory. This produces a self-extracting installer containing the
   cross `gcc`, matching `libc`, and the target sysroot:

   ```sh
   bitbake -c populate_sdk core-image-minimal
   ```

   The installer is written to `tmp/deploy/sdk/` as something like
   `poky-glibc-x86_64-core-image-minimal-cortexa72-raspberrypi4-64-toolchain-<version>.sh`.

4. **Install the SDK** (only needs to be done once per build host):

   ```sh
   ./tmp/deploy/sdk/poky-glibc-...-toolchain-<version>.sh
   # accept the default install path, e.g. /opt/poky/<version>
   ```

5. **Source the environment setup script** in the shell you'll build from.
   This exports `CC`, `CFLAGS`, `LDFLAGS`, `SYSROOT`, etc. as the cross
   toolchain for `cortexa72-raspberrypi4-64`:

   ```sh
   source /opt/poky/<version>/environment-setup-cortexa72-poky-linux
   ```

6. **Build** — the Makefile does not need to know about Yocto at all; it
   picks up the exported `CC` automatically because that variable is
   already set in the environment before `make` runs:

   ```sh
   make
   file data-logger   # should report an ARM aarch64 ELF binary, not x86-64
   ```

7. **Deploy to the Pi 4** over the network:

   ```sh
   scp data-logger pi@<raspberrypi-ip>:~/data-logger
   ```

## Running on the target and verifying logging

**Foreground (for quick manual testing):**

```sh
ssh pi@<raspberrypi-ip>
chmod +x data-logger
./data-logger        # Ctrl+C (SIGINT) to stop
```

**As a background daemon:**

```sh
sudo ./data-logger -d
cat /var/run/data-logger.pid    # confirm the PID file was written
```

**Via the start-stop script** (install `data-logger` to `/usr/bin/` first,
matching the path in `scripts/data-logger-start-stop.sh`):

```sh
sudo cp data-logger /usr/bin/data-logger
sudo cp scripts/data-logger-start-stop.sh /etc/init.d/data-logger
sudo /etc/init.d/data-logger start
sudo /etc/init.d/data-logger status
sudo /etc/init.d/data-logger stop
```

Confirm the log messages:

```sh
grep data-logger /var/log/messages
```

You should see an `data-logger starting up` line. Stop the process (via
`kill <pid>`, `Ctrl+C` in foreground mode, or the start-stop script's
`stop` action), then re-check the journal for the matching
`data-logger shutting down` line and confirm `/var/run/data-logger.pid`
was removed — this proves the signal handler drives a clean shutdown
rather than an abrupt kill.

## Dependencies

### cJSON

Telemetry JSON payloads (see `on_telemetry_line`) are parsed using
[cJSON](https://github.com/DaveGamble/cJSON) (MIT licensed).

This project vendors cJSON directly rather than requiring a system
package, so no extra install step is needed:

```
vendor/cjson/cJSON.h
vendor/cjson/cJSON.c
```

To refresh the vendored copy from upstream:

```sh
curl -sL -o vendor/cjson/cJSON.h https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
curl -sL -o vendor/cjson/cJSON.c https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
```

If you'd rather use a system/package-manager copy instead of vendoring
(e.g. `apt install libcjson-dev` on Debian/Ubuntu, or `brew install
cjson` on macOS), drop the `vendor/cjson` directory, add `#include
<cjson/cJSON.h>` in `telemetry.c`, and link with `-lcjson` instead of
compiling `cJSON.c` directly.

### SQLite

Samples are stored with [SQLite](https://www.sqlite.org/) (the system
library, not vendored):

- **Native build:** `sudo apt install libsqlite3-dev`, linked with `-lsqlite3`.
- **Yocto recipe:** `DEPENDS += "sqlite3"` (build-time headers/library; the
  runtime library is pulled into the image automatically). Add `sqlite3` to
  `IMAGE_INSTALL` as well if you want the `sqlite3` command-line client on
  the target for inspecting the database.
- The schema uses `STRICT` tables, which need **SQLite 3.37 or newer**
  (Yocto Kirkstone ships 3.38.5).

## Runtime control

The logger is controlled at runtime with POSIX signals. START and STOP drive
the SQLite persistence (see [SQLite persistence](#sqlite-persistence)).

| Signal    | Meaning | Behaviour                                                                   |
|-----------|---------|-----------------------------------------------------------------------------|
| `SIGUSR1` | START   | prints `SIGUSR1 received: START`; **deletes all stored data** (database and RAM buffer) and starts saving samples |
| `SIGUSR2` | STOP    | prints `SIGUSR2 received: STOP`; saves every sample still pending in RAM, then stops saving |
| `SIGINT`  | exit    | saves pending samples (if logging), closes the database, exit code 0        |
| `SIGTERM` | exit    | saves pending samples (if logging), closes the database, exit code 0        |

RESTART is out of scope for now (`SIGHUP` vs. FIFO to be decided later).

Messages go to stdout (foreground) and to syslog (always; the only place
you will see them with `-d`).

### Manual test steps

Build with warnings enabled and confirm there are none:

```sh
gcc -Wall -Wextra -pthread -o data-logger *.c   # or: make
```

1. **Start and PID banner**

   ```sh
   ./data-logger -p /dev/serial/by-id/<your-adapter>
   ```

   Expected:

   ```
   data-logger started, PID 12345
     kill -USR1 12345  -> START | kill -USR2 12345  -> STOP | kill -TERM 12345  -> exit
   ```

2. **START / STOP** (from a second terminal, use the PID printed above)

   ```sh
   kill -USR1 <pid>   # -> "SIGUSR1 received: START"
   kill -USR2 <pid>   # -> "SIGUSR2 received: STOP"
   ```

   Each signal must produce exactly one message. Sending several in a row
   must produce the same number of messages:

   ```sh
   for i in 1 2 3; do kill -USR1 <pid>; done   # 3 START lines
   ```

3. **No CPU usage while idle**

   ```sh
   top -p <pid>      # or: ps -o pid,pcpu,stat -p <pid>
   ```

   `%CPU` should stay at 0.0 and the main thread should be sleeping (`S`).

4. **Clean exit**

   ```sh
   kill -INT <pid>;  echo $?     # run in foreground, press Ctrl+C -> exit code 0
   kill -TERM <pid>              # then check: wait <pid>; echo $?  -> 0
   ```

   Verify the pidfile `/var/run/data-logger.pid` is removed afterwards.

5. **Daemon mode**

   ```sh
   ./data-logger -d -p /dev/serial/by-id/<your-adapter>
   cat /var/run/data-logger.pid
   kill -USR1 $(cat /var/run/data-logger.pid)
   journalctl -t data-logger -n 5     # or: grep data-logger /var/log/syslog
   ```

## SQLite persistence

Parsed telemetry is buffered in RAM and saved to SQLite by a dedicated
writer thread, so a slow disk never blocks the serial reader.

```
serial thread                        writer thread (src/writer.c)             src/db.c
on_telemetry_line()
  rb_enqueue() ──► ring buffer ──► rb_try_dequeue_seq()
                  (RAM, 1000)       collect FLUSH_EVERY_N samples
                                    flush ─────────────────────────────────► db_insert_batch()
                                                                              BEGIN; N x INSERT; COMMIT
```

### Behaviour

| Event | What happens |
|-------|--------------|
| `SIGUSR1` (START) | The `telemetry` table and the RAM buffer are cleared; from now on samples are saved. Samples received before START are never stored. |
| While logging | Every `FLUSH_EVERY_N` samples are written in **one transaction** (prepared statement + bind, not one insert per sample). The row count therefore grows in steps of `FLUSH_EVERY_N`. |
| `SIGUSR2` (STOP) | Everything still in RAM and not yet saved is written, including a final partial batch. The row count then equals the samples produced since START. |
| `SIGINT` / `SIGTERM` | Same flush as STOP (if logging), then the database is closed cleanly. |

Design points:

- All SQLite work happens in the writer thread. Signal handlers only set
  counters; they never touch the database.
- The producer never blocks on SQLite. The writer takes the ring-buffer
  lock only to copy out one record, never while SQL runs.
- Every SQLite return code is checked and logged to syslog. A batch that
  fails is rolled back and retried; its samples stay in RAM meanwhile.
- Pragmas: `journal_mode=WAL` and `synchronous=NORMAL`. WAL creates
  `telemetry.db-wal` and `telemetry.db-shm` next to the database file, and
  lets you query the database while the daemon is writing.

### Options

| Option | Meaning | Default |
|--------|---------|---------|
| `-d` | run as a background daemon | off |
| `-p <path>` | serial device | `/dev/ttyACM0` |
| `-b <path>` | SQLite database file | `/var/lib/telemetry/telemetry.db` |
| `-n <N>` | `FLUSH_EVERY_N`: samples per batch, `0 < N < 1000` | `100` |

`FLUSH_EVERY_N` must be smaller than `RING_BUFFER_CAPACITY` (1000). The
program checks this before it daemonizes and exits with code 1 and a clear
message otherwise:

```
error: FLUSH_EVERY_N (1000) must be > 0 and < RING_BUFFER_CAPACITY (1000)
```

### Database

- **Location:** `/var/lib/telemetry/telemetry.db` (persistent storage, not
  tmpfs). The daemon does **not** create the directory: the Yocto recipe
  installs `/var/lib/telemetry`; for a manual run use
  `sudo mkdir -p /var/lib/telemetry` or pass `-b`.
- **Creation:** the table and indexes are created automatically on first
  run (`CREATE TABLE IF NOT EXISTS`, the contents of `db/schema.sql`).
  `db/schema.sql` is the single source of truth: `gen_schema_header.sh`
  turns it into a C header during `make`, so there is no second copy to
  keep in sync.
- **Schema (`telemetry` table):**

| Column | Type | Meaning |
|--------|------|---------|
| `id` | INTEGER PK AUTOINCREMENT | row id (keeps counting across START) |
| `seq` | INTEGER NOT NULL UNIQUE | sequence number from the RAM buffer (lifetime enqueue index) |
| `ts` | REAL | sample timestamp from the telemetry source, in ms |
| `created_at` | INTEGER | insert time, Unix epoch seconds (UTC) |
| `pitch`, `roll`, `yaw` | REAL | attitude |
| `vel_lon`, `vel_lat`, `v` | REAL | speed components and total |
| `lon`, `lat` | REAL | position |

### Overruns (lost samples)

The ring buffer overwrites its oldest record when full (it never blocks
the producer). The writer detects this from the `seq` numbers: if a record
arrives with a higher number than expected, it logs
`writer: OVERRUN: N samples overwritten before being saved` and counts
them. Lost samples appear as gaps in the `seq` column, and the STOP/exit
log line reports the totals (`saved X samples, lost Y`).

### Inspecting the database (on the Pi, over SSH)

```sh
DB=/var/lib/telemetry/telemetry.db
sqlite3 $DB ".schema"
sqlite3 $DB "SELECT COUNT(*) FROM telemetry;"
sqlite3 -header -column $DB "SELECT * FROM telemetry ORDER BY seq DESC LIMIT 10;"
# duplicates / missing samples: gaps must be 0 unless an OVERRUN was logged
sqlite3 $DB "SELECT COUNT(*), MAX(seq)-MIN(seq)+1-COUNT(*) AS gaps FROM telemetry;"
# export to CSV
sqlite3 -csv -header $DB "SELECT * FROM telemetry;" > /tmp/telemetry.csv
```

Daemon messages (`writer: START`, `writer: STOP: saved X samples, lost Y`,
SQLite errors) are in the system log:
`journalctl -t data-logger -f`, or `logread -f` with BusyBox syslog.

### Manual acceptance test

Start the daemon (`/etc/init.d/data-logger start`, or in the foreground),
then use its PID (`cat /var/run/data-logger.pid`):

1. **Start:** `kill -USR1 <pid>`. Run
   `sqlite3 $DB "SELECT COUNT(*) FROM telemetry;"` repeatedly: the count
   grows in steps of `FLUSH_EVERY_N`.
2. **Stop:** `kill -USR2 <pid>`. The count equals the `saved X samples`
   number in the log, and the `gaps` query returns 0.
3. **Start again:** `kill -USR1 <pid>`. The table is emptied (count 0) and
   then grows again.
4. **Exit:** `kill -TERM <pid>` while logging. The log shows
   `writer: exit: saved X samples`, the rows are in the database, and
   `/var/run/data-logger.pid` is removed.
5. **Startup guard:** `data-logger -n 1000; echo $?` prints the error above
   and `1`.

## Notes

- `main.c` wires the pieces together: it blocks the control signals before
  any thread is created, starts the writer and serial-reader threads, and
  sleeps in `sigsuspend()` until a signal arrives. On exit it stops the
  reader, lets the writer flush and exit, then closes the database.
- `CFLAGS` includes `-Wall -Wextra -Werror` intentionally, so warnings
  fail the build early rather than accumulating across later issues.
- The Yocto meta-layer's bitbake recipe for this app (in the separate
  platform repo) is expected to fetch this repo, build it with the cross
  toolchain, and install `scripts/data-logger-start-stop.sh` (or a
  `.service` unit, if targeting systemd) into the target rootfs — the
  daemon and its start/stop wiring live here in the app repo; only the
  platform-specific installation step belongs in the Yocto layer.
- The recipe needs `DEPENDS += "sqlite3"` and must create the database
  directory, since the daemon does not:

  ```
  do_install:append() {
      install -d ${D}${localstatedir}/lib/telemetry
  }
  FILES:${PN} += "${localstatedir}/lib/telemetry"
  ```