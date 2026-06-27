#
# Atari app odroid HAL. Export headers (video_out.h, odroid_*.h) on the include path,
# and exclude the launcher composite bridge (odroid_display.cpp) - it includes video_out.h
# (which has definitions) and main.cpp includes it directly, so both would duplicate symbols.
#
COMPONENT_ADD_INCLUDEDIRS := .
COMPONENT_OBJEXCLUDE := odroid_display.o

# Composite video/audio HAL is timing critical -> -O2 (global opt is -Os).
CFLAGS   += -O2
CXXFLAGS += -O2
