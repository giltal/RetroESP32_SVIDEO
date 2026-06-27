#
# Main component
#
COMPONENT_ADD_INCLUDEDIRS := .

# main.cpp owns video_out.h (composite ISR + blit) -- timing critical. Global opt is RELEASE (-Os);
# force -O2 here. -fno-strict-aliasing for the 32-bit word reads over byte framebuffers in blit().
CFLAGS   += -O2 -fno-strict-aliasing
CXXFLAGS += -O2 -fno-strict-aliasing
