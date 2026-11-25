#pragma once

#include <windows.h>
#include <xinput.h>

namespace xport::pc {

// The PC's controllers through SDL3 (Xbox, PlayStation, Switch and most other pads, plugged in at any time), served
// in XInput's shape so the callers keep its button bits. Slots 0 to 3 are the Xbox ports: a controller takes the
// first free one when it appears and gives it back when it goes.
bool gamepad_init();  // once per process; false when SDL cannot start (the keyboard still works)
// noexcept like XInput's own, so the launcher's XInput pointers take them.
DWORD WINAPI gamepad_get_state(DWORD slot, XINPUT_STATE* state) noexcept;  // ERROR_SUCCESS or ERROR_DEVICE_NOT_CONNECTED
DWORD WINAPI gamepad_set_state(DWORD slot, XINPUT_VIBRATION* vibration) noexcept;
// How strongly the controllers rumble, 0 (never) to 1 (as the game asks, the default): the port passes its setting at
// start and when the player changes it. The motors running now follow at the game's next request.
void gamepad_set_rumble_scale(float scale);

}  // namespace xport::pc
