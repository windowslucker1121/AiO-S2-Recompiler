// SPDX-License-Identifier: GPL-3.0-or-later
// The library's Settings page (setup_settings.h): tabs, rows built from a table of the runtime's controls.ini keys, dark controls, the
// keyboard bindings editor with press-to-bind, and the ini edited in place.
#include "setup_settings.h"

#include <commctrl.h>
#include <mmsystem.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>

#include "keymap_win.h"
#include "setup_core.h"
#include "setup_theme.h"

#pragma comment(lib, "winmm.lib")   // the joystick API: controllers XInput does not see

namespace setup::settings {

using namespace setup::theme;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
// "[name]" -> "name" (lower case), else empty
std::string section_of(const std::string& line) {
    const std::string t = trim(line);
    if (t.size() < 2 || t[0] != '[') return {};
    const size_t e = t.find(']');
    return e == std::string::npos ? std::string() : lower(trim(t.substr(1, e - 1)));
}
bool key_line(const std::string& line, std::string* k, std::string* v) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return false;
    const size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    *k = trim(t.substr(0, eq));
    *v = trim(t.substr(eq + 1));
    return true;
}

}  // namespace

// ---- Ini ------------------------------------------------------------------------------------------------------------------------------
bool Ini::load(const std::wstring& path) {
    lines_.clear();
    if (!file_exists(path)) return false;
    std::istringstream in(read_file_text(path));
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines_.push_back(l);
    }
    return true;
}
std::string Ini::text() const {
    std::string t;
    for (const std::string& l : lines_) t += l + "\n";
    return t;
}
bool Ini::save(const std::wstring& path) const { return write_file_atomic(path, text()); }
int Ini::header(const std::string& section) const {
    const std::string s = lower(section);
    for (size_t i = 0; i < lines_.size(); ++i) if (section_of(lines_[i]) == s) return static_cast<int>(i);
    return -1;
}
int Ini::end_of(int h) const {
    size_t i = static_cast<size_t>(h) + 1;
    while (i < lines_.size() && section_of(lines_[i]).empty()) ++i;
    return static_cast<int>(i);
}
bool Ini::has(const std::string& section, const std::string& key) const {
    const int h = header(section);
    if (h < 0) return false;
    std::string k, v;
    for (int i = h + 1; i < end_of(h); ++i) if (key_line(lines_[static_cast<size_t>(i)], &k, &v) && lower(k) == lower(key)) return true;
    return false;
}
std::string Ini::get(const std::string& section, const std::string& key, const std::string& def) const {
    const int h = header(section);
    if (h < 0) return def;
    std::string k, v;
    for (int i = h + 1; i < end_of(h); ++i) if (key_line(lines_[static_cast<size_t>(i)], &k, &v) && lower(k) == lower(key)) return v;
    return def;
}
void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
    int h = header(section);
    if (h < 0) {
        if (!lines_.empty() && !trim(lines_.back()).empty()) lines_.push_back("");
        lines_.push_back("[" + section + "]");
        lines_.push_back(key + " = " + value);
        return;
    }
    std::string k, v;
    const int e = end_of(h);
    for (int i = h + 1; i < e; ++i)
        if (key_line(lines_[static_cast<size_t>(i)], &k, &v) && lower(k) == lower(key)) { lines_[static_cast<size_t>(i)] = k + " = " + value; return; }
    // after the section's last key line (blank lines and comments at its end stay below)
    int at = h + 1;
    for (int i = h + 1; i < e; ++i) if (key_line(lines_[static_cast<size_t>(i)], &k, &v)) at = i + 1;
    lines_.insert(lines_.begin() + at, key + " = " + value);
}
std::vector<std::string> Ini::sections() const {
    std::vector<std::string> out;
    for (const std::string& l : lines_) { const std::string s = section_of(l); if (!s.empty()) out.push_back(s); }
    return out;
}
std::vector<std::pair<std::string, std::string>> Ini::entries(const std::string& section) const {
    std::vector<std::pair<std::string, std::string>> out;
    const int h = header(section);
    if (h < 0) return out;
    std::string k, v;
    for (int i = h + 1; i < end_of(h); ++i) if (key_line(lines_[static_cast<size_t>(i)], &k, &v)) out.emplace_back(k, v);
    return out;
}
void Ini::replace_entries(const std::string& section, const std::vector<std::pair<std::string, std::string>>& e) {
    int h = header(section);
    if (h < 0) {
        if (!lines_.empty() && !trim(lines_.back()).empty()) lines_.push_back("");
        lines_.push_back("[" + section + "]");
        h = static_cast<int>(lines_.size()) - 1;
    }
    std::string k, v;
    std::vector<std::string> kept;   // the section's other lines (comments, blanks)
    const int end = end_of(h);
    for (int i = h + 1; i < end; ++i) if (!key_line(lines_[static_cast<size_t>(i)], &k, &v)) kept.push_back(lines_[static_cast<size_t>(i)]);
    // trailing blank lines go after the new entries
    std::vector<std::string> tail;
    while (!kept.empty() && trim(kept.back()).empty()) { tail.push_back(kept.back()); kept.pop_back(); }
    std::vector<std::string> body = kept;
    for (const auto& [a, b] : e) body.push_back(a + " = " + b);
    body.insert(body.end(), tail.begin(), tail.end());
    lines_.erase(lines_.begin() + h + 1, lines_.begin() + end);
    lines_.insert(lines_.begin() + h + 1, body.begin(), body.end());
}
void Ini::remove_section(const std::string& section) {
    const int h = header(section);
    if (h < 0) return;
    lines_.erase(lines_.begin() + h, lines_.begin() + end_of(h));
}

// ---- what the page offers ---------------------------------------------------------------------------------------------------------
namespace {

enum Tab { T_GRAPHICS, T_DISPLAY, T_PAD, T_KEYS, T_AUDIO, T_COUNT };
const wchar_t* kTabNames[T_COUNT] = {L"Graphics", L"Display", L"Controller", L"Keyboard & mouse", L"Audio"};

struct Choice {
    const char* value;
    const wchar_t* label;
};
// When a change reaches the game: live while it runs, or at its next start.
enum When { kLive, kNext };
struct Opt {
    int tab;
    const char* section;    // nullptr: a heading (label is its text)
    const char* key;        // "#size": the window size (width and height); "#mouse_speed": [mouse] yaw and pitch together
    const wchar_t* label;
    const wchar_t* hint;    // the tooltip
    const char* def;        // the runtime's default when the file has no value
    std::vector<Choice> choices;   // empty: an on/off switch ("1" / "0")
    When when = kLive;
};

const std::vector<Choice> kDeadzones = {{"0.05", L"5%"}, {"0.1", L"10%"}, {"0.15", L"15%"}, {"0.2", L"20%"}, {"0.25", L"25%"}};

// Every key here is read from controls.ini by the runtime (runtime/docs/CONFIG.md); the game's own menu writes the same keys.
const std::vector<Opt>& options() {
    static const std::vector<Opt> o = {
        // graphics
        {T_GRAPHICS, nullptr, nullptr, L"Upscaling", nullptr, nullptr, {}},
        {T_GRAPHICS, "upscale", "mode", L"Upscaler", L"How the picture is scaled to the window. DLSS and DLAA need an NVIDIA RTX card and the DLSS download.", "off",
         {{"off", L"Off"}, {"fsr1", L"AMD FSR 1"}, {"nis", L"NVIDIA Image Scaling"}, {"fsr3", L"AMD FSR 3"}, {"dlss", L"NVIDIA DLSS"}, {"dlaa", L"NVIDIA DLAA"}}},
        {T_GRAPHICS, "upscale", "sharpness", L"Sharpening", L"Sharpening of FSR and Image Scaling.", "0.8",
         {{"0", L"Off"}, {"0.25", L"Low"}, {"0.5", L"Medium"}, {"0.8", L"High"}, {"1", L"Maximum"}}},
        {T_GRAPHICS, nullptr, nullptr, L"Textures", nullptr, nullptr, {}},
        {T_GRAPHICS, "display", "anisotropy", L"Texture filtering", L"At least this much anisotropic filtering everywhere.", "0",
         {{"0", L"As the game asks"}, {"2", L"2x"}, {"4", L"4x"}, {"8", L"8x"}, {"16", L"16x"}}},
        {T_GRAPHICS, "display", "lod_bias", L"Texture sharpness", L"Sharper distant textures can shimmer.", "0",
         {{"-1", L"Sharpest"}, {"-0.5", L"Sharper"}, {"0", L"As the game"}, {"0.5", L"Softer"}}},
        {T_GRAPHICS, "display", "astc_quality", L"Texture compression", L"Uncompressed needs a lot of video memory.", "high",
         {{"fast", L"Fast"}, {"high", L"High"}, {"uncompressed", L"Uncompressed"}}, kNext},
        {T_GRAPHICS, nullptr, nullptr, L"Output", nullptr, nullptr, {}},
        {T_GRAPHICS, "hdr", "mode", L"HDR", L"Auto: HDR when the display and Windows have it on.", "auto",
         {{"off", L"Off"}, {"auto", L"Auto"}, {"on", L"On"}, {"native", L"On, original brightness"}}, kNext},
        {T_GRAPHICS, nullptr, nullptr, L"Performance", nullptr, nullptr, {}},
        {T_GRAPHICS, "display", "shader_prewarm", L"Prepare shaders ahead", L"Builds the shaders of earlier runs before the game needs them.", "1", {}, kNext},
        {T_GRAPHICS, "display", "fast_gpu_memory", L"Faster GPU memory", L"Keeps copies of game data in video memory (up to 2 GB).", "1", {}, kNext},
        {T_GRAPHICS, "display", "fewer_gpu_waits", L"Fewer GPU waits", L"Lets the GPU run further ahead.", "1", {}, kNext},
        // display
        {T_DISPLAY, nullptr, nullptr, L"Window", nullptr, nullptr, {}},
        {T_DISPLAY, "display", "window_mode", L"Window mode", L"Alt+Enter switches while playing.", "windowed",
         {{"windowed", L"Windowed"}, {"borderless", L"Borderless fullscreen"}, {"exclusive", L"Exclusive fullscreen"}}},
        {T_DISPLAY, "display", "#size", L"Window size", L"Fullscreen uses the desktop resolution.", "1280x720",
         {{"1280x720", L"1280 × 720"}, {"1600x900", L"1600 × 900"}, {"1920x1080", L"1920 × 1080"}, {"2560x1080", L"2560 × 1080"},
          {"2560x1440", L"2560 × 1440"}, {"3440x1440", L"3440 × 1440"}, {"3840x2160", L"3840 × 2160"}}},
        {T_DISPLAY, "display", "monitor", L"Display", nullptr, "auto", {{"auto", L"Where the window opens"}, {"1", L"Display 1"}, {"2", L"Display 2"}, {"3", L"Display 3"}}},
        {T_DISPLAY, "display", "keep_aspect", L"Keep the aspect ratio", L"Bars instead of a stretched picture.", "1", {}, kNext},
        {T_DISPLAY, nullptr, nullptr, L"Frame rate", nullptr, nullptr, {}},
        {T_DISPLAY, "display", "fps_limit", L"Frame rate limit", L"Auto: up to 100 fps; the console's rate for games with a fixed step.", "auto",
         {{"auto", L"Auto"}, {"30", L"30 fps"}, {"60", L"60 fps"}, {"100", L"100 fps"}, {"120", L"120 fps"}, {"144", L"144 fps"}, {"165", L"165 fps"}, {"240", L"240 fps"}, {"0", L"Unlimited"}}},        {T_DISPLAY, "display", "vsync", L"VSync", L"With G-Sync or FreeSync, leave it off and set a limit.", "0", {}},
        {T_DISPLAY, "fps", "show", L"Frame rate counter", L"F2 shows and hides it while playing.", "0", {}},
        {T_DISPLAY, "fps", "detail", L"Counter detail", nullptr, "1", {{"0", L"Frame rate"}, {"1", L"Frame rate and frame time"}, {"2", L"Also 1% low and worst frame"}}},
        {T_DISPLAY, "fps", "corner", L"Counter position", nullptr, "top_left",
         {{"top_left", L"Top left"}, {"top_right", L"Top right"}, {"bottom_left", L"Bottom left"}, {"bottom_right", L"Bottom right"}}},
        // controller (the button mapping rows are made from the pad's own controls, not from this table)
        {T_PAD, nullptr, nullptr, L"Controller", nullptr, nullptr, {}},
        {T_PAD, "pad", "label_abxy", L"Face buttons", L"By letter: the button marked A is the game's A. By position: the bottom button confirms as on the original controller.",
         "1", {{"1", L"By letter"}, {"0", L"By position"}}},
        {T_PAD, "pad", "deadzone", L"Left stick dead zone", nullptr, "0.1", kDeadzones},
        {T_PAD, "pad", "right_deadzone", L"Right stick dead zone", nullptr, "0.1", kDeadzones},
        {T_PAD, "pad", "look_sens", L"Camera speed", nullptr, "1", {{"0.5", L"Slow"}, {"0.75", L"Slower"}, {"1", L"Normal"}, {"1.5", L"Faster"}, {"2", L"Fast"}}},
        {T_PAD, "pad", "invert_y", L"Invert camera up and down", nullptr, "0", {}},
        {T_PAD, "pad", "trigger_threshold", L"Trigger press point", nullptr, "0.35", {{"0.15", L"Light"}, {"0.35", L"Normal"}, {"0.6", L"Deep"}}},
        {T_PAD, "pad", "rumble", L"Rumble", nullptr, "1", {}},
        {T_PAD, "pad", "rumble_scale", L"Rumble strength", nullptr, "1", {{"0.25", L"Weak"}, {"0.5", L"Medium"}, {"1", L"Normal"}, {"1.5", L"Strong"}}},
        {T_PAD, "pad", "gyro", L"Motion controls", L"Gyro aiming on controllers that have one.", "1", {}},
        {T_PAD, "prompts", "style", L"Button icons in the game", L"Which controller's buttons the game shows in its hints.", "auto",
         {{"auto", L"The controller used last"}, {"console", L"The game's own"}, {"xbox", L"Xbox"}, {"playstation", L"PlayStation"}, {"keyboard", L"Keyboard"}}},
        // keyboard and mouse: the mouse options, then the key bindings (not from this table)
        {T_KEYS, nullptr, nullptr, L"Mouse", nullptr, nullptr, {}},
        {T_KEYS, "mouse", "capture", L"Mouse camera", L"Click: the game takes the mouse after a click into the window; the release key gives it back.", "click",
         {{"click", L"After a click"}, {"auto", L"Whenever the window is active"}, {"off", L"Never"}}},
        {T_KEYS, "mouse", "release_key", L"Release key", L"Gives the mouse back to Windows. Alt+Tab always does.", "Left Alt", {}},
        {T_KEYS, "mouse", "#mouse_speed", L"Mouse camera speed", nullptr, "0.000767945",
         {{"0.0004", L"Slow"}, {"0.00055", L"Slower"}, {"0.000767945", L"Normal"}, {"0.0011", L"Faster"}, {"0.0016", L"Fast"}}},
        {T_KEYS, "mouse", "invert_y", L"Invert mouse up and down", nullptr, "0", {}},
        // audio
        {T_AUDIO, nullptr, nullptr, L"Sound", nullptr, nullptr, {}},
        {T_AUDIO, "audio", "volume", L"Volume", nullptr, "1",
         {{"0", L"Muted"}, {"0.1", L"10%"}, {"0.25", L"25%"}, {"0.5", L"50%"}, {"0.75", L"75%"}, {"0.9", L"90%"}, {"1", L"100%"}}},
        {T_AUDIO, "audio", "mute_unfocused", L"Mute in the background", L"Silent while another window is active.", "0", {}},
    };
    return o;
}
bool is_keycap(const Opt& o) { return std::string(o.section ? o.section : "") == "mouse" && std::string(o.key) == "release_key"; }

// What a key can do: the runtime's generic action names (play_types.cpp).
const std::vector<Choice>& actions() {
    static const std::vector<Choice> a = {
        {"move_forward", L"Move forward"}, {"move_back", L"Move back"}, {"move_left", L"Move left"}, {"move_right", L"Move right"},
        {"look_up", L"Camera up"}, {"look_down", L"Camera down"}, {"look_left", L"Camera left"}, {"look_right", L"Camera right"},
        {"btn_a", L"Button A"}, {"btn_b", L"Button B"}, {"btn_x", L"Button X"}, {"btn_y", L"Button Y"},
        {"btn_l", L"Left shoulder"}, {"btn_r", L"Right shoulder"}, {"btn_zl", L"Left trigger"}, {"btn_zr", L"Right trigger"},
        {"btn_plus", L"Start / menu"}, {"btn_minus", L"Select / map"}, {"btn_lstick", L"Left stick press"}, {"btn_rstick", L"Right stick press"},
        {"dpad_up", L"D-pad up"}, {"dpad_down", L"D-pad down"}, {"dpad_left", L"D-pad left"}, {"dpad_right", L"D-pad right"},
    };
    return a;
}

const std::map<int, const char*>& key_names() {
    static const std::map<int, const char*> m = [] {
        std::map<int, const char*> r;
        const std::pair<int, const char*> t[] = {
#include "setup_keynames.inc"
        };
        for (const auto& p : t) r.emplace(p.first, p.second);
        return r;
    }();
    return m;
}

bool numeric_equal(const std::string& a, const std::string& b) {
    if (lower(a) == lower(b)) return true;
    char* e1 = nullptr;
    char* e2 = nullptr;
    const double x = std::strtod(a.c_str(), &e1), y = std::strtod(b.c_str(), &e2);
    return e1 && *e1 == 0 && e2 && *e2 == 0 && e1 != a.c_str() && e2 != b.c_str() && std::fabs(x - y) < 1e-6;
}

// A binding's key as the player reads it: "Mouse1" -> "Left mouse button", "WheelUp" -> "Wheel up", key names as SDL writes them.
std::wstring key_text(const std::string& k) {
    if (k == "Mouse1") return L"Left mouse";
    if (k == "Mouse2") return L"Middle mouse";
    if (k == "Mouse3") return L"Right mouse";
    if (k == "Mouse4") return L"Mouse back";
    if (k == "Mouse5") return L"Mouse forward";
    if (k == "WheelUp") return L"Wheel up";
    if (k == "WheelDown") return L"Wheel down";
    return widen(k);
}

// ---- press-to-bind --------------------------------------------------------------------------------------------------------------------
struct Capture {
    std::string result;
    bool done = false;
    int left = 8;
    Fonts fonts;
    std::wstring what;
    RECT cancel{};   // a click here cancels instead of binding the mouse button
    bool hot = false;
};
LRESULT CALLBACK capture_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* c = reinterpret_cast<Capture*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    if (!c) return DefWindowProcW(h, m, w, l);
    auto finish = [&](const std::string& r) { c->result = r; c->done = true; };
    switch (m) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            const int sc = mprl::sdl_scancode_from_key_message(w, l);
            auto it = key_names().find(sc);
            if (it != key_names().end()) finish(it->second);
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (m == WM_LBUTTONDOWN && PtInRect(&c->cancel, p)) { finish({}); return 0; }
            const int b = mprl::sdl_mouse_button_from_message(m, w);
            if (b) finish("Mouse" + std::to_string(b));
            return 0;
        }
        case WM_MOUSEMOVE: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            const bool hot = PtInRect(&c->cancel, p) != 0;
            if (hot != c->hot) { c->hot = hot; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int n = mprl::wheel_notch_from_message(w);
            if (n) finish(n > 0 ? "WheelUp" : "WheelDown");
            return 0;
        }
        case WM_TIMER:
            if (--c->left <= 0) finish({});
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_CLOSE: case WM_CANCELMODE: finish({}); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            const float k = static_cast<float>(c->fonts.dpi) / 96.f;
            paint_buffered(dc, rc, [&](gp::Graphics& g) {
                gp::SolidBrush bg(C(kCard));
                g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
                gp::Pen pen(C(kAccent), 2 * k);
                gp::GraphicsPath p;
                round_path(p, gp::RectF(k, k, static_cast<float>(rc.right) - 2 * k, static_cast<float>(rc.bottom) - 2 * k), 12 * k);
                g.DrawPath(&pen, &p);
                HDC fdc = g.GetHDC();
                gp::Font title(fdc, c->fonts.title), font(fdc, c->fonts.font), bold(fdc, c->fonts.bold), little(fdc, c->fonts.little);
                g.ReleaseHDC(fdc);
                const float W = static_cast<float>(rc.right);
                text(g, L"Press a key or a mouse button", title, gp::RectF(0, 22 * k, W, 34 * k), kText, gp::StringAlignmentCenter);
                text(g, c->what, bold, gp::RectF(0, 62 * k, W, 22 * k), kAccentHi, gp::StringAlignmentCenter);
                // countdown bar
                gp::SolidBrush track(C(kLine)), fill(C(kAccent));
                const float bw = W - 120 * k;
                fill_round(g, gp::RectF(60 * k, 100 * k, bw, 4 * k), 2 * k, track);
                fill_round(g, gp::RectF(60 * k, 100 * k, std::max(4 * k, bw * static_cast<float>(c->left) / 8.f), 4 * k), 2 * k, fill);
                // Cancel
                const gp::RectF cr(W / 2 - 60 * k, 120 * k, 120 * k, 34 * k);
                c->cancel = {static_cast<LONG>(cr.X), static_cast<LONG>(cr.Y), static_cast<LONG>(cr.X + cr.Width), static_cast<LONG>(cr.Y + cr.Height)};
                gp::SolidBrush cb(C(c->hot ? kButtonHi : kButton));
                fill_round(g, cr, 8 * k, cb);
                text(g, L"Cancel", bold, cr, kText, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
                (void)font;
                (void)little;
            });
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

std::string capture_input(HWND owner, const Fonts& fonts, const std::wstring& what) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = capture_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"NxrtKeyCapture";
        RegisterClassExW(&wc);
        registered = true;
    }
    Capture c;
    c.fonts = fonts;
    c.what = what;
    HWND root = GetAncestor(owner, GA_ROOT);
    RECT orc;
    GetWindowRect(root, &orc);
    const int w = MulDiv(460, fonts.dpi, 96), h = MulDiv(172, fonts.dpi, 96);
    HWND wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"NxrtKeyCapture", L"Press a key", WS_POPUP | WS_VISIBLE,
                               (orc.left + orc.right - w) / 2, (orc.top + orc.bottom - h) / 2, w, h, root, nullptr, inst, &c);
    const HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, MulDiv(24, fonts.dpi, 96), MulDiv(24, fonts.dpi, 96));
    SetWindowRgn(wnd, rgn, FALSE);
    EnableWindow(root, FALSE);
    SetForegroundWindow(wnd);
    SetFocus(wnd);
    SetCapture(wnd);
    SetTimer(wnd, 1, 1000, nullptr);
    MSG msg;
    while (!c.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == wnd || IsChild(wnd, msg.hwnd) || msg.message == WM_TIMER || msg.message == WM_PAINT) {
            // keys go straight to the capture window: no accelerator or dialog handling may eat them (Tab, Escape, F10)
            DispatchMessageW(&msg);
        } else if (msg.message < WM_KEYFIRST || msg.message > WM_MOUSELAST) {
            DispatchMessageW(&msg);
        }
    }
    ReleaseCapture();
    EnableWindow(root, TRUE);
    DestroyWindow(wnd);
    SetForegroundWindow(root);
    return c.result;
}

// ---- controllers: XInput (Xbox and compatible) and the Windows joystick API (other pads), loaded or linked without extra DLLs ------------
struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XVib { WORD left, right; };
struct XInputApi {
    DWORD(WINAPI* get)(DWORD, XState*) = nullptr;
    DWORD(WINAPI* set)(DWORD, XVib*) = nullptr;
    XInputApi() {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        if (!m) return;
        get = reinterpret_cast<decltype(get)>(GetProcAddress(m, "XInputGetState"));
        set = reinterpret_cast<decltype(set)>(GetProcAddress(m, "XInputSetState"));
    }
};

// Which buttons the pictures show: the connected pad's, or the player's choice ([library] pad_icons).
enum class PadKind { xbox, playstation, generic };

// One physical control of the diagram: its [padmap] key, button mask (XInput layout; 0: a trigger, trig 1 left / 2 right), box in diagram
// units (96 dpi), and the console button it gives by default ("A" is decided by the face-button layout).
struct PadShape { const char* key; WORD mask; int trig; float x, y, w, h; bool round; };
const PadShape kShapes[] = {
    {"left_trigger", 0, 1, 30, 0, 90, 22, false},    {"right_trigger", 0, 2, 300, 0, 90, 22, false},
    {"left_shoulder", 0x0100, 0, 30, 26, 90, 22, false}, {"right_shoulder", 0x0200, 0, 300, 26, 90, 22, false},
    {"left_stick", 0x0040, 0, 48, 74, 60, 60, true},  {"right_stick", 0x0080, 0, 232, 170, 60, 60, true},
    {"back", 0x0020, 0, 166, 92, 26, 22, true},       {"start", 0x0010, 0, 228, 92, 26, 22, true},
    {"north", 0x8000, 0, 314, 60, 30, 30, true},      {"west", 0x4000, 0, 282, 92, 30, 30, true},
    {"east", 0x2000, 0, 346, 92, 30, 30, true},       {"south", 0x1000, 0, 314, 124, 30, 30, true},
    {"dpad_up", 1, 0, 138, 168, 24, 24, false},       {"dpad_down", 2, 0, 138, 216, 24, 24, false},
    {"dpad_left", 4, 0, 114, 192, 24, 24, false},     {"dpad_right", 8, 0, 162, 192, 24, 24, false},
};
constexpr int kPadH = 270;    // height of the diagram area (96 dpi)
constexpr UINT_PTR kPadTimer = 77, kRumbleTimer = 78, kWatchTimer = 79;

// The console button a control gives without a mapping: the [padmap] value the runtime would use (pad_map.cpp pad_default_buttons).
// PlayStation buttons have no letters, so they always go by position (the runtime does the same).
std::string default_target(const std::string& key, bool by_letter, PadKind kind) {
    const bool letter = by_letter && kind != PadKind::playstation;
    if (key == "south") return letter ? "A" : "B";
    if (key == "east") return letter ? "B" : "A";
    if (key == "west") return letter ? "X" : "Y";
    if (key == "north") return letter ? "Y" : "X";
    static const std::pair<const char*, const char*> t[] = {{"left_shoulder", "L"}, {"right_shoulder", "R"}, {"left_trigger", "ZL"}, {"right_trigger", "ZR"},
                                                            {"start", "PLUS"}, {"back", "MINUS"}, {"left_stick", "LSTICK"}, {"right_stick", "RSTICK"},
                                                            {"dpad_up", "UP"}, {"dpad_down", "DOWN"}, {"dpad_left", "LEFT"}, {"dpad_right", "RIGHT"}};
    for (const auto& p : t) if (key == p.first) return p.second;
    return {};
}

// A control's name on the player's pad.
std::wstring control_name(const std::string& key, PadKind kind) {
    struct N { const char* key; const wchar_t* xbox; const wchar_t* ps; const wchar_t* gen; };
    static const N t[] = {
        {"south", L"A", L"Cross", L"Bottom button"},           {"east", L"B", L"Circle", L"Right button"},
        {"west", L"X", L"Square", L"Left button"},             {"north", L"Y", L"Triangle", L"Top button"},
        {"left_shoulder", L"LB", L"L1", L"Left bumper"},       {"right_shoulder", L"RB", L"R1", L"Right bumper"},
        {"left_trigger", L"LT", L"L2", L"Left trigger"},       {"right_trigger", L"RT", L"R2", L"Right trigger"},
        {"start", L"Menu", L"Options", L"Start"},              {"back", L"View", L"Create", L"Select"},
        {"left_stick", L"Left stick press", L"L3", L"Left stick press"}, {"right_stick", L"Right stick press", L"R3", L"Right stick press"},
        {"dpad_up", L"D-pad up", L"D-pad up", L"D-pad up"},    {"dpad_down", L"D-pad down", L"D-pad down", L"D-pad down"},
        {"dpad_left", L"D-pad left", L"D-pad left", L"D-pad left"}, {"dpad_right", L"D-pad right", L"D-pad right", L"D-pad right"},
    };
    for (const N& n : t)
        if (key == n.key) return kind == PadKind::xbox ? n.xbox : kind == PadKind::playstation ? n.ps : n.gen;
    return widen(key);
}
// The short text on a control in the picture.
std::wstring control_cap(const std::string& key, PadKind kind) {
    if (key == "left_stick") return kind == PadKind::playstation ? L"L3" : L"LS";
    if (key == "right_stick") return kind == PadKind::playstation ? L"R3" : L"RS";
    if (key == "dpad_up") return L"▲";
    if (key == "dpad_down") return L"▼";
    if (key == "dpad_left") return L"◀";
    if (key == "dpad_right") return L"▶";
    if (kind == PadKind::generic) {
        if (key == "start") return L"≡";
        if (key == "back") return L"⧉";
        if (key == "left_shoulder") return L"L1";
        if (key == "right_shoulder") return L"R1";
        if (key == "left_trigger") return L"L2";
        if (key == "right_trigger") return L"R2";
        return L"";
    }
    if (kind == PadKind::xbox && key == "start") return L"≡";
    if (kind == PadKind::xbox && key == "back") return L"⧉";
    if (kind == PadKind::playstation && (key == "start" || key == "back")) return key == "start" ? L"≡" : L"∙";
    return control_name(key, kind);
}
bool is_face(const std::string& k) { return k == "south" || k == "east" || k == "west" || k == "north"; }

// Draws one control: a face button gets its pad's look (Xbox letters in their colours, the PlayStation shapes, a dot for the position on
// other pads), everything else its short name.
void draw_control(gp::Graphics& g, const PadShape& s, PadKind kind, const gp::RectF& r, bool on, bool hot, const gp::Font& font, const gp::Font& bold,
                  float k) {
    const std::string key = s.key;
    gp::SolidBrush fill(C(on ? kAccent : hot ? kCardHi : kCard));
    gp::Pen edge(C(on ? kAccentHi : hot ? kMuted : kLine), 1.5f * k);
    if (s.round) { g.FillEllipse(&fill, r); g.DrawEllipse(&edge, r); }
    else { fill_round(g, r, 6 * k, fill); gp::GraphicsPath p; round_path(p, r, 6 * k); g.DrawPath(&edge, &p); }
    if (is_face(key)) {
        const float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, q = r.Width * 0.24f;
        if (kind == PadKind::xbox) {
            const COLORREF col = key == "south" ? RGB(0x6C, 0xC6, 0x4F) : key == "east" ? RGB(0xEE, 0x55, 0x55) : key == "west" ? RGB(0x4C, 0x9A, 0xF0)
                                                                                                                                : RGB(0xF2, 0xC1, 0x3C);
            text(g, control_name(key, kind), bold, r, on ? RGB(255, 255, 255) : col, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        } else if (kind == PadKind::playstation) {
            const COLORREF col = key == "south" ? RGB(0x8C, 0xB4, 0xF2) : key == "east" ? RGB(0xF0, 0x74, 0x84) : key == "west" ? RGB(0xE0, 0x96, 0xD6)
                                                                                                                                : RGB(0x52, 0xD6, 0xB8);
            gp::Pen p(C(on ? RGB(255, 255, 255) : col), 2.f * k);
            if (key == "south") { g.DrawLine(&p, cx - q, cy - q, cx + q, cy + q); g.DrawLine(&p, cx + q, cy - q, cx - q, cy + q); }
            else if (key == "east") g.DrawEllipse(&p, cx - q, cy - q, 2 * q, 2 * q);
            else if (key == "west") g.DrawRectangle(&p, cx - q, cy - q, 2 * q, 2 * q);
            else { const gp::PointF t[3] = {{cx, cy - q * 1.1f}, {cx + q * 1.1f, cy + q * 0.8f}, {cx - q * 1.1f, cy + q * 0.8f}}; g.DrawPolygon(&p, t, 3); }
        } else {
            // a dot where the button sits in the diamond
            const float dx = key == "east" ? q : key == "west" ? -q : 0, dy = key == "south" ? q : key == "north" ? -q : 0;
            gp::SolidBrush dot(C(on ? RGB(255, 255, 255) : kText));
            g.FillEllipse(&dot, cx + dx - 3 * k, cy + dy - 3 * k, 6 * k, 6 * k);
        }
        return;
    }
    text(g, control_cap(key, kind), r.Width < 34 * k ? bold : font, r, on ? RGB(255, 255, 255) : kText, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
}

// ---- the panel ------------------------------------------------------------------------------------------------------------------------
enum class RowKind { heading, option, binding_head, binding, add_binding, note, pad_test, dlss_dl, padmap };
struct Row {
    RowKind kind;
    int opt = -1;                   // options() index
    std::string section;            // bindings section; padmap: the control's key
    int bind = -1;                  // index into that section's entries
    std::wstring textv;             // heading / note text
    int y = 0, h = 0;               // in content coordinates (pixels)
    HWND ctl = nullptr, ctl2 = nullptr, ctl3 = nullptr;
    bool on = false;                // switch state
    std::vector<std::string> values;   // combo item values
};

constexpr int kIdBase = 5000;

class Panel {
public:
    explicit Panel(HWND h) : wnd_(h) { field_ = CreateSolidBrush(kField); }
    ~Panel() { DeleteObject(field_); }
    LRESULT on(UINT m, WPARAM w, LPARAM l);
    void set_fonts(const Fonts& f) { fonts_ = f; rebuild(); }
    void set_dlss(int state, std::function<void()> dl) { dlss_ = state; dlss_dl_ = std::move(dl); }
    void set_tab(int t) {
        if (t < 0 || t >= T_COUNT) return;
        tab_ = t;
        scroll_ = 0;
        saved_note_.clear();
        rebuild();
    }
    void set_game(const std::wstring& title, const std::wstring& path, const std::wstring& why) {
        title_ = title;
        path_ = path;
        why_ = why;
        ini_ = Ini();
        if (!path_.empty()) exists_ = ini_.load(path_);
        stamp_ = file_stamp();
        scroll_ = 0;
        rebuild();
        if (!path_.empty()) SetTimer(wnd_, kWatchTimer, 1000, nullptr);
        else KillTimer(wnd_, kWatchTimer);
    }

private:
    int S(int v) const { return MulDiv(v, fonts_.dpi, 96); }
    float Sf(float v) const { return v * static_cast<float>(fonts_.dpi) / 96.f; }
    void rebuild();
    void rebuild_keep_scroll();
    void place();
    void paint(HDC dc);
    void save();
    void fresh();                         // the file as it is on disk now (another program may have changed it): read before every change
    std::string file_stamp() const;
    HWND make_combo(Row& r, const std::vector<std::pair<std::string, std::wstring>>& choices, const std::string& current);
    HWND make_combo(Row& r, const std::vector<Choice>& choices, const std::string& current);
    HWND make_button(const wchar_t* text, int style_kind);
    void tip(HWND ctl, const wchar_t* text);
    std::string value_of(const Opt& o) const;
    void command(int id, int code, HWND ctl);
    void draw_item(const DRAWITEMSTRUCT* d);
    void set_bindings(const std::string& section, const std::vector<std::pair<std::string, std::string>>& e);
    void paint_pad(gp::Graphics& g, float oy, const gp::Font& font, const gp::Font& bold, const gp::Font& little);
    void poll_pad();
    PadKind pad_kind() const;
    std::vector<std::pair<std::string, std::wstring>> targets(const std::string& key) const;   // a control's choices: value, text
    std::wstring target_text(const std::string& value) const;   // a [padmap] value as the name of the control that gives it by default
    void pick_mapping(const char* key, POINT screen);
    void set_mapping(const std::string& key, const std::string& value);
    double ini_num(const char* key, double def) const { return std::atof(ini_.get("pad", key, std::to_string(def)).c_str()); }

    XInputApi xi_;
    XState pad_{};                 // XInput layout, also for joystick-API pads (mapped)
    int pad_idx_ = -1, pad_scan_ = 0;
    bool pad_joy_ = false;         // pad_idx_ is a joystick-API device (winmm), not XInput
    PadKind pad_seen_ = PadKind::xbox;
    std::vector<std::pair<RECT, const PadShape*>> pad_hits_;   // client coordinates, filled by paint
    int hover_pad_ = -1;
    HWND wnd_;
    HWND tips_ = nullptr;
    Fonts fonts_;
    std::wstring title_, path_, why_;
    Ini ini_;
    bool exists_ = false;
    std::string stamp_;
    int tab_ = T_GRAPHICS;
    std::vector<Row> rows_;
    std::vector<RECT> tab_rects_;
    int hover_tab_ = -1;
    int scroll_ = 0, content_h_ = 0;
    int next_id_ = kIdBase;
    HBRUSH field_ = nullptr;
    std::wstring saved_note_;
    int dlss_ = 0;
    std::function<void()> dlss_dl_;
};

std::string Panel::file_stamp() const {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (path_.empty() || !GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &a)) return {};
    return std::to_string(a.ftLastWriteTime.dwLowDateTime) + ":" + std::to_string(a.ftLastWriteTime.dwHighDateTime) + ":" + std::to_string(a.nFileSizeLow);
}

void Panel::fresh() {
    if (path_.empty()) return;
    Ini now;
    if (now.load(path_)) { ini_ = now; exists_ = true; }
}

std::string Panel::value_of(const Opt& o) const {
    const std::string k = o.key;
    if (k == "#size") return ini_.get("display", "width", "1280") + "x" + ini_.get("display", "height", "720");
    if (k == "#mouse_speed") return ini_.get("mouse", "yaw", o.def);
    return ini_.get(o.section, o.key, o.def);
}

HWND Panel::make_combo(Row& r, const std::vector<std::pair<std::string, std::wstring>>& choices, const std::string& current) {
    HWND cb = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, S(400), wnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(next_id_++)), GetModuleHandleW(nullptr), nullptr);
    SetWindowTheme(cb, L"DarkMode_CFD", nullptr);
    SendMessageW(cb, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.font), FALSE);
    int sel = -1;
    for (const auto& [value, label] : choices) {
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        r.values.push_back(value);
        if (numeric_equal(value, current)) sel = static_cast<int>(r.values.size()) - 1;
    }
    if (sel < 0 && !current.empty()) {
        // a value from the file that the list does not have (set by hand or by the game's menu): keep it selectable
        const std::wstring t = L"Custom (" + widen(current) + L")";
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
        r.values.push_back(current);
        sel = static_cast<int>(r.values.size()) - 1;
    }
    SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    return cb;
}
HWND Panel::make_combo(Row& r, const std::vector<Choice>& choices, const std::string& current) {
    std::vector<std::pair<std::string, std::wstring>> c;
    for (const Choice& ch : choices) c.emplace_back(ch.value, ch.label);
    return make_combo(r, c, current);
}

// style_kind: 0 switch, 1 keycap, 2 small button, 3 remove (x)
HWND Panel::make_button(const wchar_t* t, int style_kind) {
    HWND b = CreateWindowExW(0, WC_BUTTONW, t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, wnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(next_id_++)), GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(b, GWLP_USERDATA, style_kind);
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.bold), FALSE);
    return b;
}

void Panel::tip(HWND ctl, const wchar_t* t) {
    if (!t || !*t || !ctl) return;
    if (!tips_) {
        tips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, wnd_, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
        SetWindowTheme(tips_, L"DarkMode_Explorer", nullptr);
        SendMessageW(tips_, TTM_SETMAXTIPWIDTH, 0, S(360));
        SendMessageW(tips_, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
    }
    TTTOOLINFOW ti{sizeof ti};
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = wnd_;
    ti.uId = reinterpret_cast<UINT_PTR>(ctl);
    ti.lpszText = const_cast<wchar_t*>(t);
    SendMessageW(tips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

PadKind Panel::pad_kind() const {
    const std::string v = lower(ini_.get("library", "pad_icons", "auto"));
    if (v == "xbox") return PadKind::xbox;
    if (v == "playstation") return PadKind::playstation;
    if (v == "generic") return PadKind::generic;
    return pad_seen_;
}

std::vector<std::pair<std::string, std::wstring>> Panel::targets(const std::string& key) const {
    const PadKind kind = pad_kind();
    std::vector<std::pair<std::string, std::wstring>> out;
    out.emplace_back("default", L"Default");
    for (const PadShape& s : kShapes) {
        if (s.key == key) continue;
        out.emplace_back(default_target(s.key, ini_.get("pad", "label_abxy", "1") != "0", kind), L"Acts as " + control_name(s.key, kind));
    }
    out.emplace_back("none", L"Nothing");
    return out;
}

std::wstring Panel::target_text(const std::string& value) const {
    const PadKind kind = pad_kind();
    if (lower(value) == "none") return L"off";
    for (const PadShape& s : kShapes)
        if (lower(default_target(s.key, ini_.get("pad", "label_abxy", "1") != "0", kind)) == lower(value)) return control_name(s.key, kind);
    return widen(value);
}

void Panel::rebuild() {
    for (Row& r : rows_) for (HWND h : {r.ctl, r.ctl2, r.ctl3}) if (h) DestroyWindow(h);
    rows_.clear();
    rows_.reserve(160);   // rows hold their controls; pointers into the vector are kept while it fills
    next_id_ = kIdBase;
    if (tips_) { DestroyWindow(tips_); tips_ = nullptr; }
    if (path_.empty()) { content_h_ = 0; place(); InvalidateRect(wnd_, nullptr, FALSE); return; }
    int y = S(76);
    auto add = [&](Row r, int h) { r.y = y; r.h = h; y += h; rows_.push_back(std::move(r)); return &rows_.back(); };
    const auto& o = options();
    auto add_options = [&](int tab) {
        for (size_t i = 0; i < o.size(); ++i) {
            if (o[i].tab != tab) continue;
            if (!o[i].section) {
                Row hd{RowKind::heading};
                hd.textv = o[i].label;
                add(hd, S(52));
                continue;
            }
            Row r{RowKind::option};
            r.opt = static_cast<int>(i);
            Row* p = add(r, S(48));
            const std::string v = value_of(o[i]);
            if (is_keycap(o[i])) {
                p->ctl = make_button(key_text(v.empty() ? "none" : v).c_str(), 1);
            } else if (o[i].choices.empty()) {
                p->on = v == "1" || lower(v) == "true" || lower(v) == "on";
                p->ctl = make_button(L"", 0);
            } else if (dlss_ != 2 && std::string(o[i].section) == "upscale" && std::string(o[i].key) == "mode") {
                std::vector<Choice> c;   // DLSS / DLAA need the DLLs and an RTX card
                for (const Choice& ch : o[i].choices) if (std::string(ch.value) != "dlss" && std::string(ch.value) != "dlaa") c.push_back(ch);
                p->ctl = make_combo(*p, c, v);
                if (dlss_ == 1) {
                    Row d{RowKind::dlss_dl};
                    d.textv = L"NVIDIA DLSS";
                    add(d, S(48))->ctl = make_button(L"Download DLSS", 2);
                }
            } else if (std::string(o[i].section) == "pad" && std::string(o[i].key) == "label_abxy" && pad_kind() == PadKind::playstation) {
                rows_.pop_back();   // PlayStation buttons have no letters: always by position
                y -= S(48);
                continue;
            } else p->ctl = make_combo(*p, o[i].choices, v);
            tip(p->ctl, o[i].hint);
        }
    };
    if (tab_ == T_KEYS) {
        add_options(T_KEYS);
        // the [bindings.*] sections the file has; without any, the game's factory bindings apply
        std::vector<std::string> secs;
        for (const std::string& s : ini_.sections()) if (s.rfind("bindings.", 0) == 0) secs.push_back(s);
        if (secs.empty()) {
            Row n{RowKind::note};
            n.textv = L"The game's own keys are in use. Start the game once to change them here.";
            add(n, S(64));
        }
        for (const std::string& sec : secs) {
            Row hd{RowKind::binding_head};
            hd.section = sec;
            const std::string ctx = sec.substr(9);
            hd.textv = ctx == "all" ? L"Keys everywhere" : ctx == "menu" ? L"Keys in menus" : L"Keys while playing (" + widen(ctx) + L")";
            add(hd, S(52));
            const auto e = ini_.entries(sec);
            for (size_t i = 0; i < e.size(); ++i) {
                Row r{RowKind::binding};
                r.section = sec;
                r.bind = static_cast<int>(i);
                Row* p = add(r, S(44));
                p->ctl = make_button(key_text(e[i].first).c_str(), 1);
                p->ctl2 = make_combo(*p, actions(), e[i].second);
                p->ctl3 = make_button(L"✕", 3);
                tip(p->ctl, L"Click, then press the new key");
                tip(p->ctl3, L"Remove this key");
            }
            Row ab{RowKind::add_binding};
            ab.section = sec;
            Row* p = add(ab, S(52));
            p->ctl = make_button(L"+  Add a key", 2);
        }
    } else if (tab_ == T_PAD) {
        Row t{RowKind::pad_test};
        Row* p = add(t, S(kPadH + 20));
        p->ctl = make_button(L"Test rumble", 2);
        p->ctl2 = make_button(L"Reset buttons", 2);
        add_options(T_PAD);
        // which pad the pictures show
        Row hd{RowKind::heading};
        hd.textv = L"Button mapping";
        add(hd, S(52));
        // the rows in the order a player looks for them: face buttons, shoulders and triggers, the two small buttons, sticks, d-pad
        static const char* const kOrder[] = {"south", "east", "west", "north", "left_shoulder", "right_shoulder", "left_trigger", "right_trigger",
                                             "back", "start", "left_stick", "right_stick", "dpad_up", "dpad_down", "dpad_left", "dpad_right"};
        for (const char* key : kOrder) {
            const PadShape& s = *std::find_if(std::begin(kShapes), std::end(kShapes), [&](const PadShape& p) { return std::string(p.key) == key; });
            Row r{RowKind::padmap};
            r.section = s.key;
            Row* q = add(r, S(46));
            q->ctl = make_combo(*q, targets(s.key), ini_.get("padmap", s.key, "default"));
        }
    } else {
        add_options(tab_);
    }
    content_h_ = y + S(24);
    if (tab_ == T_PAD) SetTimer(wnd_, kPadTimer, 33, nullptr);
    else KillTimer(wnd_, kPadTimer);
    place();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void Panel::rebuild_keep_scroll() {
    const int keep = scroll_;
    HWND focus = GetFocus();
    int focus_id = focus && IsChild(wnd_, focus) ? GetDlgCtrlID(focus) : 0;
    rebuild();
    RECT rc;
    GetClientRect(wnd_, &rc);
    scroll_ = std::clamp(keep, 0, std::max(0, content_h_ - static_cast<int>(rc.bottom)));
    place();
    if (focus_id) if (HWND f = GetDlgItem(wnd_, focus_id)) SetFocus(f);   // the same row's control: ids are given in row order
}

// ---- the controller picture -----------------------------------------------------------------------------------------------------
void Panel::poll_pad() {
    // XInput first (Xbox and compatible pads), then the joystick API for the others (PlayStation pads and the rest)
    if (pad_idx_ < 0) {
        if (pad_scan_++ % 30) return;   // an empty slot is slow to ask: look for a controller once a second
        pad_ = XState{};
        if (xi_.get)
            for (DWORD i = 0; i < 4; ++i)
                if (xi_.get(i, &pad_) == 0) { pad_idx_ = static_cast<int>(i); pad_joy_ = false; pad_seen_ = PadKind::xbox; return; }
        const UINT n = joyGetNumDevs();
        for (UINT i = 0; i < std::min(n, 16u); ++i) {
            JOYINFOEX ji{sizeof ji, JOY_RETURNALL};
            if (joyGetPosEx(i, &ji) != JOYERR_NOERROR) continue;
            JOYCAPSW caps{};
            if (joyGetDevCapsW(i, &caps, sizeof caps) != JOYERR_NOERROR) continue;
            if (caps.wMid == 0x045E) continue;   // an Xbox pad XInput did not answer for (it shows up there)
            pad_idx_ = static_cast<int>(i);
            pad_joy_ = true;
            pad_seen_ = caps.wMid == 0x054C ? PadKind::playstation : PadKind::generic;
            break;
        }
        if (pad_idx_ >= 0) rebuild_keep_scroll();   // the mapping rows speak the pad's language
        return;
    }
    if (!pad_joy_) {
        if (xi_.get(static_cast<DWORD>(pad_idx_), &pad_) != 0) { pad_idx_ = -1; pad_ = XState{}; }
        return;
    }
    JOYINFOEX ji{sizeof ji, JOY_RETURNALL};
    if (joyGetPosEx(static_cast<UINT>(pad_idx_), &ji) != JOYERR_NOERROR) { pad_idx_ = -1; pad_ = XState{}; return; }
    // the DirectInput layout of PlayStation pads (and most others): 1 Square 2 Cross 3 Circle 4 Triangle 5 L1 6 R1 7 L2 8 R2 9 Create 10 Options 11 L3 12 R3
    static const WORD kJoy[12] = {0x4000, 0x1000, 0x2000, 0x8000, 0x0100, 0x0200, 0, 0, 0x0020, 0x0010, 0x0040, 0x0080};
    XState x{};
    x.packet = pad_.packet + 1;
    for (int b = 0; b < 12; ++b) if (ji.dwButtons & (1u << b)) x.buttons |= kJoy[b];
    x.lt = (ji.dwButtons & (1u << 6)) ? 255 : 0;
    x.rt = (ji.dwButtons & (1u << 7)) ? 255 : 0;
    if (ji.dwPOV != JOY_POVCENTERED) {
        const DWORD a = ji.dwPOV;
        if (a >= 31500 || a <= 4500) x.buttons |= 1;
        if (a >= 13500 && a <= 22500) x.buttons |= 2;
        if (a >= 22500 && a <= 31500) x.buttons |= 4;
        if (a >= 4500 && a <= 13500) x.buttons |= 8;
    }
    auto ax = [](DWORD v) { return static_cast<SHORT>(std::clamp(static_cast<long>(v) - 32768L, -32767L, 32767L)); };
    x.lx = ax(ji.dwXpos);
    x.ly = static_cast<SHORT>(-ax(ji.dwYpos));
    x.rx = ax(ji.dwZpos);
    x.ry = static_cast<SHORT>(-ax(ji.dwRpos));
    if (x.buttons != pad_.buttons || x.lx != pad_.lx || x.ly != pad_.ly || x.rx != pad_.rx || x.ry != pad_.ry) pad_ = x;
    else x.packet = pad_.packet;
}

void Panel::paint_pad(gp::Graphics& g, float oy, const gp::Font& font, const gp::Font& bold, const gp::Font& little) {
    pad_hits_.clear();
    const float ox = Sf(32), k = Sf(1);
    const PadKind kind = pad_kind();
    const double thr = ini_num("trigger_threshold", 0.35);
    auto pressed = [&](const PadShape& s) {
        if (pad_idx_ < 0) return false;
        if (s.trig) return (s.trig == 1 ? pad_.lt : pad_.rt) / 255.0 > thr;
        return (pad_.buttons & s.mask) != 0;
    };
    // the pad's body
    gp::SolidBrush body(C(RGB(0x1A, 0x1D, 0x24)));
    gp::GraphicsPath bp;
    round_path(bp, gp::RectF(ox + Sf(20), oy + Sf(52), Sf(380), Sf(196)), Sf(70));
    g.FillPath(&body, &bp);
    // every control shows its own button; a mapped one says what it does now
    const bool by_letter = ini_.get("pad", "label_abxy", "1") != "0";
    for (size_t i = 0; i < std::size(kShapes); ++i) {
        const PadShape& s = kShapes[i];
        const gp::RectF r(ox + Sf(s.x), oy + Sf(s.y), Sf(s.w), Sf(s.h));
        draw_control(g, s, kind, r, pressed(s), hover_pad_ == static_cast<int>(i), font, bold, k);
        const std::string v = ini_.get("padmap", s.key, "default");
        if (lower(v) != "default" && lower(v) != lower(default_target(s.key, by_letter, kind))) {
            const std::wstring badge = L"→ " + target_text(v);
            gp::SolidBrush bb(C(kWarn));
            // beside the control: above its corner, or below it for the face buttons (they sit close together)
            const gp::RectF br(r.X + r.Width - Sf(10), is_face(s.key) ? r.Y + r.Height - Sf(4) : r.Y - Sf(8), Sf(64), Sf(16));
            fill_round(g, gp::RectF(br.X, br.Y, Sf(10) + Sf(6.5f) * static_cast<float>(badge.size()), br.Height), Sf(8), bb);
            text(g, badge, little, gp::RectF(br.X + Sf(5), br.Y, Sf(120), br.Height), kBg, gp::StringAlignmentNear, gp::StringAlignmentCenter);
        }
        pad_hits_.push_back({RECT{static_cast<LONG>(r.X), static_cast<LONG>(r.Y), static_cast<LONG>(r.X + r.Width), static_cast<LONG>(r.Y + r.Height)}, &s});
    }
    // the sticks: the dead zone is the shaded circle, the dot is the stick
    const double dz[2] = {ini_num("deadzone", 0.1), ini_num("right_deadzone", 0.1)};
    const float val[2][2] = {{pad_.lx / 32768.f, pad_.ly / 32768.f}, {pad_.rx / 32768.f, pad_.ry / 32768.f}};
    for (int i = 0; i < 2; ++i) {
        const float bs = Sf(104), bx = ox + Sf(440) + static_cast<float>(i) * Sf(120), by = oy + Sf(4);
        gp::SolidBrush bg(C(kCard));
        fill_round(g, gp::RectF(bx, by, bs, bs), Sf(10), bg);
        const float zr = static_cast<float>(dz[i]) * bs / 2;
        gp::SolidBrush zb(C(kWarn, 90));
        g.FillEllipse(&zb, bx + bs / 2 - zr, by + bs / 2 - zr, 2 * zr, 2 * zr);
        gp::Pen ring(C(kLine), Sf(1));
        g.DrawEllipse(&ring, gp::RectF(bx + Sf(4), by + Sf(4), bs - Sf(8), bs - Sf(8)));
        const float mag = std::sqrt(val[i][0] * val[i][0] + val[i][1] * val[i][1]);
        gp::SolidBrush dot(C(mag > dz[i] ? kAccentHi : kMuted));
        const float dx = bx + bs / 2 + val[i][0] * (bs / 2 - Sf(6)), dy = by + bs / 2 - val[i][1] * (bs / 2 - Sf(6));
        g.FillEllipse(&dot, dx - Sf(5), dy - Sf(5), Sf(10), Sf(10));
        text(g, i ? L"Right stick" : L"Left stick", little, gp::RectF(bx, by + bs + Sf(4), bs, Sf(18)), kMuted, gp::StringAlignmentCenter);
    }
    // what is connected
    std::wstring con;
    COLORREF cc = kMuted;
    if (pad_idx_ >= 0) {
        con = pad_seen_ == PadKind::xbox ? L"Xbox controller connected" : pad_seen_ == PadKind::playstation ? L"PlayStation controller connected" : L"Controller connected";
        cc = kOk;
    } else con = L"No controller found. Connect one to test it here.";
    gp::SolidBrush cdot(C(cc));
    g.FillEllipse(&cdot, ox + Sf(440), oy + Sf(140), Sf(8), Sf(8));
    text(g, con, font, gp::RectF(ox + Sf(456), oy + Sf(132), Sf(320), Sf(24)), cc == kOk ? kText : kMuted, gp::StringAlignmentNear, gp::StringAlignmentCenter);
    text(g, L"Click a button to change what it does", little, gp::RectF(ox + Sf(20), oy + Sf(252), Sf(380), Sf(18)), kMuted, gp::StringAlignmentCenter);
}

void Panel::set_mapping(const std::string& key, const std::string& value) {
    fresh();
    const PadKind kind = pad_kind();
    const bool by_letter = ini_.get("pad", "label_abxy", "1") != "0";
    // a control set to its own button is "default" (the file stays clean, and a layout change keeps it right)
    ini_.set("padmap", key, lower(value) == lower(default_target(key, by_letter, kind)) ? "default" : value);
    save();
    rebuild_keep_scroll();
}

// the mapping of one control, chosen from a menu (the rows below the picture show the same value)
void Panel::pick_mapping(const char* key, POINT screen) {
    HMENU m = CreatePopupMenu();
    const PadKind kind = pad_kind();
    const std::string cur = ini_.get("padmap", key, "default");
    const auto t = targets(key);
    const std::wstring head = control_name(key, kind);
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, head.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < t.size(); ++i) {
        const bool on = lower(cur) == lower(t[i].first) || (i == 0 && lower(cur) == lower(default_target(key, ini_.get("pad", "label_abxy", "1") != "0", kind)));
        AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), 1 + i, t[i].second.c_str());
    }
    const int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, 0, wnd_, nullptr);
    DestroyMenu(m);
    if (id <= 0) return;
    set_mapping(key, t[static_cast<size_t>(id - 1)].first);
}

void Panel::place() {
    RECT rc;
    GetClientRect(wnd_, &rc);
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_h_), static_cast<UINT>(rc.bottom), scroll_};
    SetScrollInfo(wnd_, SB_VERT, &si, TRUE);
    GetClientRect(wnd_, &rc);              // the scroll bar may have come or gone
    const int right = rc.right - S(40);   // the end of the heading lines
    HDWP dw = BeginDeferWindowPos(static_cast<int>(rows_.size() * 3));
    for (Row& r : rows_) {
        const int y = r.y - scroll_;
        // controls above the tab bar would cover it: hide what scrolled under it
        const bool vis = y + r.h > S(64) && y < rc.bottom;
        auto put = [&](HWND h, int x, int yy, int w, int hh) {
            const bool show = vis && yy >= S(64);   // never over the tab bar
            if (h) dw = DeferWindowPos(dw, h, nullptr, x, yy, w, hh, SWP_NOZORDER | SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        };
        if (r.kind == RowKind::option) {
            const Opt& o = options()[static_cast<size_t>(r.opt)];
            if (is_keycap(o)) put(r.ctl, right - S(180), y + (r.h - S(34)) / 2, S(180), S(34));
            else if (o.choices.empty()) put(r.ctl, right - S(52), y + (r.h - S(28)) / 2, S(52), S(28));
            else put(r.ctl, right - S(280), y + (r.h - S(30)) / 2, S(280), S(400));
        } else if (r.kind == RowKind::padmap) {
            put(r.ctl, right - S(280), y + (r.h - S(30)) / 2, S(280), S(400));
        } else if (r.kind == RowKind::binding) {
            put(r.ctl, S(32), y + S(6), S(180), S(32));
            put(r.ctl2, S(232), y + S(7), S(280), S(400));
            put(r.ctl3, S(524), y + S(6), S(32), S(32));
        } else if (r.kind == RowKind::add_binding) {
            put(r.ctl, S(32), y + S(8), S(160), S(34));
        } else if (r.kind == RowKind::dlss_dl) {
            put(r.ctl, right - S(280), y + (r.h - S(34)) / 2, S(280), S(34));
        } else if (r.kind == RowKind::pad_test) {
            put(r.ctl, S(32 + 440), y + S(8 + 172), S(110), S(34));
            put(r.ctl2, S(32 + 560), y + S(8 + 172), S(120), S(34));
        }
    }
    EndDeferWindowPos(dw);
}

void Panel::save() {
    if (path_.empty()) return;
    bool ok = true;
    if (!exists_) {
        // a new file: say what it is; the game fills in the rest (and its factory bindings) when it starts
        const std::string head = "# Settings of " + narrow(title_) + ": the library's Settings page and the game's own menu both edit this file. runtime/docs/CONFIG.md\n";
        ok = write_file_atomic(path_, head + ini_.text());
        if (ok) { exists_ = true; ini_.load(path_); }
    } else ok = ini_.save(path_);
    stamp_ = file_stamp();
    saved_note_ = ok ? L"Saved" : L"Could not save: " + path_;
    InvalidateRect(wnd_, nullptr, FALSE);   // the whole panel: the controller picture shows dead zones and mappings
}

void Panel::set_bindings(const std::string& section, const std::vector<std::pair<std::string, std::string>>& e) {
    ini_.replace_entries(section, e);
    save();
    rebuild_keep_scroll();
}

void Panel::command(int id, int code, HWND ctl) {
    (void)id;
    for (Row& r : rows_) {
        if (r.kind == RowKind::dlss_dl && ctl == r.ctl) {
            if (code == BN_CLICKED && dlss_dl_) dlss_dl_();
            return;
        }
        if (r.kind == RowKind::pad_test && (ctl == r.ctl || ctl == r.ctl2)) {
            if (code != BN_CLICKED) return;
            if (ctl == r.ctl2) {   // every control back to its own button
                fresh();
                for (const PadShape& s : kShapes) if (ini_.has("padmap", s.key)) ini_.set("padmap", s.key, "default");
                save();
                rebuild_keep_scroll();
                return;
            }
            if (!xi_.set || pad_idx_ < 0 || pad_joy_) return;
            XVib v{0xFFFF, 0xFFFF};
            xi_.set(static_cast<DWORD>(pad_idx_), &v);
            SetTimer(wnd_, kRumbleTimer, 700, nullptr);
            return;
        }
        if (r.kind == RowKind::padmap && ctl == r.ctl) {
            if (code != CBN_SELCHANGE) return;
            const int s = static_cast<int>(SendMessageW(ctl, CB_GETCURSEL, 0, 0));
            if (s < 0 || s >= static_cast<int>(r.values.size())) return;
            set_mapping(r.section, r.values[static_cast<size_t>(s)]);
            return;
        }
        if (r.kind == RowKind::option && ctl == r.ctl) {
            const Opt& o = options()[static_cast<size_t>(r.opt)];
            std::string v;
            if (is_keycap(o)) {
                if (code != BN_CLICKED) return;
                v = capture_input(wnd_, fonts_, L"to release the mouse");
                if (v.empty()) return;
                SetWindowTextW(ctl, key_text(v).c_str());
            } else if (o.choices.empty()) {
                if (code != BN_CLICKED) return;
                r.on = !r.on;
                v = r.on ? "1" : "0";
                InvalidateRect(ctl, nullptr, FALSE);
            } else {
                if (code != CBN_SELCHANGE) return;
                const int s = static_cast<int>(SendMessageW(ctl, CB_GETCURSEL, 0, 0));
                if (s < 0 || s >= static_cast<int>(r.values.size())) return;
                v = r.values[static_cast<size_t>(s)];
            }
            fresh();   // only this key changes: what the game wrote meanwhile stays
            const std::string k = o.key;
            if (k == "#size") {
                const size_t x = v.find('x');
                ini_.set("display", "width", v.substr(0, x));
                ini_.set("display", "height", v.substr(x + 1));
            } else if (k == "#mouse_speed") {
                ini_.set("mouse", "yaw", v);
                ini_.set("mouse", "pitch", v);
            } else ini_.set(o.section, o.key, v);
            save();
            // the face-button layout renames the mapping choices
            if (std::string(o.section) == "pad" && k == "label_abxy") rebuild_keep_scroll();
            return;
        }
        if (r.kind == RowKind::binding && (ctl == r.ctl || ctl == r.ctl2 || ctl == r.ctl3)) {
            fresh();
            auto e = ini_.entries(r.section);
            if (r.bind < 0 || r.bind >= static_cast<int>(e.size())) { rebuild_keep_scroll(); return; }
            auto& entry = e[static_cast<size_t>(r.bind)];
            if (ctl == r.ctl2) {
                if (code != CBN_SELCHANGE) return;
                const int s = static_cast<int>(SendMessageW(ctl, CB_GETCURSEL, 0, 0));
                if (s < 0 || s >= static_cast<int>(r.values.size())) return;
                entry.second = r.values[static_cast<size_t>(s)];
                ini_.replace_entries(r.section, e);
                save();
                return;
            }
            if (code != BN_CLICKED) return;
            if (ctl == r.ctl3) {
                e.erase(e.begin() + r.bind);
                set_bindings(r.section, e);
                return;
            }
            std::wstring what;
            for (const Choice& c : actions()) if (c.value == entry.second) what = c.label;
            if (what.empty()) what = widen(entry.second);
            const std::string key = capture_input(wnd_, fonts_, L"for: " + what);
            if (key.empty()) return;
            // a key bound twice in one section would do two things: the old binding of that key goes
            const std::string act = entry.second;
            std::vector<std::pair<std::string, std::string>> out;
            for (size_t i = 0; i < e.size(); ++i) {
                if (static_cast<int>(i) == r.bind) out.emplace_back(key, act);
                else if (lower(e[i].first) != lower(key)) out.push_back(e[i]);
            }
            set_bindings(r.section, out);
            return;
        }
        if (r.kind == RowKind::add_binding && ctl == r.ctl && code == BN_CLICKED) {
            const std::string key = capture_input(wnd_, fonts_, L"New key (then choose what it does)");
            if (key.empty()) return;
            fresh();
            auto e = ini_.entries(r.section);
            for (const auto& [k, v] : e) if (lower(k) == lower(key)) { rebuild_keep_scroll(); return; }   // already bound: its row shows it
            e.emplace_back(key, "btn_a");
            set_bindings(r.section, e);
            return;
        }
    }
}

void Panel::draw_item(const DRAWITEMSTRUCT* d) {
    const int kind = static_cast<int>(GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA));
    bool on = false;
    for (const Row& r : rows_) if (r.ctl == d->hwndItem) on = r.on;
    POINT pt;
    GetCursorPos(&pt);
    RECT wr;
    GetWindowRect(d->hwndItem, &wr);
    const bool hot = PtInRect(&wr, pt) != 0, pressed = (d->itemState & ODS_SELECTED) != 0, focus = (d->itemState & ODS_FOCUS) != 0;
    const RECT rc = d->rcItem;
    paint_buffered(d->hDC, rc, [&](gp::Graphics& g) {
        gp::SolidBrush bg(C(kBg));
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
        const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);
        auto ring = [&](float rad) {   // keyboard focus
            if (!focus) return;
            gp::Pen p(C(kAccentHi), Sf(1.5f));
            gp::GraphicsPath path;
            round_path(path, gp::RectF(Sf(1), Sf(1), W - Sf(2), H - Sf(2)), rad);
            g.DrawPath(&p, &path);
        };
        if (kind == 0) {
            // a switch
            gp::SolidBrush track(C(on ? (hot ? kAccentHi : kAccent) : (hot ? kButtonHi : RGB(0x3A, 0x40, 0x4D))));
            fill_round(g, gp::RectF(Sf(2), Sf(3), W - Sf(4), H - Sf(6)), (H - Sf(6)) / 2, track);
            const float k = H - Sf(12);
            gp::SolidBrush knob(C(RGB(255, 255, 255)));
            g.FillEllipse(&knob, on ? W - Sf(8) - k : Sf(8), Sf(6), k, k);
            ring((H - Sf(2)) / 2);
            return;
        }
        wchar_t t[128] = {};
        GetWindowTextW(d->hwndItem, t, 128);
        HDC fdc = g.GetHDC();
        gp::Font f(fdc, fonts_.bold);
        g.ReleaseHDC(fdc);
        if (kind == 1) {
            // a key cap
            gp::SolidBrush shade(C(RGB(0x0C, 0x0E, 0x12)));
            fill_round(g, gp::RectF(0, Sf(2), W, H - Sf(2)), Sf(7), shade);
            gp::SolidBrush cap(C(pressed ? kButton : hot ? kButtonHi : kButton));
            fill_round(g, gp::RectF(0, pressed ? Sf(2) : 0, W, H - Sf(4)), Sf(7), cap);
            text(g, t, f, gp::RectF(0, pressed ? Sf(2) : 0, W, H - Sf(4)), kText, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        } else {
            COLORREF fill = kind == 3 ? (hot ? RGB(0x5A, 0x2A, 0x2E) : kCard) : (hot ? kButtonHi : kButton);
            if (pressed) fill = RGB(0x24, 0x28, 0x31);
            gp::SolidBrush b(C(fill));
            fill_round(g, gp::RectF(0, 0, W, H), Sf(8), b);
            text(g, t, f, gp::RectF(0, 0, W, H), kind == 3 ? RGB(0xFF, 0xA0, 0xA0) : kText, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        }
        ring(Sf(7));
    });
}

void Panel::paint(HDC hdc) {
    RECT rc;
    GetClientRect(wnd_, &rc);
    paint_buffered(hdc, rc, [&](gp::Graphics& g) {
        gp::SolidBrush bg(C(kBg));
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
        HDC fdc = g.GetHDC();
        gp::Font font(fdc, fonts_.font), bold(fdc, fonts_.bold), little(fdc, fonts_.little), title(fdc, fonts_.title);
        g.ReleaseHDC(fdc);
        const float W = static_cast<float>(rc.right);
        if (path_.empty()) {
            // empty state: why there is nothing to set yet
            gp::SolidBrush card(C(kCard));
            fill_round(g, gp::RectF(Sf(32), Sf(24), std::min(W - Sf(64), Sf(640)), Sf(96)), Sf(12), card);
            text(g, L"No settings yet", bold, gp::RectF(Sf(56), Sf(42), Sf(560), Sf(24)), kText);
            text(g, why_.empty() ? L"Choose a game in the library." : why_, font, gp::RectF(Sf(56), Sf(72), std::min(W - Sf(112), Sf(580)), Sf(40)), kMuted,
                 gp::StringAlignmentNear, gp::StringAlignmentNear, true);
            return;
        }
        // rows
        const float sy = static_cast<float>(scroll_);
        const PadKind kind = pad_kind();
        for (const Row& r : rows_) {
            const float y = static_cast<float>(r.y) - sy;
            if (y + static_cast<float>(r.h) < Sf(64) || y > static_cast<float>(rc.bottom)) continue;
            const float lw = W - Sf(32) - Sf(340);
            if (r.kind == RowKind::heading || r.kind == RowKind::binding_head) {
                text(g, r.textv, bold, gp::RectF(Sf(32), y + Sf(20), W - Sf(64), Sf(22)), kAccentHi);
                gp::SolidBrush line(C(kLine));
                g.FillRectangle(&line, Sf(32), y + Sf(46), W - Sf(64) - Sf(8), Sf(1));
            } else if (r.kind == RowKind::note) {
                gp::SolidBrush card(C(kCard));
                fill_round(g, gp::RectF(Sf(32), y + Sf(8), W - Sf(72), static_cast<float>(r.h) - Sf(12)), Sf(8), card);
                text(g, r.textv, font, gp::RectF(Sf(46), y + Sf(14), W - Sf(100), static_cast<float>(r.h) - Sf(20)), kMuted, gp::StringAlignmentNear,
                     gp::StringAlignmentNear, true);
            } else if (r.kind == RowKind::option) {
                const Opt& o = options()[static_cast<size_t>(r.opt)];
                text(g, o.label, font, gp::RectF(Sf(32), y, lw, static_cast<float>(r.h)), kText, gp::StringAlignmentNear, gp::StringAlignmentCenter);
                if (o.when == kNext) {
                    // "applies at the next start": a small tag after the label
                    HDC dc2 = g.GetHDC();
                    SIZE ts{};
                    HGDIOBJ of = SelectObject(dc2, fonts_.font);
                    GetTextExtentPoint32W(dc2, o.label, static_cast<int>(wcslen(o.label)), &ts);
                    SelectObject(dc2, of);
                    g.ReleaseHDC(dc2);
                    const gp::RectF tag(Sf(32) + static_cast<float>(ts.cx) + Sf(10), y + static_cast<float>(r.h) / 2 - Sf(10), Sf(76), Sf(20));
                    gp::SolidBrush tb(C(kLine));
                    fill_round(g, tag, Sf(10), tb);
                    text(g, L"Next start", little, tag, kMuted, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
                }
            } else if (r.kind == RowKind::padmap) {
                // the control's look and name
                for (const PadShape& s : kShapes) {
                    if (r.section != s.key) continue;
                    const float d = Sf(28);
                    PadShape one = s;
                    one.round = is_face(s.key) || one.round;
                    draw_control(g, one, kind, gp::RectF(Sf(32), y + (static_cast<float>(r.h) - d) / 2, is_face(s.key) || s.round ? d : Sf(44), d), false, false,
                                 little, little, Sf(1) * 0.85f);
                    text(g, control_name(s.key, kind), font, gp::RectF(Sf(88), y, lw - Sf(56), static_cast<float>(r.h)), kText, gp::StringAlignmentNear,
                         gp::StringAlignmentCenter);
                }
            } else if (r.kind == RowKind::dlss_dl) {
                text(g, r.textv, font, gp::RectF(Sf(32), y, lw, static_cast<float>(r.h)), kText, gp::StringAlignmentNear, gp::StringAlignmentCenter);
            } else if (r.kind == RowKind::pad_test) {
                paint_pad(g, y + Sf(8), font, bold, little);
            } else if (r.kind == RowKind::binding) {
                text(g, L"→", font, gp::RectF(Sf(212), y, Sf(20), static_cast<float>(r.h)), kMuted, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
            }
        }
        // the header over the rows: the tabs and where it is saved
        gp::SolidBrush head(C(kBg));
        g.FillRectangle(&head, 0.f, 0.f, W, Sf(64));
        tab_rects_.clear();
        float x = Sf(32);
        for (int t = 0; t < T_COUNT; ++t) {
            HDC dc2 = g.GetHDC();
            SIZE ts{};
            HGDIOBJ of = SelectObject(dc2, fonts_.bold);
            GetTextExtentPoint32W(dc2, kTabNames[t], static_cast<int>(wcslen(kTabNames[t])), &ts);
            SelectObject(dc2, of);
            g.ReleaseHDC(dc2);
            const float tw = static_cast<float>(ts.cx) + Sf(32);
            const bool on = t == tab_;
            gp::SolidBrush pill(C(on ? kAccent : hover_tab_ == t ? kCardHi : kCard));
            fill_round(g, gp::RectF(x, Sf(12), tw, Sf(36)), Sf(18), pill);
            text(g, kTabNames[t], bold, gp::RectF(x, Sf(12), tw, Sf(36)), on ? RGB(255, 255, 255) : kText, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
            if (GetFocus() == wnd_ && on) {
                gp::Pen fp(C(kAccentHi), Sf(1.5f));
                gp::GraphicsPath fpath;
                round_path(fpath, gp::RectF(x - Sf(3), Sf(9), tw + Sf(6), Sf(42)), Sf(21));
                g.DrawPath(&fp, &fpath);
            }
            tab_rects_.push_back({static_cast<LONG>(x), static_cast<LONG>(Sf(12)), static_cast<LONG>(x + tw), static_cast<LONG>(Sf(48))});
            x += tw + Sf(8);
        }
        const bool bad = saved_note_.rfind(L"Could", 0) == 0;
        text(g, saved_note_.empty() ? L"" : (bad ? L"" : L"✓ ") + saved_note_, little, gp::RectF(x + Sf(12), Sf(12), W - x - Sf(44), Sf(36)),
             bad ? kBad : kOk, gp::StringAlignmentFar, gp::StringAlignmentCenter);
        gp::SolidBrush line(C(kLine));
        g.FillRectangle(&line, Sf(32), Sf(62), W - Sf(72), Sf(1));
        (void)title;
    });
}

LRESULT Panel::on(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd_, &ps);
            paint(dc);
            EndPaint(wnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (w == kRumbleTimer) {
                KillTimer(wnd_, kRumbleTimer);
                XVib off{0, 0};
                if (xi_.set && pad_idx_ >= 0 && !pad_joy_) xi_.set(static_cast<DWORD>(pad_idx_), &off);
            } else if (w == kPadTimer && IsWindowVisible(wnd_)) {
                const XState before = pad_;
                const int idx = pad_idx_;
                poll_pad();
                if (idx != pad_idx_ || before.packet != pad_.packet || before.buttons != pad_.buttons) InvalidateRect(wnd_, nullptr, FALSE);
            } else if (w == kWatchTimer && IsWindowVisible(wnd_)) {
                // the game (or an editor) changed the file: show what is in it now, unless a list is open or a key is being pressed
                const std::string st = file_stamp();
                if (st != stamp_ && GetCapture() == nullptr) {
                    bool dropped = false;
                    for (const Row& r : rows_) if (r.ctl && SendMessageW(r.ctl, CB_GETDROPPEDSTATE, 0, 0)) dropped = true;
                    if (!dropped) {
                        stamp_ = st;
                        fresh();
                        saved_note_ = L"Updated from the game";
                        rebuild_keep_scroll();
                    }
                }
            }
            return 0;
        case WM_SIZE: place(); InvalidateRect(wnd_, nullptr, FALSE); return 0;
        case WM_COMMAND: command(LOWORD(w), HIWORD(w), reinterpret_cast<HWND>(l)); return 0;
        case WM_DRAWITEM: draw_item(reinterpret_cast<const DRAWITEMSTRUCT*>(l)); return TRUE;
        case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, kText);
            SetBkColor(dc, kField);
            return reinterpret_cast<LRESULT>(field_);
        }
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_SETFOCUS: case WM_KILLFOCUS: { RECT top{0, 0, 100000, S(64)}; InvalidateRect(wnd_, &top, FALSE); return 0; }
        case WM_KEYDOWN:
            // the tabs by keyboard: Left / Right while the panel itself has the focus (Tab reaches it), Ctrl+Tab from anywhere in it
            if (w == VK_LEFT || w == VK_RIGHT) { set_tab((tab_ + (w == VK_RIGHT ? 1 : T_COUNT - 1)) % T_COUNT); return 0; }
            if (w == VK_NEXT || w == VK_PRIOR || w == VK_HOME || w == VK_END) {
                SendMessageW(wnd_, WM_VSCROLL, w == VK_NEXT ? SB_PAGEDOWN : w == VK_PRIOR ? SB_PAGEUP : w == VK_HOME ? SB_TOP : SB_BOTTOM, 0);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: SetFocus(wnd_); return 0;
        case WM_MOUSEMOVE: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int hv = -1, hp = -1;
            for (size_t i = 0; i < tab_rects_.size(); ++i) if (PtInRect(&tab_rects_[i], p)) hv = static_cast<int>(i);
            if (tab_ == T_PAD && p.y > S(64))
                for (const auto& h : pad_hits_) if (PtInRect(&h.first, p)) hp = static_cast<int>(h.second - kShapes);
            if (hv != hover_tab_) { hover_tab_ = hv; RECT top{0, 0, 100000, S(64)}; InvalidateRect(wnd_, &top, FALSE); }
            if (hp != hover_pad_) { hover_pad_ = hp; InvalidateRect(wnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, wnd_, 0};
            TrackMouseEvent(&t);
            SetCursor(LoadCursor(nullptr, hv >= 0 || hp >= 0 ? IDC_HAND : IDC_ARROW));
            for (const Row& r : rows_) for (HWND h : {r.ctl, r.ctl2, r.ctl3}) if (h && GetWindowLongPtrW(h, GWLP_USERDATA) != 0) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_tab_ = hover_pad_ = -1;
            InvalidateRect(wnd_, nullptr, FALSE);
            for (const Row& r : rows_) for (HWND h : {r.ctl, r.ctl2, r.ctl3}) if (h) InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
        case WM_LBUTTONUP: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (tab_ == T_PAD && p.y > S(64))
                for (const auto& h : pad_hits_)
                    if (PtInRect(&h.first, p)) { POINT s = p; ClientToScreen(wnd_, &s); pick_mapping(h.second->key, s); return 0; }
            for (size_t i = 0; i < tab_rects_.size(); ++i)
                if (PtInRect(&tab_rects_[i], p) && static_cast<int>(i) != tab_) set_tab(static_cast<int>(i));
            return 0;
        }
        case WM_MOUSEWHEEL: {
            RECT rc;
            GetClientRect(wnd_, &rc);
            scroll_ = std::clamp(scroll_ - GET_WHEEL_DELTA_WPARAM(w) * S(60) / WHEEL_DELTA, 0, std::max(0, content_h_ - static_cast<int>(rc.bottom)));
            place();
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            RECT rc;
            GetClientRect(wnd_, &rc);
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(wnd_, SB_VERT, &si);
            int pos = scroll_;
            switch (LOWORD(w)) {
                case SB_LINEUP: pos -= S(40); break;
                case SB_LINEDOWN: pos += S(40); break;
                case SB_PAGEUP: pos -= rc.bottom; break;
                case SB_PAGEDOWN: pos += rc.bottom; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = content_h_; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            }
            scroll_ = std::clamp(pos, 0, std::max(0, content_h_ - static_cast<int>(rc.bottom)));
            place();
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        }
        case WM_NOTIFY: break;
        case WM_PARENTNOTIFY: break;
        case WM_CHILDACTIVATE: break;
    }
    return DefWindowProcW(wnd_, m, w, l);
}

LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* p = reinterpret_cast<Panel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        p = new Panel(h);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
    }
    if (m == WM_NCDESTROY) {
        delete p;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return DefWindowProcW(h, m, w, l);
    }
    return p ? p->on(m, w, l) : DefWindowProcW(h, m, w, l);
}

Panel* panel_of(HWND h) { return reinterpret_cast<Panel*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

}  // namespace

bool register_class(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"NxrtSettingsPanel";
    return RegisterClassExW(&wc) != 0;
}

HWND create(HWND parent, HINSTANCE inst, int id) {
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, L"NxrtSettingsPanel", L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    return h;
}

void set_fonts(HWND panel, const Fonts& f) { if (Panel* p = panel_of(panel)) p->set_fonts(f); }

void set_game(HWND panel, const std::wstring& title, const std::wstring& controls_ini, const std::wstring& why_not) {
    if (Panel* p = panel_of(panel)) p->set_game(title, controls_ini, why_not);
}

void set_dlss(HWND panel, int state, std::function<void()> download) {
    if (Panel* p = panel_of(panel)) p->set_dlss(state, std::move(download));
}

void set_tab(HWND panel, int tab) { if (Panel* p = panel_of(panel)) p->set_tab(tab); }

}  // namespace setup::settings
