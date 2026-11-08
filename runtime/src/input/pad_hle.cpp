// vpad / vpadbase / padscore HLE. The GamePad (VPAD channel 0) is fed from input::gamepad();
// no Wii Remotes / Pro Controllers are reported (WPADProbe -> no controller).
#include <algorithm>

#include "../hle.h"
#include "input.h"

namespace {
uint32_t g_prev_buttons = 0;
uint32_t g_sampling_cb = 0;

void wf(uint32_t a, float v) { mem::wf32(a, v); }
}  // namespace

// int32 VPADRead(chan, VPADStatus* buf, uint32 count, int32* err)
HLE_EXPORT(vpad, VPADRead) {
    uint32_t chan = ARG(0), buf = ARG(1), count = ARG(2), err = ARG(3);
    if (chan != 0 || !count) {
        if (err) mem::w32(err, (uint32_t)-2);
        RET(0);
        return;
    }
    input::PadState s = input::gamepad();
    memset(mem::host(buf), 0, 0xAC);
    uint32_t hold = s.buttons;
    // stick emulation bits (games use them for menu navigation)
    auto stick_bits = [](float x, float y, uint32_t base) {
        uint32_t b = 0;
        if (x < -0.5f) b |= base << 3;   // LEFT
        if (x > 0.5f) b |= base << 2;    // RIGHT
        if (y > 0.5f) b |= base << 1;    // UP
        if (y < -0.5f) b |= base;        // DOWN
        return b;
    };
    hold |= stick_bits(s.lx, s.ly, 0x08000000) | stick_bits(s.rx, s.ry, 0x00800000);
    mem::w32(buf + 0x00, hold);
    mem::w32(buf + 0x04, hold & ~g_prev_buttons);
    mem::w32(buf + 0x08, g_prev_buttons & ~hold);
    g_prev_buttons = hold;
    wf(buf + 0x0C, s.lx);
    wf(buf + 0x10, s.ly);
    wf(buf + 0x14, s.rx);
    wf(buf + 0x18, s.ry);
    wf(buf + 0x30, 1.0f);  // accXY
    input::Motion m;
    if (input::motion(&m)) {  // controller gyro / accelerometer
        for (int k = 0; k < 3; k++) {
            wf(buf + 0x1C + 4 * k, m.acc[k]);
            wf(buf + 0x38 + 4 * k, m.gyro_change[k]);
            wf(buf + 0x44 + 4 * k, m.orientation[k]);
        }
        wf(buf + 0x28, m.acc_magnitude);
        wf(buf + 0x2C, m.acc_acceleration);
        for (int k = 0; k < 9; k++) wf(buf + 0x6C + 4 * k, m.dir[k]);
    } else {
        wf(buf + 0x20, -1.0f);  // acc: pad lying flat
        wf(buf + 0x28, 1.0f);   // acc magnitude
        // dir: identity orientation matrix (x, y, z axes)
        wf(buf + 0x6C, 1.0f);
        wf(buf + 0x7C, 1.0f);
        wf(buf + 0x8C, 1.0f);
    }
    // touch panel (tpNormal 0x52, tpFiltered1 0x5A, tpFiltered2 0x62: x, y, touch, validity): not touched,
    // position invalid (VPAD_INVALID_X | VPAD_INVALID_Y), as Cemu reports it. Zero would mean a valid
    // pointer at (0, 0).
    for (uint32_t tp : {0x52u, 0x5Au, 0x62u}) mem::w16(buf + tp + 6, 3);
    mem::w8(buf + 0xA0, 0x40);  // slide volume
    mem::w8(buf + 0xA1, 6);     // battery full
    if (err) mem::w32(err, 0);
    RET(1);
}
// int32 VPADControlMotor(chan, uint8* pattern, uint8 lengthInBits): rumble, see input::push_rumble
HLE_EXPORT(vpad, VPADControlMotor) {
    uint32_t bits = std::min<uint32_t>(ARG(2) & 0xFF, 120);
    if (ARG(0) != 0 || !ARG(1) || !bits) {
        input::clear_rumble();
        RET(0);
        return;
    }
    RET(input::push_rumble(mem::host(ARG(1)), (int)bits) ? 0 : (uint32_t)-1);
}
HLE_EXPORT(vpad, VPADStopMotor) { input::clear_rumble(); }
HLE_EXPORT(vpad, VPADDisableGyroDirRevise) {}
HLE_EXPORT(vpad, VPADEnableGyroAccRevise) {}
HLE_EXPORT(vpad, VPADSetGyroAccReviseParam) {}
HLE_EXPORT(vpad, VPADSetGyroDirection) {}
HLE_EXPORT(vpad, VPADSetGyroZeroDriftMode) {}
HLE_EXPORT(vpad, VPADSetSamplingCallback) {
    uint32_t prev = g_sampling_cb;
    g_sampling_cb = ARG(1);
    RET(prev);
}
// void VPADGetTPCalibratedPoint(chan, VPADTPData* out, const VPADTPData* in)
HLE_EXPORT(vpad, VPADGetTPCalibratedPoint) { memmove(mem::host(ARG(1)), mem::host(ARG(2)), 8); }
HLE_EXPORT(vpadbase, VPADBASEGetHeadphoneStatus) { RET(0); }

// ---- padscore: no remotes connected ---------------------------------------------------------------
HLE_EXPORT(padscore, KPADInitEx) {}
HLE_EXPORT(padscore, KPADGetMplsWorkSize) { RET(0x5FE0); }
HLE_EXPORT(padscore, KPADSetMplsWorkarea) {}
// int32 KPADReadEx(chan, KPADStatus* buf, uint32 count, int32* err)
HLE_EXPORT(padscore, KPADReadEx) {
    if (ARG(3)) mem::w32(ARG(3), (uint32_t)-1);  // KPAD_READ_ERR_NO_CONTROLLER
    RET(0);
}
// int32 WPADProbe(chan, uint32* type)
HLE_EXPORT(padscore, WPADProbe) {
    if (ARG(1)) mem::w32(ARG(1), 253);  // WPAD_DEV_NOT_FOUND
    RET((uint32_t)-1);                  // WPAD_ERR_NO_CONTROLLER
}
HLE_EXPORT(padscore, WPADControlMotor) {}
HLE_EXPORT(padscore, WPADControlSpeaker) { RET((uint32_t)-1); }
HLE_EXPORT(padscore, WPADCanSendStreamData) { RET(0); }
HLE_EXPORT(padscore, WPADSendStreamData) { RET((uint32_t)-1); }
HLE_EXPORT(padscore, WPADStartSyncDevice) { RET(0); }
HLE_EXPORT(padscore, WPADEnableURCC) {}
HLE_EXPORT(padscore, WENCGetEncodeData) { RET(0); }
