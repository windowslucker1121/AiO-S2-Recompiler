// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: mouse look that moves a camera and not the player's body. Two uses:
//   * free look (the FreeLook binding, Alt by default): with the key held the mouse turns the first-person view around the eye while the body
//     keeps its heading; letting go puts the view back;
//   * the alternate scheme's camera (a vehicle or second form seen from outside): the mouse orbits the camera around it, with its own sensitivity.
// The pieces here are plain maths and an accumulator; play.cpp feeds them the mouse counts (from the ProcessInput hook) and applies the
// result to the camera object after the game's own Think (entry hooks on CFirstPersonCameraMP1::Think and CBallCameraMP1::Think).
// Conventions (the game's world is Y up, forward is +Z at yaw 0 and left is +X, transforms are 3x4 row-major with the translation in column 3, local-to-world
// for column vectors): mouse right turns the view right (yaw decreases), mouse up looks up (pitch increases), the same signs as the aim.
#pragma once
#include <cstdint>

namespace nxrt::play {

struct LookParams {
    double yaw_rad = 0.000767945, pitch_rad = 0.000767945;   // radians per count
    bool invert_x = false, invert_y = false;
    double pitch_limit_rad = 1.3963;                          // 80 degrees
};

// Accumulates counts into a yaw and a pitch offset. No smoothing: the offset is exactly counts * radians per count (pitch clamped).
class LookOffsets {
public:
    void add(double dx, double dy, const LookParams& p);
    // The same in radians (a stick turning the camera at a rate times the frame time). dyaw > 0 turns the view left, dpitch > 0 looks up.
    void add_angles(double dyaw, double dpitch, const LookParams& p);
    void reset() { yaw_ = pitch_ = 0; }
    double yaw() const { return yaw_; }
    double pitch() const { return pitch_; }
    bool active() const { return yaw_ != 0 || pitch_ != 0; }
private:
    double yaw_ = 0, pitch_ = 0;
};

// Turns the camera transform `in` about `pivot`: first by `yaw` about the world's up axis, then by `pitch` about the camera's own right axis
// (through the same pivot); pitch > 0 looks up. The position moves with the rotation; with the pivot at the camera's own position only the view turns.
void orbit_transform(const float in[12], const float pivot[3], double yaw, double pitch, float out[12]);

}  // namespace nxrt::play
