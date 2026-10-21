// Exercise the real menu/touch implementations with a small, deterministic host API.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>

static size_t allocations = 0, settings_reads = 0;
void* operator new(size_t n) {
    ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

#ifndef BOTW_MENU_SOURCE
#define BOTW_MENU_SOURCE "runtime/src/host/menu.cpp"
#endif
#include BOTW_MENU_SOURCE
#ifndef BOTW_MENU_BENCH_ONLY
#include "runtime/src/host/touch.cpp"
#endif

float g_render_scale = 1;
static Settings options;
namespace settings {
Settings get() { ++settings_reads; return options; }
void set(const Settings& s, bool) { options = s; }
}

#ifndef BOTW_MENU_BENCH_ONLY
static uint64_t test_time = 1000000000;
static SDL_Window test_window;
uint64_t SDL_GetTicksNS() { return test_time += 16666667; }
float SDL_GetWindowPixelDensity(SDL_Window*) { return 3; }
uint32_t SDL_GetWindowID(SDL_Window*) { return 1; }
int SDL_GetTouchDeviceType(SDL_TouchID) { return SDL_TOUCH_DEVICE_DIRECT; }
bool SDL_PushEvent(SDL_Event*) { return true; }
void log_write(const char*, ...) {}
namespace input { PadState gamepad() { return {}; } }
namespace window {
void drawable_size(int* w, int* h) { *w = 2556; *h = 1179; }
SDL_Window* g_win_ptr() { return &test_window; }
}

static void finger(SDL_EventType type, SDL_FingerID id, float x, float y) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.touchID = 1;
    e.tfinger.fingerID = id;
    e.tfinger.x = x / 2556;
    e.tfinger.y = y / 1179;
    touch::event(e);
}

static void touch_tests() {
    touch::set_mode(1);
    input::PadState pad;
    touch::apply(pad);
    touch::DrawState snapshot;
    touch::draw_state(snapshot);
    assert(snapshot.visible && !snapshot.collapsed && snapshot.items.size() == 15);
    const auto* storage = snapshot.items.data();
    const auto capacity = snapshot.items.capacity();
    const auto before = allocations;
    for (int i = 0; i < 10000; ++i) touch::draw_state(snapshot);
    assert(allocations == before);
    assert(snapshot.items.data() == storage && snapshot.items.capacity() == capacity);

    // A press and release update both the input and the reused visual snapshot.
    const auto a = snapshot.items[0];
    finger(SDL_EVENT_FINGER_DOWN, 1, a.x, a.y);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(pad.buttons == input::BTN_A && snapshot.items[0].pressed);
    finger(SDL_EVENT_FINGER_UP, 1, a.x, a.y);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(!pad.buttons && !snapshot.items[0].pressed);

    // D-pad state must not leak into the next snapshot.
    const auto dpad = snapshot.items[12];
    finger(SDL_EVENT_FINGER_DOWN, 2, dpad.x, dpad.y - dpad.r * 0.6f);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(pad.buttons == input::BTN_UP && snapshot.items[12].dpad == input::BTN_UP);
    finger(SDL_EVENT_FINGER_UP, 2, dpad.x, dpad.y);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(!snapshot.items[12].pressed && !snapshot.items[12].dpad);

    // Stick/camera fields reset when their fingers lift.
    const float hx = snapshot.home_x, hy = snapshot.home_y;
    finger(SDL_EVENT_FINGER_DOWN, 3, hx, hy);
    finger(SDL_EVENT_FINGER_MOTION, 3, hx + 100, hy);
    finger(SDL_EVENT_FINGER_DOWN, 4, 1600, 550);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(snapshot.stick && snapshot.look && pad.lx > 0);
    finger(SDL_EVENT_FINGER_UP, 3, hx, hy);
    finger(SDL_EVENT_FINGER_UP, 4, 1600, 550);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(!snapshot.stick && !snapshot.look);

    touch::set_hidden(true);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(snapshot.collapsed && snapshot.items.size() == 1);
    assert(snapshot.items[0].shape == touch::DrawItem::kToggle);
    touch::set_hidden(false);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(!snapshot.collapsed && snapshot.items.size() == 15);
    assert(snapshot.items.capacity() == capacity);
    touch::set_mode(2);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(!snapshot.visible && snapshot.items.empty() && !snapshot.stick && !snapshot.look);
    assert(!snapshot.collapsed && snapshot.w == 0);
    touch::set_mode(1);
    pad = {}; touch::apply(pad); touch::draw_state(snapshot);
    assert(snapshot.visible && snapshot.items.size() == 15);
    std::puts("touch: input, reset, hidden/visible transitions and 10,000 allocation-free snapshots passed");
}
#endif

int main() {
    menu::close();
    const auto before = allocations, reads = settings_reads;
    size_t rows = 0;
    for (int i = 0; i < 10000; ++i) rows += menu::view().rows.size();
    std::printf("closed menu / 10,000 calls: %zu allocations, %zu settings reads, %zu rows\n",
                allocations - before, settings_reads - reads, rows);
#ifndef BOTW_MENU_BENCH_ONLY
    assert(allocations == before && settings_reads == reads && rows == 0);
    menu::toggle();
    auto view = menu::view();
    assert(view.open && view.rows.size() == 11 && !view.note.empty());
    assert(view.rows[0].value == "60 fps");
    menu::click(0, 1);
    assert(menu::view().rows[0].value == "30 fps (original)");
    menu::close();
    assert(menu::view().rows.empty());
    touch_tests();
    std::puts("ui hot-path tests passed");
#endif
}
