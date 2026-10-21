#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

bool g_mute = false, g_headless = false;
#include "runtime/src/log.cpp"
#include "runtime/src/host/lifecycle.cpp"
#include "runtime/src/audio/output.cpp"

using namespace std::chrono_literals;
std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path); return {std::istreambuf_iterator<char>(stream), {}};
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::filesystem::path dir(argv[1]);
    std::filesystem::create_directories(dir / "export");
#ifdef _WIN32
    _putenv_s("BOTW_LOG_EXPORT_DIR", (dir / "export").string().c_str());
#else
    setenv("BOTW_LOG_EXPORT_DIR", (dir / "export").c_str(), 1);
#endif
    auto path = (dir / "runtime.log").string();
    log_set_file(path.c_str()); LOG("session one");
    log_set_file(path.c_str()); LOG("session two");
    assert(read(dir / "runtime.log.1").find("session one") != std::string::npos);
    assert(read(dir / "export/runtime.log.1").find("session one") != std::string::npos);
    assert(read(dir / "runtime.log").find("session two") != std::string::npos);
    log_emergency(11, 0x1234);
    assert(read(dir / "runtime.log").find("FATAL SIGNAL") != std::string::npos);
    assert(read(dir / "export/runtime.log").find("1234") != std::string::npos);

    std::atomic<int> drains{0};
    lifecycle::set_gpu_drain(&drains, [](void* value) { ++*static_cast<std::atomic<int>*>(value); });
    lifecycle::set_active(false, "test Control Center");
    assert(drains == 1);
    std::promise<void> entered;
    auto completed = std::async(std::launch::async, [&] {
        entered.set_value();
        auto guard = lifecycle::lock_gpu_submission();
        return 42;
    });
    entered.get_future().wait();
    assert(completed.wait_for(80ms) == std::future_status::timeout);
    lifecycle::set_active(true, "test return");
    assert(completed.get() == 42);
    assert(lifecycle::generation() == 2);
    // An already-started submission must finish before the inactive notification returns.
    auto held = lifecycle::lock_gpu_submission();
    std::promise<void> pausing;
    auto pause = std::async(std::launch::async, [&] { pausing.set_value(); lifecycle::set_active(false, "submission race"); });
    pausing.get_future().wait();
    assert(pause.wait_for(80ms) == std::future_status::timeout);
    held.unlock(); pause.get();
    assert(!lifecycle::active() && drains == 2);
    lifecycle::set_active(true, "test resume");
    lifecycle::set_gpu_drain(nullptr, nullptr);

    audio::init();
    int16_t stereo[288]; std::fill_n(stereo, 288, 10000);
    for (int i = 0; i < 19; ++i) audio::push_tv(stereo, 144, 2);
    assert(fake_audio::resumes == 0); // never play an empty / tiny initial buffer
    audio::push_tv(stereo, 144, 2); assert(fake_audio::resumes == 1);
    audio::set_app_active(false);
    audio::push_tv(stereo, 144, 2); assert(fake_audio::queue.empty());
    audio::set_app_active(true);
    for (int i = 0; i < 19; ++i) audio::push_tv(stereo, 144, 2);
    assert(fake_audio::resumes == 1);
    audio::push_tv(stereo, 144, 2); assert(fake_audio::resumes == 2);
    audio::set_interrupted(true); audio::route_changed();
    audio::push_tv(stereo, 144, 2); assert(fake_audio::queue.empty());
    audio::set_interrupted(false); audio::set_volume(0.5f);
    audio::push_tv(stereo, 144, 2); assert(fake_audio::queue[0] == 5000);
    audio::set_volume(1); audio::set_interrupted(false);
    int16_t surround[6] = {30000,30000,30000,0,30000,30000};
    audio::push_tv(surround, 1, 6); assert(fake_audio::queue[0] == 32767);
    audio::set_interrupted(false);
    for (int i = 0; i < 150; ++i) {
        audio::push_tv(stereo, 144, 2);
        assert(audio::queued_frames() <= 12144);
    }
    int clears = fake_audio::clears;
    std::this_thread::sleep_for(90ms);
    audio::push_tv(stereo, 144, 2);
    assert(fake_audio::clears > clears && audio::queued_frames() == 144);
    std::cout << "GPU pause/submission races, audio priming, interruption recovery, clipping, queue bounds and stale audio passed.\n";

    std::string long_line(9000, 'x');
    for (int i = 0; i < 2200; ++i) LOG("%s", long_line.c_str());
    assert(std::filesystem::file_size(dir / "runtime.log") <= 16 * 1024 * 1024);
    assert(std::filesystem::file_size(dir / "runtime.log.1") <= 16 * 1024 * 1024);
    assert(read(dir / "runtime.log.1").find("TRUNCATED") != std::string::npos);
    assert(std::filesystem::exists(dir / "runtime.log.2"));
    std::cout << "Log archives, export mirror, emergency records, truncation and 16 MiB rotation passed.\n";
}
