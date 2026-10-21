// Touch controls: snapshot of the on-screen layout for drawing. Built by runtime/src/host/touch.cpp (host
// main thread), drawn by runtime/cemu_shims/overlay_touch.cpp (GPU thread, Dear ImGui draw lists).
// Plain types only (no SDL / ImGui) so both targets can include it.
#pragma once
#include <stdint.h>

#include <vector>

namespace touch {
struct DrawItem {
    enum Shape : uint8_t { kCircle, kPill, kDPad, kMenu, kToggle } shape = kCircle;
    float x = 0, y = 0;   // center, layout pixels
    float r = 0, hh = 0;  // circle radius / d-pad radius / pill half width; pill half height
    const char* label = "";
    bool pressed = false;
    uint32_t dpad = 0;  // kDPad: pressed directions (input::BTN_UP/DOWN/LEFT/RIGHT)
};
struct DrawState {
    bool visible = false;
    bool collapsed = false;  // only the show / hide button (items has just that)
    float w = 0, h = 0;  // layout space = drawable pixels when the layout was made
    float u = 1;         // pixels per design point
    std::vector<DrawItem> items;
    // floating left stick: home ring while idle, base + knob while held
    float home_x = 0, home_y = 0, stick_r = 0;
    bool stick = false;
    float base_x = 0, base_y = 0, knob_x = 0, knob_y = 0;
    // camera drag: a small ring under the finger
    bool look = false;
    float look_x = 0, look_y = 0;
};
// Fill a caller-owned snapshot, reusing its item storage between frames (any thread).
void draw_state(DrawState& state);
}  // namespace touch
