#pragma once
// Composite-pump control shared between the launcher display bridge and emulators.
// Implemented in odroid_display.cpp (the only TU that includes video_out.h).
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Switch the composite pump to NES output: EMU_NES geometry + a 64-entry NES
// composite palette, displaying `lines` (240 pointers, 256 px of 6-bit indices each).
void composite_use_nes(uint8_t** lines, const uint32_t* palette);

// Switch the composite pump to SMS output: EMU_SMS geometry (full 8-bit index) +
// a 256-entry composite palette, displaying `lines` (240 pointers, 256 px each).
void composite_use_sms(uint8_t** lines, const uint32_t* palette);

// Switch the composite pump to Atari output: EMU_ATARI geometry (2 px/color-clock,
// 384-wide rows, center 336 shown) + a 256-entry composite palette.
void composite_use_atari(uint8_t** lines, const uint32_t* palette);

// Restore the launcher's display (EMU_SMS + 3-3-2 palette + launcher framebuffer).
void composite_use_launcher(void);

// Block until vertical blanking (frame pacing / tear avoidance).
void composite_wait_vsync(void);

// Samples queued in the audio ring. Emulators pace on this (drain-rate pacing) so a frame
// that overruns the 16.6ms budget degrades gracefully instead of halving to 30fps.
int composite_audio_pending(void);

// Idle the composite pump so an emulator can repurpose the shared screen buffer with its
// own stride. Nothing is freed (the buffer is static); the launcher reclaims it via
// composite_use_launcher(). Kept for the launcher->emulator handoff.
void composite_release_launcher_fb(void);

// The single shared composite screen buffer (static internal, 384*240 bytes). Used by the
// launcher and every emulator as their 8-bit indexed framebuffer - no per-core alloc.
// Address it with the emulator's own stride (256 for NES/SMS/launcher, 384 for Atari).
uint8_t* composite_shared_screen(void);

// Queue 16-bit mono samples for the LEDC audio output (video_out's per-scanline
// audio path on AUDIO_PIN). Emulators call this once per frame.
void composite_audio_write(const int16_t* samples, int count);

// Encode an 8-bit RGB color into a composite palette word (for emulators whose
// palette changes at runtime, e.g. SMS).
unsigned int composite_encode_rgb(int r, int g, int b);

// Composite color calibration (saturation/hue/brightness/contrast). All palette encoding
// goes through composite_encode_rgb, so these apply to the launcher and every emulator.
// The launcher persists them in NVS and offers a live Video Calibration screen.
void composite_set_color_params(float chroma, float phase, float bright, float contrast);
void composite_get_color_params(float* chroma, float* phase, float* bright, float* contrast);
// Rebuild the launcher 3-3-2 palette from the current params (call after a live change).
void composite_rebuild_palette(void);

#ifdef __cplusplus
}
#endif
