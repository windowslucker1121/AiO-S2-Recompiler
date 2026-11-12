// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode, TEST ONLY: an in-process SDL virtual gamepad that the test scripts drive with text commands ("vpad attach ps5",
// "vpad button south 1", "vpad axis lx -0.5", ...; a second pad for two-player tests: "vpad attach2 xbox", "vpad button2 south 1",
// "vpad detach2"). The text arrives through the key_sink test step; play.cpp hands the text
// after "vpad " to vpad_command. Nothing here runs outside the test build paths.
#pragma once

namespace nxrt::play {

// Runs one command. args is the text after "vpad " (tokens separated by spaces). Returns false for an unknown or invalid command,
// for a value out of range, and for a set command while no pad is attached.
bool vpad_command(const char* args);

// Detaches the virtual pad if one is attached (play mode's stop calls it); does nothing when there is none.
void vpad_shutdown();

// What the game has asked the virtual pad to rumble, as SDL's Rumble callback reported it.
// low and high are SDL's 16-bit motor values; calls counts every callback since the last attach.
struct VpadRumble {
    unsigned low = 0, high = 0;
    unsigned long long calls = 0;
};
VpadRumble vpad_last_rumble();

}  // namespace nxrt::play
