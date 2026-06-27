#
# nofrendo (NES emulator core) component.
#
# Legacy 1998-era C with the composite-native osd.c (nes_emulate_init/frame).
# -Ofast for the CPU/PPU hot paths; -Wno-error so the old code's warnings don't
# fail the build under IDF's -Werror flags; type-punning is everywhere here.
#
COMPONENT_SRCDIRS := .
COMPONENT_ADD_INCLUDEDIRS := .

CFLAGS   += -Ofast -fno-strict-aliasing -w
CPPFLAGS += -Ofast -fno-strict-aliasing -w
