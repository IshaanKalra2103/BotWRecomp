#include "lifecycle.h"
#include "../log.h"
#include <atomic>
#include <condition_variable>

namespace lifecycle {
namespace {
std::mutex mutex;
std::condition_variable changed;
std::atomic<bool> running{true};
std::atomic<uint64_t> epoch{0};
void* gpu_context = nullptr;
void (*gpu_drain)(void*) = nullptr;
}
bool active() { return running.load(std::memory_order_acquire); }
uint64_t generation() { return epoch.load(); }
void wait_until_active() {
    if (active()) return;
    std::unique_lock lock(mutex);
    changed.wait(lock, [] { return active(); });
}
std::unique_lock<std::mutex> lock_gpu_submission() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [] { return active(); });
    return lock;
}
void set_gpu_drain(void* context, void (*drain)(void*)) {
    std::lock_guard lock(mutex);
    gpu_context = context;
    gpu_drain = drain;
}
void set_active(bool value, const char* reason) {
    {
        std::unique_lock lock(mutex);
        if (running == value) return;
        running.store(value, std::memory_order_release);
        ++epoch;
        if (!value && gpu_drain) gpu_drain(gpu_context);
    }
    changed.notify_all();
    LOG("lifecycle: %s, generation %llu, reason %s", value ? "active" : "inactive",
        (unsigned long long)generation(), reason);
}
}
