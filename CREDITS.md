# Credits & Licenses

**RetroESP32 S-Video** is the integration/firmware work of **Gil Tal** (© 2026), distributed under
the **GNU General Public License v2** (see [`LICENSE`](LICENSE)). The project is GPLv2 because it
bundles and links the GPLv2 emulator cores listed below; each bundled component remains under its
own license.

## This project
- Composite-video HAL glue, launcher carousel + live video calibration, the factory/OTA app split,
  and the per-core composite drivers (Atari 800/2600, Game Boy) — © 2026 Gil Tal, **GPL v2**.
- Built on the **RetroESP32 / retro-go** firmware (ODROID-GO lineage) by ducalex and contributors.

## Bundled emulator cores
| Component | System | Author / Project | License |
|-----------|--------|------------------|---------|
| `nofrendo` | NES | Matthew Conte | LGPL v2 |
| `smsplus` (SMS Plus) | Sega Master System / Game Gear | Charles MacDonald | GPL v2 |
| `gnuboy` | Game Boy / Color | the gnuboy authors | GPL v2 |
| `atari800` | Atari 800 / 5200 | the Atari800 Development Team | GPL v2 |
| `stella` | Atari 2600 | the Stella Team | GPL v2 |

Some cores keep their own license file in-tree (e.g. `atari/components/stella/license.txt`,
`launcher/components/smsplus/LICENSE`).

## Composite video
- `components/odroid/video_out.h` — NTSC/PAL composite signal generator derived from **ESP_8_BIT**,
  © 2020 **Peter Barrett** — **ISC** license (permissive; see the header in the file).

## Not included
- **No ROMs or BIOS files** are distributed with this project. Bring your own; game and BIOS images
  remain the property of their respective copyright holders.
