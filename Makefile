# Data Logging System — build
#
# Cross-compilation: source the Yocto SDK's environment-setup-* script
# before running make. That script exports CC (and CFLAGS/LDFLAGS) as the
# full cross-compiler invocation for the target sysroot; because `?=` only
# sets a variable when it is not already defined, and Make imports
# environment variables automatically, `make` picks up the cross toolchain
# with no changes to this file. Run `make` with no sourced SDK to build a
# native binary for local testing instead.

CC       ?= gcc
CFLAGS   += -Wall -Wextra -Werror -std=gnu11 -Iinclude -Ivendor/cjson -pthread
LDFLAGS  ?=
LDLIBS = -lserialport -pthread -lm

SRC_DIR    := src
INC_DIR    := include
VENDOR_DIR := vendor/cjson
BUILD_DIR  := build
TARGET     := data-logger

vpath %.c $(SRC_DIR) $(VENDOR_DIR)

SRCS := $(notdir $(wildcard $(SRC_DIR)/*.c)) $(notdir $(wildcard $(VENDOR_DIR)/*.c))
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)