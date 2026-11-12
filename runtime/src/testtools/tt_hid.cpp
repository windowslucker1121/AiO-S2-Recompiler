// SPDX-License-Identifier: GPL-3.0-or-later
// The adapter between the test tooling and the hid service's input source (S3c: include/nxrt/input_source.h, on the `services` branch).
// Compiled to no-ops when that header is not in the tree. The wrapping source:
//   - passes the live source through untouched (record-only or no pad override), and remembers it for sampling the live pad at frame boundaries;
//   - or replaces player 1's pad with the replayed / scripted pad (everything else from the live source).
// It installs itself lazily at every frame boundary (install_input), after whoever set the live source, and re-wraps if the live source changed.
#include "tt_internal.h"

#if __has_include("nxrt/input_source.h")
#include <memory>
#include <mutex>

#include "nxrt/input_source.h"

namespace nxrt::tt::detail {
namespace {

std::mutex g_mu;
PadFrame g_pad;
bool g_override = false;
bool g_merge = false;

class TtSource : public hid::InputSource {
public:
    std::shared_ptr<hid::InputSource> live;
    void poll(hid::InputSnapshot& out) override {
        std::shared_ptr<hid::InputSource> l = live;
        if (l) l->poll(out);
        PadFrame p;
        bool ov, merge;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            p = g_pad;
            ov = g_override;
            merge = g_merge;
        }
        if (!ov) return;
        hid::NpadInput& n = out.npad[0];
        n.connected = !hid::player1_unplugged();   // [test] unplug_player1_at_s holds player 1 off until the controller applet answers
        n.style = hid::NpadStyle::FullKey;
        if (merge) {
            // script with key steps: the live pad (the play layer's output for the keys) stays, the script adds its buttons; a stick the
            // script leaves at (0, 0) keeps the live stick
            n.buttons |= p.buttons;
            if (p.lx != 0 || p.ly != 0) { n.left.x = p.lx; n.left.y = p.ly; }
            if (p.rx != 0 || p.ry != 0) { n.right.x = p.rx; n.right.y = p.ry; }
        } else {
            n.buttons = p.buttons;
            n.left.x = p.lx; n.left.y = p.ly;
            n.right.x = p.rx; n.right.y = p.ry;
        }
        out.mouse.dx = static_cast<int32_t>(p.mouse_dx);
        out.mouse.dy = static_cast<int32_t>(p.mouse_dy);
    }
};

std::shared_ptr<TtSource> g_src;

}  // namespace

void install_input() {
    if (!g_src) g_src = std::make_shared<TtSource>();
    std::shared_ptr<hid::InputSource> cur = hid::input_source();
    if (cur.get() == g_src.get()) return;
    g_src->live = cur;
    hid::set_input_source(g_src);
}

bool sample_live(PadFrame* out) {
    if (!g_src) return false;
    std::shared_ptr<hid::InputSource> l = g_src->live;
    if (!l) return false;
    hid::InputSnapshot snap;
    l->poll(snap);
    const hid::NpadInput& n = snap.npad[0];
    PadFrame p;
    p.buttons = n.buttons;
    p.lx = static_cast<int16_t>(n.left.x); p.ly = static_cast<int16_t>(n.left.y);
    p.rx = static_cast<int16_t>(n.right.x); p.ry = static_cast<int16_t>(n.right.y);
    p.mouse_dx = static_cast<float>(snap.mouse.dx);
    p.mouse_dy = static_cast<float>(snap.mouse.dy);
    *out = p;
    return true;
}

void publish_pad(const PadFrame& p, bool override_live, bool merge_live) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pad = p;
    g_override = override_live;
    g_merge = override_live && merge_live;
}

}  // namespace nxrt::tt::detail
#else
namespace nxrt::tt::detail {
void install_input() {}
bool sample_live(PadFrame*) { return false; }
void publish_pad(const PadFrame&, bool, bool) {}
}  // namespace nxrt::tt::detail
#endif
