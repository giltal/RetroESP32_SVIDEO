#
# Component Makefile
#
# This Makefile can be left empty. By default, it will take the sources in the
# src/ directory, compile them and link them into lib(subdirectory_name).a
# in the build directory. This behaviour is entirely configurable,
# please read the ESP-IDF documents if you need to do this.
#

COMPONENT_ADD_INCLUDEDIRS := . ./cpu ./sound
COMPONENT_SRCDIRS := . cpu sound

# -Ofast for the Z80/VDP hot paths; -w because this 1998-era core has many warnings
# that would trip IDF's -Werror.
CFLAGS += -DLSB_FIRST=1 -Ofast -fno-strict-aliasing -w
