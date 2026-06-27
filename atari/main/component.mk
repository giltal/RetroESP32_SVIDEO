#
# Main component
#
COMPONENT_ADD_INCLUDEDIRS := .

# main.cpp owns video_out.h (the composite ISR + blit) -- timing critical. The global opt level is
# RELEASE (-Os); force -O2 here so the composite path runs fast (avoids choppy video/audio).
CFLAGS   += -O2 -fno-exceptions
CXXFLAGS += -O2 -fno-exceptions
