#include "log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <functional>
#include <chrono>
#include <unistd.h>

int g_log_level = 0;
namespace {
std::recursive_mutex lock;
FILE* files[2]{};
std::string paths[2];
size_t sizes[2]{};
volatile sig_atomic_t emergency_fds[2] = {-1, -1};
constexpr size_t max_bytes = 16 * 1024 * 1024;
constexpr int archives = 4;
uint64_t sequence = 0;
void rotate(const std::string& path) {
    std::remove((path + "." + std::to_string(archives)).c_str());
    for (int i = archives - 1; i >= 1; --i)
        std::rename((path + "." + std::to_string(i)).c_str(), (path + "." + std::to_string(i + 1)).c_str());
    std::rename(path.c_str(), (path + ".1").c_str());
}
void open_file(int i) {
    emergency_fds[i] = -1;
    if (files[i]) fclose(files[i]);
    rotate(paths[i]);
    files[i] = fopen(paths[i].c_str(), "a");
    sizes[i] = 0;
    if (files[i]) {
        setvbuf(files[i], nullptr, _IONBF, 0);
        emergency_fds[i] = fileno(files[i]);
    } else fprintf(stderr, "Unable to open diagnostic log: %s\n", paths[i].c_str());
}
}

void log_set_file(const char* path) {
    std::lock_guard guard(lock);
    paths[0] = path;
    open_file(0);
    if (const char* directory = getenv("BOTW_LOG_EXPORT_DIR"); directory && *directory) {
        paths[1] = std::string(directory) + "/runtime.log";
        if (paths[1] != paths[0]) open_file(1);
    }
    time_t now = time(nullptr);
    char wall[64]{};
    if (auto tm = gmtime(&now)) strftime(wall, sizeof wall, "%Y-%m-%dT%H:%M:%SZ", tm);
    LOG("diagnostics: session UTC %s, PID %d, format v2; max 16 MiB/file, 4 previous files", wall, (int)getpid());
}

void log_write(const char* fmt, ...) {
    char message[8192];
    va_list ap;
    va_start(ap, fmt);
    int count = vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    static const double start = now;
    std::lock_guard guard(lock);
    char line[8500];
    int n = snprintf(line, sizeof line, "[%10.6f] #%llu t%zx %s%s\n", now - start,
                     (unsigned long long)++sequence, std::hash<std::thread::id>{}(std::this_thread::get_id()),
                     message, count >= (int)sizeof message ? " [TRUNCATED]" : "");
    size_t bytes = n > 0 ? std::min((size_t)n, sizeof line - 1) : 0;
    fwrite(line, 1, bytes, stderr);
    for (int i = 0; i < 2; ++i) {
        if (!files[i]) continue;
        if (sizes[i] + bytes > max_bytes) open_file(i);
        if (files[i]) { fwrite(line, 1, bytes, files[i]); sizes[i] += bytes; }
    }
}

void log_emergency(int sig, uintptr_t address) {
    char buffer[128];
    size_t n = 0;
    const char prefix[] = "FATAL SIGNAL: signal=0x";
    for (char c : prefix) if (c) buffer[n++] = c;
    auto hex = [&](uintptr_t value) {
        const char digits[] = "0123456789abcdef";
        for (int shift = (int)(sizeof(uintptr_t) * 8) - 4; shift >= 0; shift -= 4)
            buffer[n++] = digits[(value >> shift) & 15];
    };
    hex((uintptr_t)sig);
    const char middle[] = " fault_address=0x";
    for (char c : middle) if (c) buffer[n++] = c;
    hex(address);
    buffer[n++] = '\n';
    (void)!write(STDERR_FILENO, buffer, n);
    for (int i = 0; i < 2; ++i) {
        int fd = emergency_fds[i];
        if (fd >= 0) (void)!write(fd, buffer, n);
    }
}
