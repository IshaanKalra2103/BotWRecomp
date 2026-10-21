// On-screen touch controls (iPhone / iPad, or a touchscreen). Host main thread: window.mm forwards SDL finger
// events; every finger owns one control until it is lifted, and apply() merges the virtual pad into the
// GamePad state each host loop. runtime/cemu_shims/overlay_touch.cpp draws draw_state() on the GPU thread.
//
// Layout (landscape, inside the window's safe area, sizes in points scaled with the screen):
//   [ZL]                         (-)  (menu)  (+)  (hide)                 [ZR]
//   [L ]                                                                  [R ]
//   D-pad                                                                  (X)
//                                                                       (Y)   (A)
//   floating stick (any touch on the left half)                            (B)
//                         (L3)                         (R3)
// The right half outside the buttons is the camera: dragging moves the right stick (finger speed ->
// deflection, smoothed), lifting the finger returns it to 0. The eye button collapses everything to itself
// (and back); that choice is saved in settings.ini (touch_hidden).
//
// Test hook (headless / simulator): BOTW_TOUCH_TEST="check[:s],show[:s],menu[:s]" pushes synthetic finger
// events with SDL_PushEvent, s seconds after the first host loop:
//   check  input checks; logs "touch test: <name> ok|FAIL" with the pad state the game received
//   show   holds stick + B + D-pad + camera finger for 30 s (screenshots), then lets go
//   hide   taps the eye button, checks the stick is dead, taps it again, checks the stick works
//   menu   taps the menu button, then outside the menu (closes it), then the button and the menu's X
#include "touch.h"

#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../log.h"
#include "../settings.h"
#include "menu.h"
#include "touch_draw.h"
#include "window.h"

namespace touch {
namespace {
// Feel. Distances in points (pixels / u).
constexpr float kStickRadius = 54;     // floating stick drag radius
constexpr float kStickDead = 0.10f;    // radial dead zone, fraction of the radius
constexpr float kLookFullSpeed = 420;  // camera drag speed (pt/s) for a full right-stick deflection
constexpr float kLookRest = 10;        // slower than this (smoothed) counts as a resting finger
constexpr float kLookFloor = 0.12f;    // smallest non-zero output, past the game's own stick dead zone
constexpr float kLookTau = 0.045f;     // velocity smoothing time constant, seconds
constexpr float kSlop = 1.25f;         // round buttons react a bit outside their drawn radius
constexpr float kPillSlop = 10;        // and shoulder buttons this many points outside

constexpr SDL_TouchID kTestTouch = 0x70C4E57;  // synthetic BOTW_TOUCH_TEST events

std::atomic<int> g_mode{0};
std::atomic<bool> g_controller{false};
std::atomic<bool> g_hidden{false};
#if defined(__APPLE__) && TARGET_OS_IOS
std::atomic<bool> g_touchscreen{true};
#else
std::atomic<bool> g_touchscreen{false};  // Mac: Auto shows the controls once a touchscreen reports a touch
#endif
std::atomic<bool> g_test{false};

enum Role : uint8_t { kCtrl, kStick, kLook };
struct Finger {
    SDL_FingerID id;
    Role role;
    int ctrl;      // kCtrl: index into State::ctrls
    uint32_t dir;  // kCtrl on the D-pad: current direction
    float x, y;    // layout pixels
};
struct Control {
    DrawItem d;
    uint32_t mask = 0;
    bool dpad = false, menu = false, toggle = false;
};
struct State {
    float w = 0, h = 0, density = 0, safe[4] = {-1, -1, -1, -1};
    float u = 1;
    std::vector<Control> ctrls;
    float home_x = 0, home_y = 0, stick_r = 0;
    std::vector<Finger> fingers;
    bool stick = false;
    SDL_FingerID stick_id = 0;
    float bx = 0, by = 0, kx = 0, ky = 0;
    bool look = false;
    SDL_FingerID look_id = 0;
    float lpx = 0, lpy = 0;  // look finger at the previous apply()
    float vx = 0, vy = 0;    // smoothed look velocity, pt/s
    uint64_t last_ns = 0;
    bool visible = false;
    input::PadState out;  // our contribution in the last apply()
};
std::mutex g_lock;
State g;

bool visible_now() { return g_test || (active() && (g_mode == 1 || g_touchscreen)); }

Finger* find(SDL_FingerID id) {
    for (auto& f : g.fingers)
        if (f.id == id) return &f;
    return nullptr;
}

void release_all() {
    g.fingers.clear();
    g.stick = g.look = false;
    g.vx = g.vy = 0;
}

void add(DrawItem::Shape shape, float x, float y, float r, float hh, const char* label, uint32_t mask,
         bool dpad = false, bool menu = false) {
    Control c;
    c.d.shape = shape;
    c.d.x = x, c.d.y = y, c.d.r = r, c.d.hh = hh;
    c.d.label = label;
    c.mask = mask;
    c.dpad = dpad, c.menu = menu;
    g.ctrls.push_back(c);
}

// Rebuild the layout when the drawable size, pixel density or safe area changes.
void update_layout() {
    int iw, ih;
    window::drawable_size(&iw, &ih);
    float safe[4];
    menu::safe_area(&safe[0], &safe[1], &safe[2], &safe[3]);
    SDL_Window* win = window::g_win_ptr();
    float density = win ? SDL_GetWindowPixelDensity(win) : 1.0f;
    if (!(density > 0)) density = 1;
    const float W = (float)iw, H = (float)ih;
    if (W == g.w && H == g.h && density == g.density && !memcmp(safe, g.safe, sizeof safe)) return;
    if (W <= 0 || H <= 0) return;
    release_all();
    g.w = W, g.h = H, g.density = density;
    memcpy(g.safe, safe, sizeof safe);
    // one design point = u pixels: points, grown a little on big screens (iPad, Mac windows)
    const float u = g.u = density * std::clamp(std::min(W, H) / density / 400.0f, 0.85f, 1.5f);
    const float L = safe[0], T = safe[1], R = W - safe[2], B = H - safe[3];
    const float m = 10 * u;
    g.ctrls.clear();
    // face buttons, Nintendo positions: A right, B bottom, X top, Y left
    const float rb = 25 * u, d = 47 * u;
    const float cx = R - m - d - rb, cy = B - m - d - rb;
    add(DrawItem::kCircle, cx + d, cy, rb, 0, "A", input::BTN_A);
    add(DrawItem::kCircle, cx, cy + d, rb, 0, "B", input::BTN_B);
    add(DrawItem::kCircle, cx, cy - d, rb, 0, "X", input::BTN_X);
    add(DrawItem::kCircle, cx - d, cy, rb, 0, "Y", input::BTN_Y);
    // shoulders: ZL / ZR in the top corners, L / R under them
    const float pw = 37 * u, ph = 16 * u, gap = 8 * u;
    add(DrawItem::kPill, L + m + pw, T + m + ph, pw, ph, "ZL", input::BTN_ZL);
    add(DrawItem::kPill, L + m + pw, T + m + 3 * ph + gap, pw, ph, "L", input::BTN_L);
    add(DrawItem::kPill, R - m - pw, T + m + ph, pw, ph, "ZR", input::BTN_ZR);
    add(DrawItem::kPill, R - m - pw, T + m + 3 * ph + gap, pw, ph, "R", input::BTN_R);
    // top center: - (map), menu, + (pause)
    const float sr = 16 * u, ty = T + m + sr;
    add(DrawItem::kCircle, W * 0.5f - 62 * u, ty, sr, 0, "-", input::BTN_MINUS);
    add(DrawItem::kMenu, W * 0.5f, ty, 15 * u, 0, "", 0, false, true);
    add(DrawItem::kCircle, W * 0.5f + 62 * u, ty, sr, 0, "+", input::BTN_PLUS);
    add(DrawItem::kToggle, W * 0.5f + 124 * u, ty, 14 * u, 0, "", 0);  // show / hide the controls
    g.ctrls.back().toggle = true;
    // D-pad on the left, under L, above the stick area
    const float dr = 40 * u;
    add(DrawItem::kDPad, L + m + dr + 4 * u, T + m + 4 * ph + gap + 16 * u + dr, dr, 0, "", 0, true);
    // stick clicks, bottom center-ish: L3 crouch, R3 look / scope
    const float r3 = 18 * u;
    add(DrawItem::kCircle, W * 0.5f - 122 * u, B - m - r3, r3, 0, "L3", input::BTN_STICK_L);
    add(DrawItem::kCircle, W * 0.5f + 122 * u, B - m - r3, r3, 0, "R3", input::BTN_STICK_R);
    // floating stick: drawn at home while idle, anywhere on the left half when touched
    g.stick_r = kStickRadius * u;
    g.home_x = L + m + 20 * u + g.stick_r;
    g.home_y = B - m - 14 * u - g.stick_r;
    LOG("touch: layout %.0fx%.0f px, %.2f px/pt, safe area l %.0f t %.0f r %.0f b %.0f", W, H, u, safe[0], safe[1],
        safe[2], safe[3]);
}

int hit(float x, float y) {
    int best = -1;
    float best_score = 1e9f;
    for (int i = 0; i < (int)g.ctrls.size(); i++) {
        const DrawItem& d = g.ctrls[i].d;
        float score;
        if (d.shape == DrawItem::kPill) {
            float dx = std::max(fabsf(x - d.x) - d.r, 0.0f), dy = std::max(fabsf(y - d.y) - d.hh, 0.0f);
            float out = hypotf(dx, dy);
            if (out > kPillSlop * g.u) continue;
            score = out / (kPillSlop * g.u);
        } else {
            float slop = d.shape == DrawItem::kDPad ? 1.15f : kSlop;
            float dist = hypotf(x - d.x, y - d.y);
            if (dist > d.r * slop) continue;
            score = dist / (d.r * slop);
        }
        if (score < best_score) best_score = score, best = i;
    }
    return best;
}

uint32_t dpad_dir(const DrawItem& d, float x, float y) {
    float dx = x - d.x, dy = y - d.y;
    if (hypotf(dx, dy) < d.r * 0.2f) return 0;
    if (fabsf(dx) > fabsf(dy)) return dx > 0 ? input::BTN_RIGHT : input::BTN_LEFT;
    return dy > 0 ? input::BTN_DOWN : input::BTN_UP;
}

void stick_to(float x, float y) {  // the base follows a thumb that leaves the ring
    float dx = x - g.bx, dy = y - g.by, m = hypotf(dx, dy);
    if (m > g.stick_r) {
        g.bx = x - dx / m * g.stick_r;
        g.by = y - dy / m * g.stick_r;
    }
    g.kx = x, g.ky = y;
}

input::PadState contribution(float dt) {
    input::PadState t;
    for (auto& f : g.fingers)
        if (f.role == kCtrl) t.buttons |= g.ctrls[f.ctrl].dpad ? f.dir : g.ctrls[f.ctrl].mask;
    if (g.stick) {
        float vx = (g.kx - g.bx) / g.stick_r, vy = (g.ky - g.by) / g.stick_r, m = hypotf(vx, vy);
        if (m > kStickDead) {
            float o = std::min(1.0f, (m - kStickDead) / (1 - kStickDead));
            t.lx = vx / m * o;
            t.ly = -vy / m * o;
        }
    }
    if (g.look) {
        if (Finger* f = find(g.look_id)) {
            float dx = (f->x - g.lpx) / g.u, dy = (f->y - g.lpy) / g.u;
            g.lpx = f->x, g.lpy = f->y;
            float a = 1 - expf(-dt / kLookTau);
            g.vx += (dx / dt - g.vx) * a;
            g.vy += (dy / dt - g.vy) * a;
            float sp = hypotf(g.vx, g.vy);
            if (sp > kLookRest) {
                float o = kLookFloor + (1 - kLookFloor) * std::min(1.0f, (sp - kLookRest) / (kLookFullSpeed - kLookRest));
                t.rx = g.vx / sp * o;
                t.ry = -g.vy / sp * o;
            }
        }
    }
    return t;
}

// ---- BOTW_TOUCH_TEST ------------------------------------------------------------------------------------
struct Step {
    double t;
    std::function<void()> fn;
    bool done = false;
};
std::vector<Step> g_steps;
std::function<void(double)> g_tick;  // per-loop hook (continuous finger motion)
uint64_t g_test_t0 = 0;
int g_pass = 0, g_fail = 0;

void push(SDL_EventType type, SDL_FingerID id, float x, float y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    e.tfinger.touchID = kTestTouch;
    e.tfinger.fingerID = id;
    e.tfinger.x = x / g.w;
    e.tfinger.y = y / g.h;
    e.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0f : 1.0f;
    e.tfinger.windowID = window::g_win_ptr() ? SDL_GetWindowID(window::g_win_ptr()) : 0;
    SDL_PushEvent(&e);
}
void down(SDL_FingerID id, float x, float y) { push(SDL_EVENT_FINGER_DOWN, id, x, y); }
void move(SDL_FingerID id, float x, float y) { push(SDL_EVENT_FINGER_MOTION, id, x, y); }
void up(SDL_FingerID id) {
    Finger* f = find(id);
    push(SDL_EVENT_FINGER_UP, id, f ? f->x : 0, f ? f->y : 0);
}
const DrawItem& ctrl(const char* label) {
    for (auto& c : g.ctrls)
        if (!strcmp(c.d.label, label) && (*label || c.dpad)) return c.d;
    for (auto& c : g.ctrls)
        if ((c.menu && !strcmp(label, "menu")) || (c.toggle && !strcmp(label, "hide"))) return c.d;
    return g.ctrls[0].d;
}
void check(const char* name, std::function<bool(const input::PadState&)> ok) {
    input::PadState s = input::gamepad();  // what the game got after the previous host loop
    bool pass = ok(s);
    (pass ? g_pass : g_fail)++;
    LOG("touch test: %-28s %s  buttons %05x  L %+.2f %+.2f  R %+.2f %+.2f", name, pass ? "ok  " : "FAIL", s.buttons,
        s.lx, s.ly, s.rx, s.ry);
}
bool zero_axes(const input::PadState& s) { return s.lx == 0 && s.ly == 0 && s.rx == 0 && s.ry == 0; }

void at(double t, std::function<void()> fn) { g_steps.push_back({t, std::move(fn)}); }

void plan_check(double t0) {
    using namespace input;
    const float u = g.u;
    const float px = g.home_x, py = g.home_y;
    at(t0 + 0.00, [=] { down(1, px, py); });
    at(t0 + 0.05, [=] { move(1, px + 40 * u, py); });
    at(t0 + 0.25, [] { check("stick right", [](auto& s) { return s.lx > 0.6f && s.lx < 0.8f && fabsf(s.ly) < 0.02f && !s.buttons; }); });
    at(t0 + 0.25, [] { down(2, ctrl("A").x, ctrl("A").y); });
    at(t0 + 0.45, [] { check("A held + stick", [](auto& s) { return s.buttons == BTN_A && s.lx > 0.6f; }); });
    // camera: a finger dragged right at 250 pt/s for 0.5 s, then resting, then lifted
    const float lx0 = g.w * 0.62f, ly0 = g.h * 0.5f;
    at(t0 + 0.45, [=] {
        down(3, lx0, ly0);
        double start = t0 + 0.45;
        g_tick = [=](double t) {
            if (t > start + 0.5) return;
            move(3, lx0 + (float)(t - start) * 250 * u, ly0);
        };
    });
    at(t0 + 0.85, [] {
        check("camera drag right", [](auto& s) { return s.rx > 0.45f && s.rx < 0.8f && fabsf(s.ry) < 0.05f && s.buttons == BTN_A && s.lx > 0.6f; });
    });
    at(t0 + 1.35, [] { check("camera finger resting", [](auto& s) { return fabsf(s.rx) < 0.01f && fabsf(s.ry) < 0.01f; }); });
    at(t0 + 1.35, [] { g_tick = nullptr; up(3); });
    at(t0 + 1.45, [] { check("camera released", [](auto& s) { return s.rx == 0 && s.ry == 0 && s.lx > 0.6f; }); });
    at(t0 + 1.45, [] { up(2); });
    at(t0 + 1.55, [] { check("A released", [](auto& s) { return !s.buttons; }); });
    // stick: past the ring to the left (base follows), then back a little
    at(t0 + 1.55, [=] { move(1, px - 160 * u, py); });
    at(t0 + 1.75, [] { check("stick hard left", [](auto& s) { return s.lx < -0.99f && fabsf(s.ly) < 0.02f; }); });
    at(t0 + 1.75, [=] { move(1, px - 130 * u, py); });
    at(t0 + 1.95, [] { check("stick base followed", [](auto& s) { return s.lx > -0.5f && s.lx < -0.25f; }); });
    at(t0 + 1.95, [=] { move(1, px - 106 * u, py - 80 * u); });  // straight up from the moved base
    at(t0 + 2.10, [] { check("stick up", [](auto& s) { return s.ly > 0.9f && fabsf(s.lx) < 0.1f; }); });
    at(t0 + 2.10, [] { up(1); });
    at(t0 + 2.20, [] { check("stick released", [](auto& s) { return zero_axes(s) && !s.buttons; }); });
    // D-pad: press up, slide to right
    at(t0 + 2.20, [] { auto& d = ctrl(""); down(4, d.x, d.y - d.r * 0.6f); });
    at(t0 + 2.30, [] { check("dpad up", [](auto& s) { return s.buttons == BTN_UP; }); });
    at(t0 + 2.30, [] { auto& d = ctrl(""); move(4, d.x + d.r * 0.6f, d.y); });
    at(t0 + 2.40, [] { check("dpad slid right", [](auto& s) { return s.buttons == BTN_RIGHT; }); });
    at(t0 + 2.40, [] { up(4); });
    // several buttons at once, touches a little off-center
    at(t0 + 2.50, [=] {
        down(5, ctrl("ZR").x - 30 * u, ctrl("ZR").y + 10 * u);
        down(6, ctrl("R3").x + 8 * u, ctrl("R3").y);
        down(7, ctrl("L").x, ctrl("L").y);
        down(8, ctrl("Y").x, ctrl("Y").y - 12 * u);
    });
    at(t0 + 2.60, [] { check("ZR + R3 + L + Y", [](auto& s) { return s.buttons == (BTN_ZR | BTN_STICK_R | BTN_L | BTN_Y) && zero_axes(s); }); });
    at(t0 + 2.60, [] { up(5), up(6), up(7), up(8); });
    at(t0 + 2.70, [] {
        down(9, ctrl("-").x, ctrl("-").y);
        down(10, ctrl("L3").x, ctrl("L3").y);
        down(11, ctrl("X").x, ctrl("X").y);
        down(12, ctrl("ZL").x, ctrl("ZL").y);
    });
    at(t0 + 2.80, [] { check("- + L3 + X + ZL", [](auto& s) { return s.buttons == (BTN_MINUS | BTN_STICK_L | BTN_X | BTN_ZL); }); });
    at(t0 + 2.80, [] { up(9), up(10), up(11), up(12); });
    at(t0 + 2.85, [] { down(13, ctrl("+").x, ctrl("+").y), down(14, ctrl("R").x, ctrl("R").y), down(15, ctrl("B").x, ctrl("B").y); });
    at(t0 + 2.95, [] { check("+ + R + B", [](auto& s) { return s.buttons == (BTN_PLUS | BTN_R | BTN_B); }); });
    at(t0 + 2.95, [] { up(13), up(14), up(15); });
    // a second finger on the left half while the stick is held is ignored; the stick keeps its finger
    at(t0 + 3.05, [=] { down(16, px, py); down(17, px + 120 * u, py - 40 * u); });
    at(t0 + 3.10, [=] { move(16, px, py + 40 * u); move(17, px + 200 * u, py - 40 * u); });
    at(t0 + 3.25, [] { check("2nd left finger ignored", [](auto& s) { return s.ly < -0.5f && fabsf(s.lx) < 0.02f && !s.buttons; }); });
    at(t0 + 3.25, [] { up(16), up(17); });
    at(t0 + 3.35, [] { check("all released", [](auto& s) { return zero_axes(s) && !s.buttons; }); });
    at(t0 + 3.35, [] { LOG("touch test: check done, %d passed, %d failed", g_pass, g_fail); });
}

void plan_show(double t0) {
    const float u = g.u;
    at(t0, [=] {
        down(31, g.home_x, g.home_y);
        down(32, ctrl("B").x, ctrl("B").y);
        down(33, ctrl("").x - ctrl("").r * 0.6f, ctrl("").y);
        down(34, g.w * 0.66f, g.h * 0.42f);
    });
    at(t0 + 0.1, [=] { move(31, g.home_x + 30 * u, g.home_y - 32 * u); });
    at(t0 + 1.0, [] { check("show: stick + B + dpad left", [](auto& s) { return s.buttons == (input::BTN_B | input::BTN_LEFT) && s.lx > 0.4f && s.ly > 0.4f; }); });
    at(t0 + 30, [] { up(31), up(32), up(33), up(34); });
    at(t0 + 30.2, [] { check("show: released", [](auto& s) { return zero_axes(s) && !s.buttons; }); });
}

void plan_hide(double t0) {
    const float u = g.u;
    const float px = g.home_x, py = g.home_y;
    at(t0, [] { down(51, ctrl("hide").x, ctrl("hide").y); });
    at(t0 + 0.05, [] { up(51); });
    at(t0 + 0.15, [=] { down(52, px, py); });
    at(t0 + 0.20, [=] { move(52, px + 40 * u, py); });
    at(t0 + 0.35, [] { check("hidden: stick dead", [](auto& s) { return zero_axes(s) && !s.buttons; }); });
    at(t0 + 0.35, [] { up(52); });
    at(t0 + 0.45, [] { down(53, ctrl("hide").x, ctrl("hide").y); });
    at(t0 + 0.50, [] { up(53); });
    at(t0 + 0.60, [=] { down(54, px, py); });
    at(t0 + 0.65, [=] { move(54, px + 40 * u, py); });
    at(t0 + 0.80, [] { check("shown again: stick right", [](auto& s) { return s.lx > 0.6f; }); });
    at(t0 + 0.80, [] { up(54); });
    at(t0 + 0.90, [] { LOG("touch test: hide done, %d passed, %d failed", g_pass, g_fail); });
}

void expect_menu(const char* name, bool open) {
    bool ok = menu::is_open() == open;
    (ok ? g_pass : g_fail)++;
    LOG("touch test: %-28s %s", name, ok ? "ok" : "FAIL");
}
void plan_menu(double t0) {
    auto tap = [](SDL_FingerID id, float x, float y) { down(id, x, y), up(id); };
    at(t0, [=] { tap(41, ctrl("menu").x, ctrl("menu").y); });
    at(t0 + 1.0, [] { expect_menu("menu: opened by the button", true); });
    at(t0 + 1.0, [=] { tap(42, g.w * 0.03f, g.h * 0.5f); });
    at(t0 + 1.5, [] { expect_menu("menu: tap outside closes", false); });
    at(t0 + 1.5, [=] { tap(43, ctrl("menu").x, ctrl("menu").y); });
    at(t0 + 2.5, [=] {
        expect_menu("menu: opened again", true);
        float p[4], x[4];
        menu::rects(p, x);
        LOG("touch test: menu panel %.2f %.2f - %.2f %.2f, X %.2f %.2f - %.2f %.2f", p[0], p[1], p[2], p[3], x[0], x[1], x[2], x[3]);
        tap(44, g.w * (p[0] + p[2]) * 0.5f, g.h * (p[1] + p[3]) * 0.5f);  // a row, not the X
    });
    at(t0 + 3.0, [=] {
        expect_menu("menu: tap inside stays open", true);
        float p[4], x[4];
        menu::rects(p, x);
        tap(45, g.w * (x[0] + x[2]) * 0.5f, g.h * (x[1] + x[3]) * 0.5f);
    });
    at(t0 + 3.5, [] { expect_menu("menu: X closes", false); });
    at(t0 + 3.5, [] { LOG("touch test: menu done, %d passed, %d failed", g_pass, g_fail); });
}

void test_tick() {
    static bool parsed = false;
    if (!parsed && g.w > 0) {  // after the first layout
        parsed = true;
        const char* spec = getenv("BOTW_TOUCH_TEST");
        if (!spec || !*spec) return;
        g_test = true;
        g_test_t0 = SDL_GetTicksNS();
        std::string s = spec;
        size_t i = 0;
        while (i <= s.size()) {
            size_t j = s.find(',', i);
            std::string tok = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
            i = j == std::string::npos ? s.size() + 1 : j + 1;
            size_t c = tok.find(':');
            std::string name = tok.substr(0, c);
            double t = c == std::string::npos ? -1 : atof(tok.c_str() + c + 1);
            if (name == "check") plan_check(t < 0 ? 1 : t);
            else if (name == "show") plan_show(t < 0 ? 20 : t);
            else if (name == "hide") plan_hide(t < 0 ? 10 : t);
            else if (name == "menu") plan_menu(t < 0 ? 60 : t);
        }
        LOG("touch test: \"%s\", %zu steps", spec, g_steps.size());
    }
    if (!g_test) return;
    double t = (SDL_GetTicksNS() - g_test_t0) * 1e-9;
    if (g_tick) g_tick(t);
    for (size_t i = 0; i < g_steps.size(); i++)
        if (!g_steps[i].done && t >= g_steps[i].t) {
            g_steps[i].done = true;
            g_steps[i].fn();
        }
}
}  // namespace

void set_mode(int mode) { g_mode = mode; }
void cancel_all() {
    std::lock_guard guard(g_lock);
    release_all();
    g.out = input::PadState{};
}
void set_controller_connected(bool c) { g_controller = c; }
void set_hidden(bool h) { g_hidden = h; }
bool active() { return g_mode == 1 || (g_mode == 0 && !g_controller); }

void event(const SDL_Event& e) {
    const SDL_TouchFingerEvent& f = e.tfinger;
    if (f.touchID != kTestTouch) {  // only touchscreens (not trackpads, not mouse-made touches)
        if (f.touchID == SDL_MOUSE_TOUCHID || SDL_GetTouchDeviceType(f.touchID) != SDL_TOUCH_DEVICE_DIRECT) return;
        g_touchscreen = true;
    }
    std::lock_guard<std::mutex> l(g_lock);
    update_layout();
    if (g.w <= 0) return;
    const float x = f.x * g.w, y = f.y * g.h;
    Finger* fg = find(f.fingerID);
    if (e.type == SDL_EVENT_FINGER_DOWN) {
        // touches while the settings menu is open belong to the menu (ImGui gets them as mouse events),
        // except a tap outside it or on its X, which closes it
        if (menu::is_open()) {
            if (menu::touch_close(f.x, f.y)) LOG("touch: settings menu closed by a tap");
            return;
        }
        if (fg || !g.visible) return;
        Finger n{f.fingerID, kCtrl, -1, 0, x, y};
        int c = hit(x, y);
        if (g_hidden && (c < 0 || !g.ctrls[c].toggle)) return;  // collapsed: only the eye button is live
        if (c >= 0) {
            n.ctrl = c;
            if (g.ctrls[c].dpad) n.dir = dpad_dir(g.ctrls[c].d, x, y);
            if (g.ctrls[c].menu) {
                menu::toggle();
                LOG("touch: menu button -> settings menu %s", menu::is_open() ? "open" : "closed");
            }
            if (g.ctrls[c].toggle) {
                Settings s = settings::get();
                s.touch_hidden = !s.touch_hidden;
                settings::set(s);  // -> set_hidden()
                LOG("touch: controls %s", s.touch_hidden ? "hidden" : "shown");
            }
        } else if (x < g.w * 0.5f) {
            if (g.stick) return;  // one stick finger at a time
            n.role = kStick;
            g.stick = true, g.stick_id = f.fingerID;
            g.bx = g.kx = x, g.by = g.ky = y;
        } else {
            if (g.look) return;
            n.role = kLook;
            g.look = true, g.look_id = f.fingerID;
            g.lpx = x, g.lpy = y;
            g.vx = g.vy = 0;
        }
        g.fingers.push_back(n);
    } else if (!fg) {
        return;
    } else if (e.type == SDL_EVENT_FINGER_MOTION) {
        fg->x = x, fg->y = y;
        if (fg->role == kStick) stick_to(x, y);
        else if (fg->role == kCtrl && g.ctrls[fg->ctrl].dpad) fg->dir = dpad_dir(g.ctrls[fg->ctrl].d, x, y);
    } else {  // up / canceled
        if (fg->role == kStick) g.stick = false;
        if (fg->role == kLook) g.look = false, g.vx = g.vy = 0;
        g.fingers.erase(g.fingers.begin() + (fg - g.fingers.data()));
    }
}

void apply(input::PadState& s) {
    std::lock_guard<std::mutex> l(g_lock);
    update_layout();
    test_tick();
    const bool vis = visible_now() && g.w > 0;
    if (vis != g.visible) {
        g.visible = vis;
        if (!vis) release_all();
        LOG("touch: on-screen controls %s", vis ? "shown" : "hidden");
    }
    const uint64_t now = SDL_GetTicksNS();
    const float gap = g.last_ns ? (now - g.last_ns) * 1e-9f : 1.0f;
    const float dt = std::clamp(gap, 0.0005f, 0.1f);
    g.last_ns = now;
    if (gap > 0.1f && g.look)  // first loop after a pause (settings menu was open): no velocity spike
        if (const Finger* f = find(g.look_id)) g.lpx = f->x, g.lpy = f->y, g.vx = g.vy = 0;
    static bool was_hidden = false;
    if (g_hidden != was_hidden) {
        was_hidden = g_hidden;
        if (was_hidden) g.stick = g.look = false, g.vx = g.vy = 0;
    }
    if (!vis || g_hidden) {
        g.out = {};
        return;
    }
    input::PadState t = contribution(dt);
    g.out = t;
    s.buttons |= t.buttons;
    s.lx += t.lx;
    s.ly += t.ly;
    s.rx += t.rx;
    s.ry += t.ry;
}

void draw_state(DrawState& d) {
    std::lock_guard<std::mutex> l(g_lock);
    // Reset transient fields without discarding the caller's vector capacity.
    DrawState fresh;
    fresh.items.swap(d.items);
    fresh.items.clear();
    d = std::move(fresh);
    d.visible = g.visible && g.w > 0;
    if (!d.visible) return;
    d.w = g.w, d.h = g.h, d.u = g.u;
    d.items.reserve(g.ctrls.size());
    for (auto& c : g.ctrls) d.items.push_back(c.d);
    for (auto& f : g.fingers)
        if (f.role == kCtrl) {
            d.items[f.ctrl].pressed = true;
            d.items[f.ctrl].dpad |= f.dir;
        }
    if (g_hidden) {  // just the eye button
        for (size_t i = 0; i < g.ctrls.size(); i++)
            if (g.ctrls[i].toggle) d.items = {d.items[i]};
        d.collapsed = true;
        return;
    }
    d.home_x = g.home_x, d.home_y = g.home_y, d.stick_r = g.stick_r;
    d.stick = g.stick;
    d.base_x = g.bx, d.base_y = g.by, d.knob_x = g.kx, d.knob_y = g.ky;
    if (g.look)
        if (const Finger* f = find(g.look_id)) d.look = true, d.look_x = f->x, d.look_y = f->y;
}
}  // namespace touch
