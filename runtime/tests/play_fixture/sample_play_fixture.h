// SPDX-License-Identifier: GPL-3.0-or-later
// Test fixture: a sample game's play-mode vocabulary and factory controls.ini, for the tests of the shared settings code.
//
// The play_config, play_engine and play_sync groups check the base's settings reader, writer and binding engine through a game that names
// its own actions and contexts, the way a game module does (play_types.h GameVocabulary, GameDefaults): actions "attack", "target", "item",
// "vehicle", "weapon_1".."weapon_4" (set A, cycled by the wheel), "mode_1".."mode_4" (set B); contexts all, field, vehicle, map, menu, focus;
// the [mouse] / [pad] keys vehicle_yaw, vehicle_camera, wheel_weapons, vehicle_orbit ...; a factory file at defaults_rev 3 (X on the map
// added at revision 2, [mouse] capture moved from auto to click at revision 3). The sample is made up: only its consistency with the
// tests matters.
//
// The groups register it with NXTEST_GROUP_FIXTURE(nxtest::sample_play::install, nxtest::sample_play::remove): every case then runs with a
// copy of the registered game whose play_defaults are these, whatever game the test executable registers (CMakeLists.txt NXRT_TEST_GAME).
#pragma once

namespace nxrt::play { struct GameDefaults; }

namespace nxtest::sample_play {

const nxrt::play::GameDefaults& defaults();   // the vocabulary, the factory text, its revision, the additions and the changes
void install();                               // registers a copy of the current game with defaults() as its play_defaults
void remove();                                // registers the game that was current before install() again

}  // namespace nxtest::sample_play
