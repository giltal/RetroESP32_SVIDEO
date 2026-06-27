#
# "main" component makefile.
#
# Uses default behaviour: compiles every source file in this directory and adds
# this directory to the include path.
#
# -Ofast is REQUIRED here: the composite video ISR + blit are timing-critical and
# the original Arduino build used -Ofast. -fno-strict-aliasing protects the 32-bit
# word reads over byte framebuffers in blit().

CFLAGS   += -Ofast -fno-strict-aliasing
CPPFLAGS += -Ofast
CXXFLAGS += -Ofast -fno-strict-aliasing
