// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the binding engine. Keyboard keys, mouse buttons and wheel notches go in with timestamps; the Switch-pad contribution
// (buttons and stick deflection) comes out for any moment in time. No SDL, no game memory, no threads of its own: the caller supplies the
// clock, so the tests drive it with made-up times.
//
// Rules (docs/S8.md has the player-facing version):
//  * Bindings of the current context win over bindings of the `all` layer for the same input. `none` as the action switches the input off.
//  * Triggers: press = active while held. tap = a pulse of [timing] pulse_ms right after a release that came within the tap window.
//    hold = active while held, from the hold delay on. toggle = each press flips it. doubletap = active while held when the press came
//    within the window after the previous release of the same input. A mouse wheel notch is a pulse for every trigger except toggle (which flips).
//  * Set A / set B selects (title E: beams / visors) and the wheel cycling are edge actions: when the binding becomes active the select output is
//    pressed for pulse_ms, however long the key stays down. SetANext / SetAPrev walk the tracked set-A member (the last one selected through
//    the engine or told by set_cycle_hint) over the members in the hint mask.
//  * Changing the context consumes every held input whose bindings differ in the new context: it does nothing until it is released.
//  * Stick contributions are summed per stick and normalised to length 1 (W + D is a diagonal, not 1.41).
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "play_types.h"

namespace nxrt::play {

struct EngineOut {
    uint64_t buttons = 0;       // nn::hid::NpadButton bits
    float lx = 0, ly = 0;       // -1..1, y up = +
    float rx = 0, ry = 0;
};

class Engine {
public:
    Engine();
    explicit Engine(const Settings& s);

    // Replaces bindings, outputs and timing; keeps what is held (held inputs are consumed if their bindings changed).
    void set_settings(const Settings& s, uint64_t now_ns);
    Settings settings() const;

    void set_context(Ctx c, uint64_t now_ns);
    Ctx context() const;

    // Input events. Times are nanoseconds on any monotonic clock; they must not go backwards between calls.
    void key(int scancode, bool down, uint64_t now_ns);
    void mouse_button(int button, bool down, uint64_t now_ns);    // 1 left .. 5, the numbering of input.h
    void wheel(int dir, uint64_t now_ns);                          // +1 up, -1 down: one notch
    void release_all(uint64_t now_ns);                             // focus lost: everything is released (no taps fire)
    void pulse(Act a, uint64_t now_ns, int ms = 0);                // ms 0 = [timing] pulse_ms
    void set_aux_sticks(float lx, float ly, float rx, float ry);   // added to the sticks (the menu pointer dragging the map); 0 = none                            // one press of the action's console output for [timing] pulse_ms (the menu pointer's click on a plain screen)
    // One call per game frame (the frame-boundary hook). Once it has been called, a select pulse also stays on for at least `kPulseFrames` frame
    // boundaries after the first poll that reported it: the game samples its input once per tick and other readers poll the pad in between, so
    // a pulse that is only a few milliseconds of one frame can be eaten before the tick sees it.
    void frame_tick();

    // What the game knows: the set-A member currently selected (0..3 = SetA1..SetA4; -1 = unknown) and a bit mask of the members the player
    // can select (title E: the beam in use and the beams owned).
    // The game's view is adopted only when no select of ours is pending or recent (the game needs a frame or two to act on a pulse).
    void set_cycle_hint(int current, unsigned mask, uint64_t now_ns);
    int tracked_cycle() const;

    // The Switch-side result at time now. Thread safe.
    EngineOut output(uint64_t now_ns);

    // Diagnostics for the settings page and the tests.
    bool input_held(const InputId& id) const;
    bool act_active(Act a, uint64_t now_ns);             // any binding of the action is active (select actions: the pulse is running)
    uint64_t activations(Act a) const;                   // activation edges since construction
    Act last_activated() const;

private:
    struct InState {
        bool held = false, consumed = false, had_release = false;
        uint64_t press_ns = 0, release_ns = 0;
    };
    struct BindRt {
        bool toggled = false, dt_on = false, prev = false;
        uint64_t pulse_until = 0;
    };
    void rebuild();                                       // eff_ and cur_ from settings_ + ctx_
    std::vector<int> effective(Ctx c, const InputId& id) const;
    std::vector<std::array<int, 3>> effective_sig(Ctx c, const InputId& id) const;   // (act, trigger, ms) per effective binding
    void press(const InputId& id, uint64_t now);
    void release(const InputId& id, uint64_t now);
    bool binding_active(int b, uint64_t now) const;
    void step(uint64_t now);
    void on_activate(int b, uint64_t now);
    void select(Act eff_act, uint64_t now);
    static bool is_select(Act a);

    mutable std::mutex mu_;
    Settings st_;
    Ctx ctx_ = Ctx::Menu;
    std::vector<BindRt> rt_;
    std::vector<int> cur_;                                // indices of the bindings that apply in ctx_
    std::map<InputId, InState> in_;
    // `seen` = a poll of output() has reported it: a pulse always lasts until the first poll at or after its end, so a slow game frame (100 ms or more)
    // cannot miss it, and only the head of the queue is ever reported (two selects never show in one poll).
    struct Sel { Act act; uint64_t start, end; bool seen = false; uint64_t seen_frame = 0; };
    std::vector<Sel> sels_;                               // select pulses, queued so that two selects never overlap
    uint64_t acts_[kActCount] = {};
    Act last_ = Act::None;
    int cycle_ = 0;
    uint64_t last_select_ns_ = 0;
    uint64_t frame_ = 0;
    bool frames_used_ = false;
    static constexpr uint64_t kPulseFrames = 2;
    unsigned cycle_mask_ = 0x0f;
    float aux_[4] = {};
};

}  // namespace nxrt::play
