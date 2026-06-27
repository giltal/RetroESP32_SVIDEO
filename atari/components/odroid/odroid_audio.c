// odroid_audio.c - STUB for RetroESP32_SVIDEO.
//
// The original odroid audio driver installs the I2S0 built-in-DAC engine, which is the
// SAME peripheral our composite video pump (video_out.h) owns. Letting it init would
// destroy the video signal. So audio is a no-op for now; composite audio will later go
// through video_out's per-scanline LEDC path, not I2S0.

#include "odroid_audio.h"

static odroid_volume_level s_volume = ODROID_VOLUME_LEVEL4;
static int s_sample_rate = 16000;

odroid_volume_level odroid_audio_volume_get() { return s_volume; }
void odroid_audio_volume_set(odroid_volume_level value) { s_volume = value; }
void odroid_audio_volume_change() {
    s_volume = (odroid_volume_level)((s_volume + 1) % ODROID_VOLUME_LEVEL_COUNT);
}
void odroid_audio_init(int sample_rate) { s_sample_rate = sample_rate; }
void odroid_audio_terminate() {}
void odroid_audio_submit(short* stereoAudioBuffer, int frameCount) {
    (void)stereoAudioBuffer; (void)frameCount;
}
int odroid_audio_sample_rate_get() { return s_sample_rate; }
