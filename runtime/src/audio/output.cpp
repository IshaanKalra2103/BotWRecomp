#include "output.h"

#include <SDL3/SDL.h>
#include <math.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>

#include "../log.h"
#include "../host/lifecycle.h"

extern bool g_mute;
extern bool g_headless;

namespace audio {
namespace {
std::once_flag g_once;
std::mutex g_lock;
SDL_AudioStream* g_stream = nullptr;
std::vector<int16_t> g_stereo;
uint64_t g_frames_total = 0;
double g_sum_sq = 0;
uint64_t g_level_frames = 0;
bool g_app_active = true, g_interrupted = false;
std::atomic<bool> g_started{false};
std::atomic<uint64_t> g_shortage_requests{0};
uint64_t g_resets = 0, g_clipped = 0;
constexpr int kPrimeFrames = 2880; // 60 ms, enough for several iOS hardware callbacks
constexpr int kMaxFrames = 12000; // discard stale audio instead of growing latency without limit
using Clock = std::chrono::steady_clock;
Clock::time_point g_last_push{}, g_last_report = Clock::now();
double g_max_gap_ms = 0;

void reset_locked(const char* reason) {
    g_started = false;
    g_last_push = {};
    if (g_stream) {
        if (!SDL_PauseAudioStreamDevice(g_stream)) LOG("audio: pause failed: %s", SDL_GetError());
        if (!SDL_ClearAudioStream(g_stream)) LOG("audio: clear failed: %s", SDL_GetError());
    }
    ++g_resets;
    LOG("audio: queue reset (%s), app_active=%d interrupted=%d", reason, g_app_active, g_interrupted);
}
}  // namespace

void init() {
    std::call_once(g_once, [] {
        std::lock_guard guard(g_lock);
        if (g_headless || g_mute) {
            LOG("audio: muted");
            return;
        }
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            LOG("audio: SDL audio init failed: %s", SDL_GetError());
            return;
        }
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!g_stream) {
            LOG("audio: no output device: %s", SDL_GetError());
            return;
        }
        SDL_SetAudioStreamGetCallback(g_stream, [](void*, SDL_AudioStream*, int additional, int) {
            // This runs under SDL's stream lock. Never acquire g_lock or write logs here.
            if (additional > 0 && g_started.load()) ++g_shortage_requests;
        }, nullptr);
        SDL_AudioSpec hardware{};
        int samples = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_stream), &hardware, &samples);
        LOG("audio: source 48000 Hz stereo; device %d Hz %d channels %d frames; prime 60 ms, maximum queue 250 ms",
            hardware.freq, hardware.channels, samples);
    });
}

std::atomic<float> g_volume{1.0f};
void set_volume(float v) { g_volume = std::clamp(v, 0.0f, 1.0f); }

void set_app_active(bool active) {
    std::lock_guard guard(g_lock);
    if (g_app_active == active) return;
    g_app_active = active;
    reset_locked(active ? "foreground" : "inactive");
}
void set_interrupted(bool interrupted) {
    std::lock_guard guard(g_lock);
    g_interrupted = interrupted;
    reset_locked(interrupted ? "session interruption" : "session resumed / route changed");
}
void route_changed() {
    std::lock_guard guard(g_lock);
    reset_locked("output route changed");
}

void push_tv(const int16_t* in, int frames, int channels) {
    std::lock_guard guard(g_lock);
    if (!in || frames <= 0 || (channels != 1 && channels != 2 && channels != 6)) {
        LOG("audio: invalid input frames=%d channels=%d", frames, channels);
        return;
    }
    if (!g_app_active || g_interrupted || !lifecycle::active()) return;
    auto now = Clock::now();
    if (g_last_push != Clock::time_point{}) {
        double gap = std::chrono::duration<double, std::milli>(now - g_last_push).count();
        g_max_gap_ms = std::max(g_max_gap_ms, gap);
        if (gap > 80 && g_started) reset_locked("producer gap >80 ms");
    }
    g_last_push = now;
    if (g_stream && SDL_GetAudioStreamQueued(g_stream) / 4 > kMaxFrames)
        reset_locked("queue >250 ms");
    const float vol = g_volume;
    g_stereo.resize((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        int l, r;
        if (channels >= 6) {
            // L R C LFE Ls Rs -> stereo
            const int16_t* s = in + i * channels;
            l = (int)(s[0] + 0.707f * s[2] + 0.707f * s[4]);
            r = (int)(s[1] + 0.707f * s[2] + 0.707f * s[5]);
        } else if (channels == 2) {
            l = in[i * 2];
            r = in[i * 2 + 1];
        } else {
            l = r = in[i * channels];
        }
        l = (int)(l * vol); r = (int)(r * vol);
        g_clipped += (l < -32768 || l > 32767) + (r < -32768 || r > 32767);
        g_stereo[i * 2] = (int16_t)std::clamp(l, -32768, 32767);
        g_stereo[i * 2 + 1] = (int16_t)std::clamp(r, -32768, 32767);
        g_sum_sq += (double)g_stereo[i * 2] * g_stereo[i * 2] +
                    (double)g_stereo[i * 2 + 1] * g_stereo[i * 2 + 1];
    }
    g_frames_total += frames;
    g_level_frames += frames;
    if (g_stream) {
        if (!SDL_PutAudioStreamData(g_stream, g_stereo.data(), frames * 4))
            LOG("audio: submit failed: %s", SDL_GetError());
        if (!g_started && SDL_GetAudioStreamQueued(g_stream) / 4 >= kPrimeFrames) {
            if (SDL_ResumeAudioStreamDevice(g_stream)) g_started = true;
            else LOG("audio: resume failed: %s", SDL_GetError());
        }
    }
    if (now - g_last_report >= std::chrono::seconds(2)) {
        double rms = sqrt(g_sum_sq / std::max<uint64_t>(1, g_level_frames * 2)) / 32768.0;
        int queued = g_stream ? SDL_GetAudioStreamQueued(g_stream) / 4 : 0;
        LOG("audio: level=%.1f dBFS queued=%.1f ms max_producer_gap=%.3f ms shortage_requests=%llu resets=%llu clipped=%llu started=%d",
            rms > 0 ? 20 * log10(rms) : -120.0, queued / 48.0, g_max_gap_ms,
            (unsigned long long)g_shortage_requests.exchange(0), (unsigned long long)g_resets,
            (unsigned long long)g_clipped, (int)g_started.load());
        g_level_frames = 0;
        g_sum_sq = 0;
        g_clipped = 0;
        g_max_gap_ms = 0;
        g_last_report = now;
    }
}

int queued_frames() {
    std::lock_guard guard(g_lock);
    return g_stream ? SDL_GetAudioStreamQueued(g_stream) / 4 : 0;
}
}  // namespace audio
