#pragma once
#include "SDL_events.h"
#include <cstring>
#define SDL_zero(e) std::memset(&(e), 0, sizeof(e))
uint64_t SDL_GetTicksNS();
float SDL_GetWindowPixelDensity(SDL_Window*);
uint32_t SDL_GetWindowID(SDL_Window*);
int SDL_GetTouchDeviceType(SDL_TouchID);
bool SDL_PushEvent(SDL_Event*);
