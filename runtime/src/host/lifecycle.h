#pragma once
#include <cstdint>
#include <mutex>

// Inactive includes Control Center / Notification Center, not just background.
namespace lifecycle {
bool active();
void wait_until_active();
void set_active(bool active, const char* reason);
std::unique_lock<std::mutex> lock_gpu_submission();
// Called with the submission lock held; only drain already committed work.
void set_gpu_drain(void* context, void (*drain)(void*));
uint64_t generation();
}
