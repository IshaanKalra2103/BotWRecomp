// Host audio output for the TV (and optionally GamePad) mix produced by snd_core (AX).
// Interleaved signed 16-bit samples at 48 kHz. In headless/muted runs nothing is played,
// but push() still accepts data (and reports levels to the log occasionally).
#pragma once
#include <stdint.h>

namespace audio {
void init();  // safe to call more than once
// channels: 2 or 6 (6 is downmixed to stereo). frames = samples per channel.
void push_tv(const int16_t* interleaved, int frames, int channels);
int queued_frames();  // frames waiting in the host device queue (for pacing)
void set_volume(float v);  // 0..1
void set_app_active(bool active);
void set_interrupted(bool interrupted); // AVAudioSession / output route interruptions
void route_changed(); // reset stale audio without clearing a still-active interruption
}  // namespace audio
