# Data Logging System

A Raspberry Pi 4 daemon that ingests vehicle telemetry over USB and persists
it locally. This repository currently contains the project scaffolding,
build system, and baseline syslog integration (Sprint 1, Issue #1). Serial
ingestion, JSON parsing, RAM buffering, SQLite persistence, and the IPC
state machine are implemented in later issues on top of this skeleton.

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
│   └── telemetry.h
├── src/                  # implementation files
│   ├── main.c              # entry point, arg parsing, signal handling, main loop
│   ├── daemon.c             # double-fork daemonization
│   ├── logger.c             # syslog wrapper implementation
│   ├── pidfile.c            # PID file implementation
│   ├── serial_reader.c      # Device reader thread
│   └── telemetry.c          # JSON parse
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
(and, later, the SIGUSR1/SIGUSR2 IPC state machine) can find and signal it.

`make` compiles every `.c` file under `src/` against the headers in
`include/` and produces a single binary, `data-logger`, at the repo root.
Object files land in `build/`, which `make clean` removes along with the
binary.

## Prerequisites

- A Linux build host (this was built and tested on Ubuntu 24.04).
- `gcc` and `make` for a native/local build.
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
startup/shutdown logging works before deploying to hardware.

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

## Notes

- The current `main.c` only proves the scaffold: it daemonizes (or runs in
  the foreground), logs, idles, and exits cleanly. USB ingestion, JSON
  parsing, RAM buffering, and SQLite persistence are added in subsequent
  issues and will replace the placeholder loop.
- `CFLAGS` includes `-Wall -Wextra -Werror` intentionally, so warnings
  fail the build early rather than accumulating across later issues.
- The Yocto meta-layer's bitbake recipe for this app (in the separate
  platform repo) is expected to fetch this repo, build it with the cross
  toolchain, and install `scripts/data-logger-start-stop.sh` (or a
  `.service` unit, if targeting systemd) into the target rootfs — the
  daemon and its start/stop wiring live here in the app repo; only the
  platform-specific installation step belongs in the Yocto layer.
