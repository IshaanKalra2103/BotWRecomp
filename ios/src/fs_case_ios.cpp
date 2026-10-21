// iOS file systems are case-sensitive; the Wii U's (and the default macOS volume) are not. When a
// game path does not exist as spelled, resolve it component by component ignoring case, so a guest
// request like "Actor/Pack/foo.sbactorpack" still finds "Actor/Pack/Foo.sbactorpack". Only paths
// inside the read-only game folder are touched (saves are created by the runtime itself).
// Called from runtime/src/coreinit/fs.cpp host_path() under TARGET_OS_IOS.
#include <dirent.h>
#include <strings.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "os.h"

namespace {
std::mutex g_lock;
std::map<std::string, std::vector<std::string>> g_dirs;  // directory -> entry names (game folder only)
std::atomic<int> g_logged{0};
// The game directory is read-only during a run. Cache successful resolutions only, so missing
// files can still appear later. Per-thread storage avoids a shared lock on repeated file opens.
constexpr size_t kPathCacheLimit = 256;
thread_local std::unordered_map<std::string, std::string> g_paths;

const std::vector<std::string>& list_dir(const std::string& dir) {
    auto it = g_dirs.find(dir);
    if (it != g_dirs.end()) return it->second;
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) names.emplace_back(e->d_name);
        closedir(d);
    }
    return g_dirs.emplace(dir, std::move(names)).first->second;
}
}  // namespace

std::string botw_ios_fix_case(const std::string& host) {
    const std::string& root = g_game_root;
    if (root.empty() || host.compare(0, root.size(), root) != 0 ||
        (host.size() > root.size() && host[root.size()] != '/')) return host;
    if (auto it = g_paths.find(host); it != g_paths.end()) return it->second;
    auto remember = [&](const std::string& resolved) {
        if (g_paths.size() >= kPathCacheLimit) g_paths.clear();
        g_paths.emplace(host, resolved);
        return resolved;
    };
    if (access(host.c_str(), F_OK) == 0) return remember(host);
    std::lock_guard<std::mutex> l(g_lock);
    std::string cur = root;
    size_t i = root.size();
    while (i < host.size()) {
        if (host[i] == '/') {
            i++;
            continue;
        }
        size_t j = host.find('/', i);
        std::string comp = host.substr(i, j == std::string::npos ? std::string::npos : j - i);
        std::string next = cur + "/" + comp;
        if (access(next.c_str(), F_OK) != 0) {
            const std::string* hit = nullptr;
            for (auto& n : list_dir(cur))
                if (strcasecmp(n.c_str(), comp.c_str()) == 0) {
                    hit = &n;
                    break;
                }
            if (!hit) return host;  // really missing: keep the guest's spelling for the error path
            next = cur + "/" + *hit;
        }
        cur = std::move(next);
        if (j == std::string::npos) break;
        i = j;
    }
    if (g_logged.fetch_add(1) < 32) LOG("ios: case-insensitive match %s -> %s", host.c_str(), cur.c_str());
    return remember(cur);
}
