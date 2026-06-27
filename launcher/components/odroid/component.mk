#
# Component Makefile
#
# This Makefile can be left empty. By default, it will take the sources in the
# src/ directory, compile them and link them into lib(subdirectory_name).a
# in the build directory. This behaviour is entirely configurable,
# please read the ESP-IDF documents if you need to do this.
#

#COMPONENT_DEPENDS :=
#COMPONENT_SRCDIRS := .

# Export this dir so other components (main/nes_run.cpp) can #include "composite_video.h"
# and the odroid_*.h headers by name.
COMPONENT_ADD_INCLUDEDIRS := .

# The composite video bridge (odroid_display.cpp -> video_out.h) carries the
# timing-critical NTSC ISR/blit. Build this component -Ofast like the 1a driver,
# and keep type-punning safe (blit reads 32-bit words from byte framebuffers).
CFLAGS   += -Ofast -fno-strict-aliasing
CPPFLAGS += -Ofast
CXXFLAGS += -Ofast -fno-strict-aliasing
