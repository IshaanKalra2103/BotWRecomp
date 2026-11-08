// Host window, event loop and input mapping (host main thread). The renderer presents into the
// window's CAMetalLayer from the GPU thread.
#pragma once
#include <stdint.h>

#include <string>

struct SDL_Window;

namespace window {
void create();                   // main thread, before the game starts: window + renderer
void run();                      // event loop; returns when the app should quit
void drawable_size(int* w, int* h);
// Pixel size of the display the window is on (for the automatic render scale).
void display_pixel_size(int* w, int* h);
void set_gyro(bool on);  // controller gyro -> GamePad motion (default on)
void set_device_gyro(bool on);  // the phone's own gyro while no controller has one (default on)
void set_rumble(bool on);
void set_layout(int layout);  // 0: A = right face button (Nintendo positions), 1: A = bottom face button
void set_script(const std::string& s);  // "F:key:NAME,F:hold:NAME:frames,F:stick:l:x:y:frames,F:quit"
void on_frame(uint64_t frame);   // called by the renderer after each presented frame
bool quit_requested();
SDL_Window* g_win_ptr();
}  // namespace window
