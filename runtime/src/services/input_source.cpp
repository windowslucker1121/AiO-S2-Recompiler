// SPDX-License-Identifier: GPL-3.0-or-later
// The input source the hid service reads (include/nxrt/input_source.h), and the vibration sink the hid service forwards to.
#include <atomic>
#include <mutex>
#include <vector>

#include "nxrt/input_source.h"
#include "services_internal.h"

namespace nxrt::hid {

namespace {
std::mutex g_mu;
std::shared_ptr<InputSource> g_src;
// The vibration sink and its user pointer. The sink is called with g_vib_mu held, so after set_vibration_sink() returns no call is in flight;
// a sink must not call set_vibration_sink() itself.
std::mutex g_vib_mu;
VibrationSink g_vib_fn = nullptr;
void* g_vib_user = nullptr;
}  // namespace

void set_input_source(std::shared_ptr<InputSource> src) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_src = std::move(src);
}

std::shared_ptr<InputSource> input_source() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_src;
}

void set_vibration_sink(VibrationSink fn, void* user) {
    std::lock_guard<std::mutex> lk(g_vib_mu);
    g_vib_fn = fn;
    g_vib_user = user;
}

namespace {
std::mutex g_players_mu;
std::vector<uint32_t> g_player_pads;
std::atomic<bool> g_p1_unplugged{false};
}  // namespace

void set_player1_unplugged(bool on) { g_p1_unplugged.store(on); }
bool player1_unplugged() { return g_p1_unplugged.load(); }

void set_player_pads(const std::vector<uint32_t>& pad_ids) {
    std::lock_guard<std::mutex> lk(g_players_mu);
    g_player_pads = pad_ids;
    if (g_player_pads.size() > kNpadPlayers - 1) g_player_pads.resize(kNpadPlayers - 1);
}

std::vector<uint32_t> player_pads() {
    std::lock_guard<std::mutex> lk(g_players_mu);
    return g_player_pads;
}

}  // namespace nxrt::hid

namespace nxrt::services {

// Called by the hid service after it has stored a vibration value for one handle. Does nothing when no sink is registered.
void vibration_dispatch(uint32_t npad_slot, int hand, const nxrt::hid::VibrationValue& v) {
    std::lock_guard<std::mutex> lk(nxrt::hid::g_vib_mu);
    if (nxrt::hid::g_vib_fn) nxrt::hid::g_vib_fn(npad_slot, hand, v, nxrt::hid::g_vib_user);
}

}  // namespace nxrt::services
