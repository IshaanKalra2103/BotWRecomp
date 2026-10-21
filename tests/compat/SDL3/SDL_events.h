#pragma once
#include <cstdint>
using SDL_TouchID = uint64_t;
using SDL_FingerID = uint64_t;
using SDL_EventType = uint32_t;
constexpr SDL_EventType SDL_EVENT_FINGER_DOWN = 1, SDL_EVENT_FINGER_MOTION = 2, SDL_EVENT_FINGER_UP = 3;
constexpr SDL_TouchID SDL_MOUSE_TOUCHID = ~SDL_TouchID{0};
constexpr int SDL_TOUCH_DEVICE_DIRECT = 1;
struct SDL_Window {};
struct SDL_TouchFingerEvent {
    SDL_EventType type;
    SDL_TouchID touchID;
    SDL_FingerID fingerID;
    float x, y, pressure;
    uint32_t windowID;
};
union SDL_Event {
    SDL_EventType type;
    SDL_TouchFingerEvent tfinger;
};
