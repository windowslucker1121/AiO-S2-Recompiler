// SPDX-License-Identifier: GPL-3.0-or-later
// Test fixture: title E's play-mode vocabulary and factory controls.ini, for the tests of the shared settings code.
//
// The play_config, play_engine, launcher_bindings_edit and launcher_tweaks groups check the base's settings reader, writer, binding engine and
// launcher keyboard tab through title E's words ("fire", "morph", "beam_1", the contexts gameplay / morph / scan, the
// [mouse] keys morph_yaw / ball_camera / wheel_beams) and its factory file (defaults_rev 3, the map's X added at revision 2, lock_mode moved
// to free at revision 3). That data lived in the game module (games/<id>/mpr_vocabulary.cpp, mpr_defaults.cpp), which this repository does
// not carry, so it is rebuilt here from what those tests expect. It is a fixture, not the game's real table: only its consistency with the
// tests matters.
//
// The groups register it with NXTEST_GROUP_FIXTURE(nxtest::mpr_play::install, nxtest::mpr_play::remove): every case then runs with a copy
// of the registered game whose play_defaults are these, whatever game the test executable registers (CMakeLists.txt NXRT_TEST_GAME).
#pragma once

namespace nxrt::play { struct GameDefaults; }

namespace nxtest::mpr_play {

const nxrt::play::GameDefaults& defaults();   // the vocabulary, the factory text, its revision, the additions and the changes
void install();                               // registers a copy of the current game with defaults() as its play_defaults
void remove();                                // registers the game that was current before install() again

}  // namespace nxtest::mpr_play
