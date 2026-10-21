// On-screen touch controls (iPhone / iPad, or a touchscreen Mac). Host side (window.mm) feeds SDL finger
// events and merges the virtual pad into the GamePad state; the overlay (cemu_shims/overlay_menu.cpp)
// calls draw() inside its ImGui pass on the GPU thread.
#pragma once
#include <SDL3/SDL_events.h>

#include "../input/input.h"

namespace touch {
struct DrawState;
void set_mode(int mode);              // 0 auto (shown while no controller is connected), 1 on, 2 off
void set_controller_connected(bool connected);
void set_hidden(bool hidden);          // collapsed: only the show/hide button is shown and live
bool active();                        // controls currently shown and live
void event(const SDL_Event& e);       // SDL_EVENT_FINGER_* (host main thread)
void apply(input::PadState& s);       // add virtual buttons / sticks (host main thread, every loop)
void cancel_all(); // release touches when a system overlay takes input focus
void draw(const DrawState& state);    // GPU thread, draw the overlay's snapshot without copying it
}  // namespace touch
