// A case-sensitive fake filesystem makes the real iOS resolver testable on Windows as well.
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include "ios/src/fs_case_ios.cpp"

std::string g_game_root = "/game";
static std::set<std::string> paths;
static size_t probes = 0, listings = 0;
void log_write(const char*, ...) {}
int access(const char* p, int) { ++probes; return paths.count(p) ? 0 : -1; }
int strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        const auto x = std::tolower(static_cast<unsigned char>(*a++));
        const auto y = std::tolower(static_cast<unsigned char>(*b++));
        if (x != y) return x - y;
    }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}
DIR* opendir(const char* name) {
    ++listings;
    if (!paths.count(name)) return nullptr;
    auto* dir = new DIR;
    const std::string prefix = std::string(name) + "/";
    for (const auto& p : paths) {
        if (p.compare(0, prefix.size(), prefix)) continue;
        auto leaf = p.substr(prefix.size());
        if (!leaf.empty() && leaf.find('/') == std::string::npos) dir->names.push_back(leaf);
    }
    return dir;
}
dirent* readdir(DIR* d) {
    if (d->pos == d->names.size()) return nullptr;
    std::strcpy(d->entry.d_name, d->names[d->pos++].c_str());
    return &d->entry;
}
int closedir(DIR* d) { delete d; return 0; }

int main() {
    paths = {"/game", "/game/content", "/game/content/Actor", "/game/content/Actor/Link.pack"};
    const std::string exact = "/game/content/Actor/Link.pack", mixed = "/game/content/actor/link.pack";
    assert(botw_ios_fix_case(exact) == exact);
    auto before = probes;
    for (int i = 0; i < 10000; ++i) assert(botw_ios_fix_case(exact) == exact);
    assert(probes == before);
    assert(botw_ios_fix_case(mixed) == exact);
    before = probes;
    auto listed = listings;
    for (int i = 0; i < 10000; ++i) assert(botw_ios_fix_case(mixed) == exact);
    assert(probes == before && listings == listed);
    std::puts("filesystem: 20,000 warm exact/mixed-case lookups used 0 access calls and 0 directory reads");

    // Missing files are not cached: a subsequent exact-name addition is immediately visible.
    const std::string missing = "/game/content/Actor/new.pack";
    assert(botw_ios_fix_case(missing) == missing);
    before = probes;
    paths.insert(missing);
    assert(botw_ios_fix_case(missing) == missing && probes > before);
    assert(g_paths.count(missing));
    before = probes;
    assert(botw_ios_fix_case("/game-other/content/link.pack") == "/game-other/content/link.pack");
    assert(botw_ios_fix_case("/saves/link.pack") == "/saves/link.pack");
    assert(probes == before);

    for (int i = 0; i < 1000; ++i) {
        auto p = "/game/content/Actor/" + std::to_string(i) + ".pack";
        paths.insert(p);
        assert(botw_ios_fix_case(p) == p);
        assert(g_paths.size() <= kPathCacheLimit);
    }
    // Each thread has its own bounded cache, independent of the main thread's entries.
    before = probes;
    std::thread worker([&] {
        assert(g_paths.empty());
        assert(botw_ios_fix_case(exact) == exact);
        auto warm = probes;
        assert(botw_ios_fix_case(exact) == exact && probes == warm);
    });
    worker.join();
    assert(probes == before + 1);
    std::puts("filesystem: missing files, directory boundary, cache bound and thread isolation passed");
}
