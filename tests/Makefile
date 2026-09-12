# Unit test build for the telemetry (cJSON) parser.
#
# These test needs to build and run
# natively on the host, so CC/CFLAGS/LDFLAGS are all forced with
# `override` here -- this ignores any CC/CFLAGS/LDFLAGS the SDK setup
# script may have exported into the shell's environment.
#
# Usage (from repo root or from tests/):
#   make -C tests            # build + run once
#   make -C tests test       # same
#   make -C tests clean

override CC      := gcc
override CFLAGS  := -Wall -Wextra -std=gnu11 -I../include -I../vendor/cjson
override LDFLAGS :=
override LDLIBS  := -lm

ROOT_DIR  := ..
BUILD_DIR := build
TARGET    := test_telemetry

SRCS := test_telemetry.c \
        $(ROOT_DIR)/src/telemetry.c \
        $(ROOT_DIR)/vendor/cjson/cJSON.c

.PHONY: all test clean

all: test

test: $(TARGET)
	./$(TARGET)

$(TARGET): $(SRCS) $(ROOT_DIR)/include/telemetry.h $(ROOT_DIR)/vendor/cjson/cJSON.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)
