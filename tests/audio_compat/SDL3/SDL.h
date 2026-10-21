#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

struct SDL_AudioStream {};
struct SDL_AudioSpec { int format, channels, freq; };
constexpr int SDL_AUDIO_S16 = 1, SDL_INIT_AUDIO = 1, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK = 1;
constexpr const char* SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES = "samples";
namespace fake_audio {
inline SDL_AudioStream stream;
inline std::vector<int16_t> queue;
inline int resumes = 0, pauses = 0, clears = 0;
inline void (*callback)(void*, SDL_AudioStream*, int, int) = nullptr;
}
inline bool SDL_SetHint(const char*, const char*) { return true; }
inline bool SDL_InitSubSystem(int) { return true; }
inline const char* SDL_GetError() { return "fake SDL error"; }
inline SDL_AudioStream* SDL_OpenAudioDeviceStream(int, SDL_AudioSpec*, void*, void*) { return &fake_audio::stream; }
inline bool SDL_SetAudioStreamGetCallback(SDL_AudioStream*, void (*cb)(void*, SDL_AudioStream*, int, int), void*) { fake_audio::callback = cb; return true; }
inline int SDL_GetAudioStreamDevice(SDL_AudioStream*) { return 1; }
inline bool SDL_GetAudioDeviceFormat(int, SDL_AudioSpec* spec, int* samples) { *spec = {1,2,48000}; *samples = 1024; return true; }
inline bool SDL_ResumeAudioStreamDevice(SDL_AudioStream*) { ++fake_audio::resumes; return true; }
inline bool SDL_PauseAudioStreamDevice(SDL_AudioStream*) { ++fake_audio::pauses; return true; }
inline bool SDL_ClearAudioStream(SDL_AudioStream*) { ++fake_audio::clears; fake_audio::queue.clear(); return true; }
inline int SDL_GetAudioStreamQueued(SDL_AudioStream*) { return (int)fake_audio::queue.size() * 2; }
inline bool SDL_PutAudioStreamData(SDL_AudioStream*, const void* data, int size) {
    auto samples = static_cast<const int16_t*>(data);
    fake_audio::queue.insert(fake_audio::queue.end(), samples, samples + size / 2); return true;
}
