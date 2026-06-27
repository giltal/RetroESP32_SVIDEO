#!/usr/bin/env python
# Flash + (optional) monitor for RetroESP32_GB.
#
# IMPORTANT: run with the NATIVE WINDOWS python, NOT msys64's cygwin python.
# The cygwin python's pyserial uses the POSIX backend (/dev/ttySN) which does
# NOT toggle DTR/RTS, so the ESP32 auto-reset never fires and esptool times out
# ("Failed to connect ... Timed out waiting for packet header").
# Native Windows pyserial (serial.serialwin32) drives RTS/DTR correctly, so the
# board resets into the bootloader on its own -- no BOOT/EN button presses.
#
# Usage (from Git Bash / cmd, Windows python on PATH):
#   python flash.py            # flash only      (default COM6)
#   python flash.py COM6       # flash on COM6
#   python flash.py COM6 mon   # flash, then print serial console (Ctrl-C to stop)
#
# Mirrors RetroESP32-master/flash_carousel.py but for this single-app build.

import subprocess, sys, time

PORT_ARG = sys.argv[1] if len(sys.argv) > 1 else 'COM6'
PORT = '//./' + PORT_ARG            # Windows device path form esptool/pyserial want
MONITOR = len(sys.argv) > 2 and sys.argv[2].startswith('mon')

ESPTOOL = r'C:\Users\97254\esp\v3.3.1\esp-idf\components\esptool_py\esptool\esptool.py'
BUILD   = r'C:\ESPIDFprojects\RetroESP32_GB\build'

flash_cmd = [
    sys.executable, ESPTOOL,
    '--chip', 'esp32', '--port', PORT, '--baud', '921600',
    '--before', 'default_reset', '--after', 'hard_reset',
    'write_flash', '-z', '--flash_mode', 'dio', '--flash_freq', '40m', '--flash_size', 'detect',
    '0x1000',  BUILD + r'\bootloader\bootloader.bin',
    '0x8000',  BUILD + r'\partitions.bin',
    '0x10000', BUILD + r'\RetroESP32_GB.bin',
]

print('Flashing RetroESP32_GB to', PORT, '...')
subprocess.run(flash_cmd, check=True)
print('Flash complete!')

if MONITOR:
    import serial
    print('--- serial console (115200), Ctrl-C to stop ---')
    s = serial.Serial(PORT, 115200, timeout=1)
    try:
        while True:
            data = s.read(512)
            if data:
                sys.stdout.write(data.decode('utf-8', 'replace'))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
