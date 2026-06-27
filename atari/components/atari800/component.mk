#
# Component Makefile for atari800 emulator core
#
# All .cpp source files in this directory are compiled.
# The ESP-IDF build system automatically finds them.
#

# Exceptions are enabled globally (for the Stella/2600 core), but they add per-function unwinding
# overhead that slows this hot 6502 core -> unstable speed + shake. Force -fno-exceptions here so
# the atari800 codegen matches the fast pre-exceptions build.
CXXFLAGS += -O2 -fno-exceptions -Wno-unused-variable -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-format -Wno-sign-compare
CFLAGS += -O2 -Wno-unused-variable -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-format -Wno-sign-compare

# Standalone Atari app: no smsplus/nofrendo to collide with, so export headers normally.
COMPONENT_ADD_INCLUDEDIRS := .
COMPONENT_SRCDIRS := .

# Exclude files not needed for ESP-IDF build:
# rdevice.cpp - R: device serial/network emulation (requires termios.h / sockets not available)
COMPONENT_OBJEXCLUDE := rdevice.o
