// SPDX-License-Identifier: GPL-3.0-or-later
// The library's Settings page (setup_settings.h): tabs, rows built from a table of the runtime's controls.ini keys, dark controls, the
// keyboard bindings editor with press-to-bind, and the ini edited in place.
#include "setup_settings.h"

#include <commctrl.h>
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
struct Opt {
    int tab;
    const char* section;    // nullptr: a heading (label is its text)
    const char* key;        // "#size": the window size (width and height)
    const wchar_t* label;
    const wchar_t* hint;
    const char* def;        // the runtime's default when the file has no value
    std::vector<Choice> choices;   // empty: an on/off switch ("1" / "0")
};

const std::vector<Choice> kPadButtons = {{"default", L"As on the controller"}, {"none", L"Nothing"}, {"A", L"A"}, {"B", L"B"}, {"X", L"X"},
                                         {"Y", L"Y"}, {"L", L"L"}, {"R", L"R"}, {"ZL", L"ZL"}, {"ZR", L"ZR"}, {"PLUS", L"+ (Plus)"},
                                         {"MINUS", L"- (Minus)"}, {"LSTICK", L"Left stick press"}, {"RSTICK", L"Right stick press"},
                                         {"UP", L"D-pad up"}, {"DOWN", L"D-pad down"}, {"LEFT", L"D-pad left"}, {"RIGHT", L"D-pad right"}};
const std::vector<Choice> kDeadzones = {{"0.05", L"5%"}, {"0.1", L"10%"}, {"0.15", L"15%"}, {"0.2", L"20%"}, {"0.25", L"25%"}};

const std::vector<Opt>& options() {
    static const std::vector<Opt> o = {
        // graphics
        {T_GRAPHICS, nullptr, nullptr, L"Resolution and upscaling", nullptr, nullptr, {}},
        {T_GRAPHICS, "display", "render_scale", L"Render scale", L"The game's picture size against the window. Auto: the window's size at start.", "auto",
         {{"auto", L"Auto (window size)"}, {"50", L"50%"}, {"67", L"67%"}, {"75", L"75%"}, {"100", L"100%"}, {"125", L"125%"}, {"150", L"150%"}, {"200", L"200% (supersampling)"}}},
        {T_GRAPHICS, "display", "dynamic_resolution", L"Dynamic resolution", L"The game lowers its own render size when the GPU falls behind.", "0", {}},
        {T_GRAPHICS, "upscale", "mode", L"Upscaler", L"Temporal modes need their DLLs and GPU support; without them the picture stays as Off.", "off",
         {{"off", L"Off"}, {"fsr1", L"AMD FSR 1"}, {"nis", L"NVIDIA Image Scaling"}, {"fsr3", L"AMD FSR 3 (temporal)"}, {"dlss", L"NVIDIA DLSS"}, {"dlaa", L"NVIDIA DLAA"}}},
        {T_GRAPHICS, "upscale", "quality", L"Upscaler quality", L"Render size of FSR 3 and DLSS.", "quality",
         {{"native", L"Native (100%)"}, {"quality", L"Quality (67%)"}, {"balanced", L"Balanced (59%)"}, {"performance", L"Performance (50%)"}, {"ultra_performance", L"Ultra performance (33%)"}}},
        {T_GRAPHICS, "upscale", "sharpness", L"Sharpening", nullptr, "0.8", {{"0", L"Off"}, {"0.25", L"Low"}, {"0.5", L"Medium"}, {"0.8", L"High"}, {"1", L"Maximum"}}},
        {T_GRAPHICS, nullptr, nullptr, L"Quality", nullptr, nullptr, {}},
        {T_GRAPHICS, "display", "anisotropy", L"Texture filtering", L"At least this anisotropic filtering on every filtering sampler.", "0",
         {{"0", L"As the game asks"}, {"2", L"2x"}, {"4", L"4x"}, {"8", L"8x"}, {"16", L"16x"}}},
        {T_GRAPHICS, "display", "lod_bias", L"Texture sharpness", L"Negative LOD bias: sharper distant textures, possibly shimmering.", "0",
         {{"-1", L"Sharpest (-1.0)"}, {"-0.5", L"Sharper (-0.5)"}, {"0", L"As the game"}, {"0.5", L"Softer (+0.5)"}}},
        {T_GRAPHICS, "display", "astc_quality", L"Texture compression", L"Next start. Uncompressed needs a lot of video memory.", "high",
         {{"fast", L"Fast"}, {"high", L"High"}, {"uncompressed", L"Uncompressed"}}},
        {T_GRAPHICS, "display", "bloom", L"Bloom", nullptr, "1", {}},
        {T_GRAPHICS, "display", "chromatic_aberration", L"Chromatic aberration", nullptr, "1", {}},
        {T_GRAPHICS, "hdr", "mode", L"HDR output", L"Auto: HDR when the display and Windows offer it.", "auto",
         {{"off", L"Off"}, {"auto", L"Auto"}, {"on", L"On"}, {"native", L"On, the console's levels"}}},
        {T_GRAPHICS, nullptr, nullptr, L"Performance", nullptr, nullptr, {}},
        {T_GRAPHICS, "display", "shader_prewarm", L"Prepare shaders ahead", L"Builds the pipelines earlier runs used before the draws need them.", "1", {}},
        {T_GRAPHICS, "display", "fast_gpu_memory", L"Faster GPU memory", L"Copies of the game's data in video memory (up to 2 GiB). Next start.", "1", {}},
        {T_GRAPHICS, "display", "fewer_gpu_waits", L"Fewer GPU pipeline waits", L"Next start.", "1", {}},
        // display
        {T_DISPLAY, nullptr, nullptr, L"Window", nullptr, nullptr, {}},
        {T_DISPLAY, "display", "window_mode", L"Window mode", L"Alt+Enter switches while playing.", "windowed",
         {{"windowed", L"Windowed"}, {"borderless", L"Borderless fullscreen"}, {"exclusive", L"Exclusive fullscreen"}}},
        {T_DISPLAY, "display", "#size", L"Window size", L"Fullscreen modes use the desktop resolution.", "1280x720",
         {{"1280x720", L"1280 x 720"}, {"1600x900", L"1600 x 900"}, {"1920x1080", L"1920 x 1080"}, {"2560x1080", L"2560 x 1080"}, {"2560x1440", L"2560 x 1440"},
          {"3440x1440", L"3440 x 1440"}, {"3840x2160", L"3840 x 2160"}}},
        {T_DISPLAY, "display", "monitor", L"Display", nullptr, "auto", {{"auto", L"Where the window opens"}, {"1", L"Display 1"}, {"2", L"Display 2"}, {"3", L"Display 3"}}},
        {T_DISPLAY, "display", "keep_aspect", L"Keep the aspect ratio", L"Bars instead of a stretched picture.", "1", {}},
        {T_DISPLAY, "display", "aspect", L"Aspect ratio", L"The shape of the 3D view and the HUD.", "auto",
         {{"auto", L"Auto (the window's)"}, {"16:9", L"16:9"}, {"16:10", L"16:10"}, {"21:9", L"21:9"}, {"32:9", L"32:9"}, {"4:3", L"4:3"}}},
        {T_DISPLAY, nullptr, nullptr, L"Frame rate", nullptr, nullptr, {}},
        {T_DISPLAY, "display", "fps_limit", L"Frame rate limit", L"Auto: the console's rate for games with a fixed step.", "auto",
         {{"auto", L"Auto"}, {"30", L"30 fps"}, {"60", L"60 fps"}, {"120", L"120 fps"}, {"144", L"144 fps"}, {"0", L"Unlimited"}}},
        {T_DISPLAY, "display", "vsync", L"VSync", L"With G-Sync or FreeSync, leave it off and set a limit.", "0", {}},
        {T_DISPLAY, "fps", "show", L"Frame rate counter", L"F2 shows and hides it in the game.", "0", {}},
        {T_DISPLAY, "fps", "detail", L"Counter detail", nullptr, "1", {{"0", L"Frame rate"}, {"1", L"Frame rate and frame time"}, {"2", L"Also 1% low and worst frame"}}},
        {T_DISPLAY, "fps", "corner", L"Counter position", nullptr, "top_left",
         {{"top_left", L"Top left"}, {"top_right", L"Top right"}, {"bottom_left", L"Bottom left"}, {"bottom_right", L"Bottom right"}}},
        // controller
        {T_PAD, nullptr, nullptr, L"Controller", nullptr, nullptr, {}},
        {T_PAD, "pad", "label_abxy", L"Buttons by their label", L"On: Xbox A is Switch A. Off: by position (the bottom button is Switch B).", "1", {}},
        {T_PAD, "pad", "deadzone", L"Left stick dead zone", nullptr, "0.1", kDeadzones},
        {T_PAD, "pad", "right_deadzone", L"Right stick dead zone", nullptr, "0.1", kDeadzones},
        {T_PAD, "pad", "invert_y", L"Invert the camera's up and down", nullptr, "0", {}},
        {T_PAD, "pad", "look_sens", L"Camera speed", nullptr, "1", {{"0.5", L"Slow"}, {"0.75", L"Slower"}, {"1", L"Normal"}, {"1.5", L"Faster"}, {"2", L"Fast"}}},
        {T_PAD, "pad", "trigger_threshold", L"Trigger press point", nullptr, "0.35", {{"0.15", L"Light"}, {"0.35", L"Normal"}, {"0.6", L"Deep"}}},
        {T_PAD, "pad", "gyro", L"Motion controls (gyro)", nullptr, "1", {}},
        {T_PAD, "pad", "rumble", L"Rumble", nullptr, "1", {}},
        {T_PAD, nullptr, nullptr, L"Button mapping", nullptr, nullptr, {}},
        {T_PAD, "padmap", "south", L"Bottom face button (Xbox A)", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "east", L"Right face button (Xbox B)", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "west", L"Left face button (Xbox X)", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "north", L"Top face button (Xbox Y)", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "left_shoulder", L"Left bumper", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "right_shoulder", L"Right bumper", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "left_trigger", L"Left trigger", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "right_trigger", L"Right trigger", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "start", L"Start / Menu", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "back", L"Back / View", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "left_stick", L"Left stick press", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "right_stick", L"Right stick press", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "dpad_up", L"D-pad up", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "dpad_down", L"D-pad down", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "dpad_left", L"D-pad left", nullptr, "default", kPadButtons},
        {T_PAD, "padmap", "dpad_right", L"D-pad right", nullptr, "default", kPadButtons},
        // audio
        {T_AUDIO, nullptr, nullptr, L"Sound", nullptr, nullptr, {}},
        {T_AUDIO, "audio", "volume", L"Volume", nullptr, "1",
         {{"0", L"Muted"}, {"0.1", L"10%"}, {"0.25", L"25%"}, {"0.5", L"50%"}, {"0.75", L"75%"}, {"0.9", L"90%"}, {"1", L"100%"}}},
        {T_AUDIO, "audio", "mute_unfocused", L"Mute in the background", L"Silent while the game window has no focus.", "0", {}},
    };
    return o;
}

// What a key can do: the runtime's generic action names (play_types.cpp).
const std::vector<Choice>& actions() {
    static const std::vector<Choice> a = {
        {"move_forward", L"Move forward"}, {"move_back", L"Move back"}, {"move_left", L"Move left"}, {"move_right", L"Move right"},
        {"look_up", L"Camera up"}, {"look_down", L"Camera down"}, {"look_left", L"Camera left"}, {"look_right", L"Camera right"},
        {"btn_a", L"A button"}, {"btn_b", L"B button"}, {"btn_x", L"X button"}, {"btn_y", L"Y button"}, {"btn_l", L"L button"}, {"btn_r", L"R button"},
        {"btn_zl", L"ZL button"}, {"btn_zr", L"ZR button"}, {"btn_plus", L"+ button"}, {"btn_minus", L"- button"},
        {"btn_lstick", L"Left stick press"}, {"btn_rstick", L"Right stick press"},
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

// ---- press-to-bind --------------------------------------------------------------------------------------------------------------------
struct Capture {
    std::string result;
    bool done = false;
    int left = 6;
    Fonts fonts;
    std::wstring what;
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
            const int b = mprl::sdl_mouse_button_from_message(m, w);
            if (b) finish("Mouse" + std::to_string(b));
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
        case WM_CLOSE: finish({}); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            paint_buffered(dc, rc, [&](gp::Graphics& g) {
                gp::SolidBrush bg(C(kCard));
                g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
                gp::Pen pen(C(kAccent), 2);
                g.DrawRectangle(&pen, 1, 1, rc.right - 2, rc.bottom - 2);
                HDC fdc = g.GetHDC();
                gp::Font title(fdc, c->fonts.title), font(fdc, c->fonts.font);
                g.ReleaseHDC(fdc);
                const float W = static_cast<float>(rc.right);
                text(g, L"Press a key or a mouse button", title, gp::RectF(0, 22, W, 34), kText, gp::StringAlignmentCenter);
                text(g, L"for: " + c->what, font, gp::RectF(0, 62, W, 22), kAccentHi, gp::StringAlignmentCenter);
                text(g, L"Every key can be bound, Escape included. Closes by itself in " + std::to_wstring(c->left) + L" s.", font,
                     gp::RectF(0, 96, W, 22), kMuted, gp::StringAlignmentCenter);
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
    RECT orc;
    GetWindowRect(GetAncestor(owner, GA_ROOT), &orc);
    const int w = MulDiv(460, fonts.dpi, 96), h = MulDiv(140, fonts.dpi, 96);
    HWND root = GetAncestor(owner, GA_ROOT);
    HWND wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"NxrtKeyCapture", L"Press a key", WS_POPUP | WS_VISIBLE,
                               (orc.left + orc.right - w) / 2, (orc.top + orc.bottom - h) / 2, w, h, root, nullptr, inst, &c);
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

// ---- the panel ------------------------------------------------------------------------------------------------------------------------
enum class RowKind { heading, option, binding_head, binding, add_binding, note };
struct Row {
    RowKind kind;
    int opt = -1;                   // options() index
    std::string section;            // bindings section
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
    void set_game(const std::wstring& title, const std::wstring& path, const std::wstring& why) {
        title_ = title;
        path_ = path;
        why_ = why;
        ini_ = Ini();
        if (!path_.empty()) exists_ = ini_.load(path_);
        scroll_ = 0;
        rebuild();
    }

private:
    int S(int v) const { return MulDiv(v, fonts_.dpi, 96); }
    float Sf(float v) const { return v * static_cast<float>(fonts_.dpi) / 96.f; }
    void rebuild();
    void place();
    void paint(HDC dc);
    void save();
    HWND make_combo(Row& r, const std::vector<Choice>& choices, const std::string& current);
    HWND make_button(const wchar_t* text, int style_kind);
    std::string value_of(const Opt& o) const;
    void command(int id, int code, HWND ctl);
    void draw_item(const DRAWITEMSTRUCT* d);
    void set_bindings(const std::string& section, const std::vector<std::pair<std::string, std::string>>& e);

    HWND wnd_;
    Fonts fonts_;
    std::wstring title_, path_, why_;
    Ini ini_;
    bool exists_ = false;
    int tab_ = T_GRAPHICS;
    std::vector<Row> rows_;
    std::vector<RECT> tab_rects_;
    int hover_tab_ = -1;
    int scroll_ = 0, content_h_ = 0;
    int next_id_ = kIdBase;
    HBRUSH field_ = nullptr;
    std::wstring saved_note_;
};

std::string Panel::value_of(const Opt& o) const {
    if (std::string(o.key) == "#size") {
        const std::string w = ini_.get("display", "width", "1280"), h = ini_.get("display", "height", "720");
        return w + "x" + h;
    }
    return ini_.get(o.section, o.key, o.def);
}

HWND Panel::make_combo(Row& r, const std::vector<Choice>& choices, const std::string& current) {
    HWND cb = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, S(400), wnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(next_id_++)), GetModuleHandleW(nullptr), nullptr);
    SetWindowTheme(cb, L"DarkMode_CFD", nullptr);
    SendMessageW(cb, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.font), FALSE);
    int sel = -1;
    for (const Choice& c : choices) {
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.label));
        r.values.push_back(c.value);
        if (numeric_equal(c.value, current)) sel = static_cast<int>(r.values.size()) - 1;
    }
    if (sel < 0 && !current.empty()) {
        // a value from the file that the list does not have: keep it selectable
        const std::wstring t = widen(current) + L" (from the file)";
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
        r.values.push_back(current);
        sel = static_cast<int>(r.values.size()) - 1;
    }
    SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    return cb;
}

// style_kind: 0 switch, 1 keycap, 2 small button, 3 remove (x)
HWND Panel::make_button(const wchar_t* t, int style_kind) {
    HWND b = CreateWindowExW(0, WC_BUTTONW, t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, wnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(next_id_++)), GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(b, GWLP_USERDATA, style_kind);
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.bold), FALSE);
    return b;
}

void Panel::rebuild() {
    for (Row& r : rows_) for (HWND h : {r.ctl, r.ctl2, r.ctl3}) if (h) DestroyWindow(h);
    rows_.clear();
    next_id_ = kIdBase;
    if (path_.empty()) { content_h_ = 0; place(); InvalidateRect(wnd_, nullptr, FALSE); return; }
    int y = S(76);
    auto add = [&](Row r, int h) { r.y = y; r.h = h; y += h; rows_.push_back(std::move(r)); return &rows_.back(); };
    if (tab_ == T_KEYS) {
        // the [bindings.*] sections the file has; without any, the game's factory bindings apply
        std::vector<std::string> secs;
        for (const std::string& s : ini_.sections()) if (s.rfind("bindings.", 0) == 0) secs.push_back(s);
        if (secs.empty()) {
            Row n{RowKind::note};
            n.textv = L"Using the game's default keys. Start the game once to edit them here.";
            add(n, S(64));
            secs.push_back("bindings.all");
        }
        for (const std::string& sec : secs) {
            Row hd{RowKind::binding_head};
            hd.section = sec;
            const std::string ctx = sec.substr(9);
            hd.textv = ctx == "all" ? L"Keys everywhere" : ctx == "menu" ? L"Keys in menus" : L"Keys: " + widen(ctx);
            add(hd, S(48));
            const auto e = ini_.entries(sec);
            for (size_t i = 0; i < e.size(); ++i) {
                Row r{RowKind::binding};
                r.section = sec;
                r.bind = static_cast<int>(i);
                Row* p = add(r, S(44));
                p->ctl = make_button(widen(e[i].first).c_str(), 1);
                p->ctl2 = make_combo(*p, actions(), e[i].second);
                p->ctl3 = make_button(L"✕", 3);
            }
            Row ab{RowKind::add_binding};
            ab.section = sec;
            Row* p = add(ab, S(52));
            p->ctl = make_button(L"+  Add a key", 2);
        }
    } else {
        const auto& o = options();
        for (size_t i = 0; i < o.size(); ++i) {
            if (o[i].tab != tab_) continue;
            if (!o[i].section) {
                Row hd{RowKind::heading};
                hd.textv = o[i].label;
                add(hd, S(48));
                continue;
            }
            Row r{RowKind::option};
            r.opt = static_cast<int>(i);
            Row* p = add(r, S(48));   // labels only: the choices speak for themselves (hints are kept in the table for later tooltips)
            const std::string v = value_of(o[i]);
            if (o[i].choices.empty()) {
                p->on = v == "1" || lower(v) == "true" || lower(v) == "on";
                p->ctl = make_button(L"", 0);
            } else p->ctl = make_combo(*p, o[i].choices, v);
        }
    }
    content_h_ = y + S(24);
    place();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void Panel::place() {
    RECT rc;
    GetClientRect(wnd_, &rc);
    const int right = rc.right - S(40);   // the end of the heading lines
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_h_), static_cast<UINT>(rc.bottom), scroll_};
    SetScrollInfo(wnd_, SB_VERT, &si, TRUE);
    HDWP dw = BeginDeferWindowPos(static_cast<int>(rows_.size() * 3));
    for (Row& r : rows_) {
        const int y = r.y - scroll_;
        // controls above the tab bar would cover it: hide what scrolled under it
        const bool vis = y + r.h > S(60) && y < rc.bottom;
        auto put = [&](HWND h, int x, int yy, int w, int hh) {
            if (h) dw = DeferWindowPos(dw, h, nullptr, x, yy, w, hh, SWP_NOZORDER | SWP_NOACTIVATE | (vis ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        };
        if (r.kind == RowKind::option) {
            if (options()[static_cast<size_t>(r.opt)].choices.empty()) put(r.ctl, right - S(56), y + (r.h - S(28)) / 2, S(56), S(28));
            else put(r.ctl, right - S(280), y + (r.h - S(30)) / 2, S(280), S(400));
        } else if (r.kind == RowKind::binding) {
            put(r.ctl, S(32), y + S(6), S(180), S(32));
            put(r.ctl2, S(228), y + S(7), S(280), S(400));
            put(r.ctl3, S(520), y + S(6), S(32), S(32));
        } else if (r.kind == RowKind::add_binding) {
            put(r.ctl, S(32), y + S(8), S(160), S(34));
        }
    }
    EndDeferWindowPos(dw);
}

void Panel::save() {
    if (path_.empty()) return;
    if (!exists_) {
        // a new file: say what it is; the game fills in the rest (and its factory bindings) when it starts
        std::string head = "# Settings of " + narrow(title_) + ", written by the game library and the game's own menu. runtime/docs/CONFIG.md\n";
        if (!write_file_atomic(path_, head + ini_.text())) { saved_note_ = L"Could not write " + path_; return; }
        exists_ = true;
    } else if (!ini_.save(path_)) {
        saved_note_ = L"Could not write " + path_;
        InvalidateRect(wnd_, nullptr, FALSE);
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[64];
    std::swprintf(b, 64, L"Saved ✓");
    (void)t;
    saved_note_ = b;
    RECT top{0, 0, 100000, S(76)};
    InvalidateRect(wnd_, &top, FALSE);
}

void Panel::set_bindings(const std::string& section, const std::vector<std::pair<std::string, std::string>>& e) {
    ini_.replace_entries(section, e);
    save();
    const int keep = scroll_;
    rebuild();
    scroll_ = std::min(keep, std::max(0, content_h_ - 100));
    place();
}

void Panel::command(int id, int code, HWND ctl) {
    (void)id;
    for (Row& r : rows_) {
        if (r.kind == RowKind::option && ctl == r.ctl) {
            const Opt& o = options()[static_cast<size_t>(r.opt)];
            std::string v;
            if (o.choices.empty()) {
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
            if (std::string(o.key) == "#size") {
                const size_t x = v.find('x');
                ini_.set("display", "width", v.substr(0, x));
                ini_.set("display", "height", v.substr(x + 1));
            } else ini_.set(o.section, o.key, v);
            save();
            return;
        }
        if (r.kind == RowKind::binding && (ctl == r.ctl || ctl == r.ctl2 || ctl == r.ctl3)) {
            auto e = ini_.entries(r.section);
            if (r.bind < 0 || r.bind >= static_cast<int>(e.size())) return;
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
            const std::string key = capture_input(wnd_, fonts_, what);
            if (key.empty()) return;
            entry.first = key;
            set_bindings(r.section, e);
            return;
        }
        if (r.kind == RowKind::add_binding && ctl == r.ctl && code == BN_CLICKED) {
            const std::string key = capture_input(wnd_, fonts_, L"a new key (then choose what it does)");
            if (key.empty()) return;
            auto e = ini_.entries(r.section);
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
    const bool hot = PtInRect(&wr, pt) != 0;
    const RECT rc = d->rcItem;
    paint_buffered(d->hDC, rc, [&](gp::Graphics& g) {
        gp::SolidBrush bg(C(kBg));
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
        const float W = static_cast<float>(rc.right), H = static_cast<float>(rc.bottom);
        if (kind == 0) {
            // a switch
            gp::SolidBrush track(C(on ? kAccent : RGB(0x3A, 0x40, 0x4D)));
            fill_round(g, gp::RectF(Sf(2), Sf(3), W - Sf(4), H - Sf(6)), (H - Sf(6)) / 2, track);
            const float k = H - Sf(12);
            gp::SolidBrush knob(C(RGB(255, 255, 255)));
            g.FillEllipse(&knob, on ? W - Sf(8) - k : Sf(8), Sf(6), k, k);
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
            gp::SolidBrush cap(C(hot ? kButtonHi : kButton));
            fill_round(g, gp::RectF(0, 0, W, H - Sf(4)), Sf(7), cap);
            text(g, t, f, gp::RectF(0, 0, W, H - Sf(4)), kText, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        } else {
            gp::SolidBrush b(C(kind == 3 ? (hot ? RGB(0x5A, 0x2A, 0x2E) : kCard) : (hot ? kButtonHi : kButton)));
            fill_round(g, gp::RectF(0, 0, W, H), Sf(8), b);
            text(g, t, f, gp::RectF(0, 0, W, H), kind == 3 ? RGB(0xFF, 0xA0, 0xA0) : kText, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        }
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
            text(g, why_.empty() ? L"Choose a game in the library." : why_, font, gp::RectF(Sf(32), Sf(20), std::min(W - Sf(64), Sf(720)), Sf(120)), kMuted,
                 gp::StringAlignmentNear, gp::StringAlignmentNear, true);
            return;
        }
        // rows
        const float sy = static_cast<float>(scroll_);
        for (const Row& r : rows_) {
            const float y = static_cast<float>(r.y) - sy;
            if (y + static_cast<float>(r.h) < Sf(60) || y > static_cast<float>(rc.bottom)) continue;
            if (r.kind == RowKind::heading || r.kind == RowKind::binding_head) {
                text(g, r.textv, bold, gp::RectF(Sf(32), y + Sf(18), W - Sf(64), Sf(22)), kAccentHi);
                gp::SolidBrush line(C(kLine));
                g.FillRectangle(&line, Sf(32), y + Sf(44), W - Sf(64) - Sf(8), Sf(1));
            } else if (r.kind == RowKind::note) {
                gp::SolidBrush card(C(kCard));
                fill_round(g, gp::RectF(Sf(32), y + Sf(8), W - Sf(72), static_cast<float>(r.h) - Sf(12)), Sf(8), card);
                text(g, r.textv, font, gp::RectF(Sf(46), y + Sf(14), W - Sf(100), static_cast<float>(r.h) - Sf(20)), kMuted, gp::StringAlignmentNear,
                     gp::StringAlignmentNear, true);
            } else if (r.kind == RowKind::option) {
                const Opt& o = options()[static_cast<size_t>(r.opt)];
                const float lw = W - Sf(32) - Sf(320);
                if ((false)) {
                    text(g, o.label, font, gp::RectF(Sf(32), y + Sf(10), lw, Sf(20)), kText);
                    text(g, o.hint, little, gp::RectF(Sf(32), y + Sf(32), lw, Sf(18)), kMuted);
                } else text(g, o.label, font, gp::RectF(Sf(32), y, lw, static_cast<float>(r.h)), kText, gp::StringAlignmentNear, gp::StringAlignmentCenter);
            } else if (r.kind == RowKind::binding) {
                text(g, L"→", font, gp::RectF(Sf(212), y, Sf(16), static_cast<float>(r.h)), kMuted, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
            }
        }
        // the header over the rows: title, where it is saved, the tabs
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
            tab_rects_.push_back({static_cast<LONG>(x), static_cast<LONG>(Sf(12)), static_cast<LONG>(x + tw), static_cast<LONG>(Sf(48))});
            x += tw + Sf(8);
        }
        const std::wstring where = saved_note_;
        text(g, where, little, gp::RectF(x + Sf(12), Sf(12), W - x - Sf(44), Sf(36)), saved_note_.rfind(L"Could", 0) == 0 ? kBad : kMuted,
             gp::StringAlignmentFar, gp::StringAlignmentCenter);
        gp::SolidBrush line(C(kLine));
        g.FillRectangle(&line, Sf(32), Sf(62), W - Sf(72), Sf(1));
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
        case WM_SIZE: place(); InvalidateRect(wnd_, nullptr, FALSE); return 0;
        case WM_COMMAND: command(LOWORD(w), HIWORD(w), reinterpret_cast<HWND>(l)); return 0;
        case WM_DRAWITEM: draw_item(reinterpret_cast<const DRAWITEMSTRUCT*>(l)); return TRUE;
        case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, kText);
            SetBkColor(dc, kField);
            return reinterpret_cast<LRESULT>(field_);
        }
        case WM_MOUSEMOVE: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int hv = -1;
            for (size_t i = 0; i < tab_rects_.size(); ++i) if (PtInRect(&tab_rects_[i], p)) hv = static_cast<int>(i);
            if (hv != hover_tab_) { hover_tab_ = hv; RECT top{0, 0, 100000, S(64)}; InvalidateRect(wnd_, &top, FALSE); }
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, wnd_, 0};
            TrackMouseEvent(&t);
            SetCursor(LoadCursor(nullptr, hv >= 0 ? IDC_HAND : IDC_ARROW));
            for (const Row& r : rows_) for (HWND h : {r.ctl, r.ctl3}) if (h && GetWindowLongPtrW(h, GWLP_USERDATA) != 0) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_tab_ = -1;
            InvalidateRect(wnd_, nullptr, FALSE);
            for (const Row& r : rows_) for (HWND h : {r.ctl, r.ctl3}) if (h) InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
        case WM_LBUTTONUP: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            for (size_t i = 0; i < tab_rects_.size(); ++i)
                if (PtInRect(&tab_rects_[i], p) && static_cast<int>(i) != tab_) { tab_ = static_cast<int>(i); scroll_ = 0; saved_note_.clear(); rebuild(); }
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
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            }
            scroll_ = std::clamp(pos, 0, std::max(0, content_h_ - static_cast<int>(rc.bottom)));
            place();
            InvalidateRect(wnd_, nullptr, FALSE);
            return 0;
        }
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
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, L"NxrtSettingsPanel", L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    return h;
}

void set_fonts(HWND panel, const Fonts& f) { if (Panel* p = panel_of(panel)) p->set_fonts(f); }

void set_game(HWND panel, const std::wstring& title, const std::wstring& controls_ini, const std::wstring& why_not) {
    if (Panel* p = panel_of(panel)) p->set_game(title, controls_ini, why_not);
}

}  // namespace setup::settings
