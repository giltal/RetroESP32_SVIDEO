CFLAGS += -DGNUBOY_NO_MINIZIP -DGNUBOY_NO_SCREENSHOT -DIS_LITTLE_ENDIAN -Wno-implicit-function-declaration
# gnuboy CPU/LCD hot paths -> -O2 (global opt is -Os, was -Og).
CFLAGS += -O2 -fno-strict-aliasing
COMPONENT_ADD_INCLUDEDIRS := .
COMPONENT_OBJEXCLUDE := main.o
