# Unit test build for the telemetry (cJSON) parser AND the thread-safe
# ring buffer. Each test is its own independent target with its own
# sources, so building/running one never requires the other's files to
# even compile cleanly.
#
# These tests need to build and run natively on the host, so
# CC/CFLAGS/LDFLAGS/LDLIBS are all forced with `override` here -- this
# ignores any CC/CFLAGS/LDFLAGS the SDK setup script may have exported
# into the shell's environment.
#
# Usage (from repo root or from tests/):
#   make -C tests                    # build + run BOTH tests
#   make -C tests test                # same as above
#   make -C tests test-telemetry      # build + run only test_telemetry
#   make -C tests test-ring-buffer    # build + run only test_ring_buffer
#   make -C tests test_telemetry      # build only (no run)
#   make -C tests test_ring_buffer    # build only (no run)
#   make -C tests clean

override CC      := gcc
override CFLAGS  := -Wall -Wextra -std=gnu11 -I../include -I../vendor/cjson -pthread
override LDFLAGS := -pthread
override LDLIBS  := -lm

ROOT_DIR  := ..
BUILD_DIR := build

# ---------------------------------------------------------------------------
# Telemetry (cJSON) parser test
# ---------------------------------------------------------------------------
TELEMETRY_TARGET := test_telemetry
TELEMETRY_SRCS   := test_telemetry.c \
                     $(ROOT_DIR)/src/telemetry.c \
                     $(ROOT_DIR)/vendor/cjson/cJSON.c
TELEMETRY_DEPS   := $(ROOT_DIR)/include/telemetry.h \
                     $(ROOT_DIR)/vendor/cjson/cJSON.h

# ---------------------------------------------------------------------------
# Ring buffer test
# ---------------------------------------------------------------------------
RINGBUF_TARGET := test_ring_buffer
RINGBUF_SRCS   := test_ring_buffer.c \
                   $(ROOT_DIR)/src/ring_buffer.c
RINGBUF_DEPS   := $(ROOT_DIR)/include/ring_buffer.h

.PHONY: all test test-telemetry test-ring-buffer clean

# Default: build + run everything.
all: test

test: test-telemetry test-ring-buffer

test-telemetry: $(TELEMETRY_TARGET)
	./$(TELEMETRY_TARGET)

test-ring-buffer: $(RINGBUF_TARGET)
	./$(RINGBUF_TARGET)

$(TELEMETRY_TARGET): $(TELEMETRY_SRCS) $(TELEMETRY_DEPS)
	$(CC) $(CFLAGS) -o $@ $(TELEMETRY_SRCS) $(LDFLAGS) $(LDLIBS)

$(RINGBUF_TARGET): $(RINGBUF_SRCS) $(RINGBUF_DEPS)
	$(CC) $(CFLAGS) -o $@ $(RINGBUF_SRCS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TELEMETRY_TARGET) $(RINGBUF_TARGET)
	rm -rf $(BUILD_DIR)