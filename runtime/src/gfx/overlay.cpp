// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay (declarations in include/nxrt/overlay.h). ImGui and its Vulkan backend are pinned in cmake/gfx_deps.cmake.
#include "nxrt/overlay.h"

#include <atomic>
#include <mutex>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/nvn.h"
#include "nxrt/platform.h"
#include "extras/overlay_composite.h"
#include "fps_stats.h"

namespace nxrt::gfx {
namespace {
constexpr const char* kTag = "overlay";

ImGuiKey map_key(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return static_cast<ImGuiKey>(ImGuiKey_A + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return static_cast<ImGuiKey>(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
    if (sc == SDL_SCANCODE_0) return ImGuiKey_0;
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (sc - SDL_SCANCODE_F1));
    switch (sc) {
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
        case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
        case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
        case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
        case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
        case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

const double kFpsChoices[] = {0, 30, 60, 90, 120, 144, 165, 240, 360};

// SDL_GamepadButton numbers (input.h GamepadButtonDown) and SDL_GamepadAxis numbers (GamepadAxis).
enum : int { kPadSouth = 0, kPadEast, kPadWest, kPadNorth, kPadBack, kPadGuide, kPadStart, kPadLStick, kPadRStick, kPadLb, kPadRb, kPadUp, kPadDown,
             kPadLeft, kPadRight };
enum : int { kAxisLx = 0, kAxisLy, kAxisRx, kAxisRy, kAxisLt, kAxisRt };

ImGuiKey map_pad_button(int b) {
    switch (b) {
        case kPadSouth: return ImGuiKey_GamepadFaceDown;     // activate
        case kPadEast: return ImGuiKey_GamepadFaceRight;     // cancel, back
        case kPadWest: return ImGuiKey_GamepadFaceLeft;      // text input, menu layer
        case kPadNorth: return ImGuiKey_GamepadFaceUp;
        case kPadBack: return ImGuiKey_GamepadBack;
        case kPadStart: return ImGuiKey_GamepadStart;
        case kPadLStick: return ImGuiKey_GamepadL3;
        case kPadRStick: return ImGuiKey_GamepadR3;
        case kPadLb: return ImGuiKey_GamepadL1;
        case kPadRb: return ImGuiKey_GamepadR1;
        case kPadUp: return ImGuiKey_GamepadDpadUp;
        case kPadDown: return ImGuiKey_GamepadDpadDown;
        case kPadLeft: return ImGuiKey_GamepadDpadLeft;
        case kPadRight: return ImGuiKey_GamepadDpadRight;
        default: return ImGuiKey_None;
    }
}
}  // namespace

bool parse_pad_chord(const std::string& text, uint32_t* bits) {
    static const struct { const char* name; int button; } kNames[] = {
        {"south", kPadSouth}, {"a", kPadSouth}, {"east", kPadEast}, {"b", kPadEast}, {"west", kPadWest}, {"x", kPadWest}, {"north", kPadNorth},
        {"y", kPadNorth}, {"back", kPadBack}, {"view", kPadBack}, {"select", kPadBack}, {"guide", kPadGuide}, {"start", kPadStart}, {"menu", kPadStart},
        {"lstick", kPadLStick}, {"l3", kPadLStick}, {"rstick", kPadRStick}, {"r3", kPadRStick}, {"lb", kPadLb}, {"l1", kPadLb}, {"rb", kPadRb},
        {"r1", kPadRb}, {"dpad_up", kPadUp}, {"dpad_down", kPadDown}, {"dpad_left", kPadLeft}, {"dpad_right", kPadRight},
    };
    uint32_t out = 0;
    std::string word;
    auto take = [&]() {
        if (word.empty()) return true;
        if (word == "none") {
            word.clear();
            return true;
        }
        for (const auto& n : kNames) {
            if (word == n.name) {
                out |= 1u << n.button;
                word.clear();
                return true;
            }
        }
        return false;
    };
    for (char c : text) {
        if (c == '+' || c == ',' || c == ' ') {
            if (!take()) return false;
        } else {
            word += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
    }
    if (!take()) return false;
    *bits = out;
    return true;
}

namespace {
std::atomic<int> g_shader_backlog{0};
}
void set_shader_backlog(int pending) { g_shader_backlog.store(pending < 0 ? 0 : pending, std::memory_order_relaxed); }
int shader_backlog() { return g_shader_backlog.load(std::memory_order_relaxed); }

struct Overlay::Impl {
    Context ctx;
    ImGuiContext* imgui = nullptr;
    ImGuiStyle base_style;            // the style at scale 1; style = base_style scaled by ui_scale (update_scale)
    float ui_scale = 0.f;             // 0: not applied yet
    bool backend_ready = false;
    std::atomic<bool> visible{false};
    std::mutex mu;                    // guards the ImGui context: events arrive on the game's main thread, drawing happens on the presenting thread
    int toggle_sc = SDL_SCANCODE_F1;
    uint32_t pad_chord = (1u << kPadBack) | (1u << kPadStart);   // [gfx] overlay_pad_chord
    struct PadHeld {
        uint32_t id = 0;
        uint32_t mask = 0;
        bool used = false;
    };
    PadHeld held[8];                  // buttons down per pad (the chord), from the input events
    std::atomic<uint64_t> hides{0};
    std::atomic<bool> focus_next{false};   // the menu was just shown: give it the keyboard and gamepad focus
    bool restore_relative = false;
    std::chrono::steady_clock::time_point last_frame = std::chrono::steady_clock::now();
    uint64_t drawn = 0;

    struct Panel { std::string title; std::function<void()> fn; };
    std::vector<Panel> panels;

    // menu state applied by apply_pending()
    bool want_mode = false;
    PresentMode new_mode = PresentMode::Fifo;
    bool want_fullscreen = false;
    platform::FullscreenMode new_fullscreen = platform::FullscreenMode::Windowed;

    int fps_choice = 0;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    bool hdr = false;                 // HDR swapchain: the overlay is drawn into an RGBA8 target and composited (draw)
    extras::OverlayComposite composite;

    // Corner frame-rate counter. set_counter() stores the settings from any thread; the rest is used by the presenting thread only.
    std::atomic<bool> ptr_show{false}, ptr_in_shots{true};
    std::atomic<float> ptr_x{0}, ptr_y{0};
    std::atomic<bool> counter_show{false};
    std::atomic<int> counter_corner{0};
    std::atomic<int> counter_detail{1};
    std::atomic<bool> counter_in_shots{false};
    bool counter_was_on = false;
    bool counter_have_prev = false;
    std::chrono::steady_clock::time_point counter_prev{};
    FpsStats stats;
    double text_s = -1e9;                 // time of the last text refresh, seconds (far past: refresh at once)
    FpsStats::Snapshot snap;              // the snapshot taken at text_s
    std::string counter_lines[4];
    int counter_n = 0;

    void build_ui();
    void update_scale(const VkExtent2D& extent);
    void refresh_counter_text(double now_s);
    void draw_counter(const FrameInfo& f, double now_s);
    // The "Preparing shaders" note: shown once the backlog has stood for a second and a half, gone a second after it is empty.
    bool note_enabled = true;
    bool note_read = false;
    double note_since_s = -1.0, note_empty_s = -1.0;
    int note_peak = 0;
    bool note_visible(double now_s);
    void draw_note(const FrameInfo& f);
};

void Overlay::Impl::build_ui() {
    Renderer& r = *ctx.renderer;
    // First-use layout, in units of the UI scale (update_scale): the panels (the settings menu) sit at the top left, the diagnostics window
    // at the top right and folded up. The player moves them; nothing is remembered between runs.
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float diag_w = std::min(420.f * k, std::max(200.f, disp.x - 32.f * k));
    if (ctx.diagnostics_window) {   // (without the window these would apply to the next Begin: the menu would open folded up)
        ImGui::SetNextWindowPos(ImVec2(std::max(16.f * k, disp.x - diag_w - 16.f * k), 16.f * k), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(diag_w, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    }
    if (ctx.diagnostics_window && ImGui::Begin("Diagnostics")) {
        const GpuInfo& g = ctx.device->gpu();
        ImGui::Text("GPU: %s", g.name.c_str());
        ImGui::Text("Vulkan 1.3, driver 0x%08x, validation %s%s", g.driver_version, ctx.device->validation_enabled() ? "on" : "off",
                    ctx.device->headless() ? ", headless" : "");
        const VkExtent2D e = r.extent();
        ImGui::Text("Target: %ux%u, %u images, %u frames in flight", e.width, e.height, r.image_count(), r.frames_in_flight());

        ImGui::SeparatorText("Presentation");
        const PresentMode modes[] = {PresentMode::Fifo, PresentMode::FifoRelaxed, PresentMode::Mailbox, PresentMode::Immediate};
        int cur = static_cast<int>(r.requested_present_mode());
        if (ImGui::BeginCombo("Present mode", present_mode_name(modes[cur]))) {
            for (int i = 0; i < 4; ++i) {
                const bool ok = r.present_mode_supported(modes[i]);
                ImGui::BeginDisabled(!ok);
                if (ImGui::Selectable(present_mode_name(modes[i]), i == cur)) { want_mode = true; new_mode = modes[i]; }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (r.present_mode() != r.requested_present_mode()) ImGui::TextDisabled("in use: %s (requested mode unavailable)", present_mode_name(r.present_mode()));

        if (ctx.limiter) {
            char label[32];
            const double t = ctx.limiter->target_fps();
            std::snprintf(label, sizeof label, t > 0 ? "%.0f fps" : "unlimited", t);
            if (ImGui::BeginCombo("Frame cap", label)) {
                for (double v : kFpsChoices) {
                    std::snprintf(label, sizeof label, v > 0 ? "%.0f fps" : "unlimited", v);
                    if (ImGui::Selectable(label, v == t)) ctx.limiter->set_target_fps(v);
                }
                ImGui::EndCombo();
            }
            const float hz = platform::has_window() ? platform::display_refresh_hz() : 0.f;
            if (hz > 0) {
                ImGui::TextDisabled("display %.2f Hz; VRR-friendly cap %.1f fps", hz, suggest_vrr_cap(hz));
                if (ImGui::SmallButton("Use VRR cap")) ctx.limiter->set_target_fps(suggest_vrr_cap(hz));
            }
        }

        if (ctx.has_window && platform::has_window()) {
            ImGui::SeparatorText("Window");
            const auto fm = platform::fullscreen_mode();
            if (ImGui::RadioButton("Windowed", fm == platform::FullscreenMode::Windowed)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Windowed; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Borderless", fm == platform::FullscreenMode::Borderless)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Borderless; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Exclusive", fm == platform::FullscreenMode::Exclusive)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Exclusive; }
            bool rel = platform::relative_mouse();
            if (ImGui::Checkbox("Raw mouse (relative mode)", &rel)) platform::set_relative_mouse(rel);
            ImGui::SameLine();
            ImGui::TextDisabled(platform::relative_mouse_is_raw() ? "(unaccelerated)" : "(system scaling!)");
        }

        ImGui::SeparatorText("Debug");
        draw_debug_page(ctx);
    }
    if (ctx.diagnostics_window) ImGui::End();
    // The panels (play mode: the one menu window). The first one is centred and large; it takes the focus when the overlay is shown, so the
    // keyboard and the gamepad navigate it at once.
    const bool focus = focus_next.exchange(false);
    for (size_t i = 0; i < panels.size(); ++i) {
        Panel& p = panels[i];
        if (i == 0) {
            const ImVec2 size(std::min(1120.f * k, std::max(240.f, disp.x - 32.f * k)), std::min(860.f * k, std::max(200.f, disp.y - 32.f * k)));
            ImGui::SetNextWindowPos(ImVec2(std::max(0.f, (disp.x - size.x) * 0.5f), std::max(0.f, (disp.y - size.y) * 0.5f)), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
            if (focus) ImGui::SetNextWindowFocus();
        } else {
            ImGui::SetNextWindowPos(ImVec2(16.f * k, 16.f * k), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(std::min(760.f * k, std::max(240.f, disp.x - 32.f * k)), std::min(680.f * k, std::max(200.f, disp.y - 32.f * k))),
                                     ImGuiCond_FirstUseEver);
        }
        if (ImGui::Begin(p.title.c_str())) p.fn();
        ImGui::End();
    }
}

// DPI: the UI is laid out for a 1080 pixel high frame at 16 px text and grows with the frame (4K: twice the size; a Windows display scaling
// of 150% shows up as more pixels in the same window, so it is covered too). Never below 1: a 720p window keeps the 16 px text.
void Overlay::Impl::update_scale(const VkExtent2D& extent) {
    const float want = std::round(std::clamp(static_cast<float>(extent.height) / 1080.f, 1.0f, 3.0f) * 20.f) / 20.f;   // steps of 0.05
    if (want == ui_scale) return;
    ui_scale = want;
    ImGuiStyle& style = ImGui::GetStyle();
    style = base_style;
    style.ScaleAllSizes(want);
    style.FontScaleDpi = want;
}

std::unique_ptr<Overlay> Overlay::create(const Context& ctx, std::string* error) {
    if (!ctx.device || !ctx.renderer) {
        if (error) *error = "overlay needs a device and a renderer";
        return nullptr;
    }
    std::unique_ptr<Overlay> o(new Overlay());
    o->impl_ = new Impl();
    Impl& s = *o->impl_;
    s.ctx = ctx;
    s.color_format = ctx.renderer->format();
    if (ctx.renderer->output_kind() != extras::OutputKind::Sdr8) {
        // HDR swapchain: ImGui draws into an RGBA8 target that the composite maps onto the swapchain (see draw)
        if (s.composite.init(*ctx.device)) {
            s.hdr = true;
            s.color_format = VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            NXLOG_W(kTag, "HDR overlay composite failed to initialise: the overlay is drawn straight into the HDR swapchain (colours will look wrong)");
        }
    }
    const std::string key = config_get_str("gfx", "overlay_key", "F1");
    if (const SDL_Scancode sc = SDL_GetScancodeFromName(key.c_str()); sc != SDL_SCANCODE_UNKNOWN) s.toggle_sc = sc;
    else NXLOG_W(kTag, "unknown [gfx] overlay_key '%s': using F1", key.c_str());
    const std::string chord = config_get_str("gfx", "overlay_pad_chord", "back+start");
    if (!parse_pad_chord(chord, &s.pad_chord)) NXLOG_W(kTag, "unknown [gfx] overlay_pad_chord '%s': using back+start", chord.c_str());

    IMGUI_CHECKVERSION();
    s.imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // no imgui.ini next to the exe
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui::StyleColorsDark();
    io.Fonts->AddFontDefaultVector();          // the scalable default font: stays sharp at every UI scale (update_scale)
    ImGui::GetStyle().FontSizeBase = 16.0f;
    s.base_style = ImGui::GetStyle();
    const VkExtent2D e = ctx.renderer->extent();
    io.DisplaySize = ImVec2(static_cast<float>(e.width), static_cast<float>(e.height));

    VkPipelineRenderingCreateInfoKHR rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &s.color_format;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = ctx.device->instance();
    ii.PhysicalDevice = ctx.device->physical();
    ii.Device = ctx.device->device();
    ii.QueueFamily = ctx.device->queue_family();
    ii.Queue = ctx.device->queue();
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = std::max(2u, ctx.renderer->image_count());
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = rci;
    ii.CheckVkResultFn = [](VkResult r) { if (r != VK_SUCCESS) NXLOG_E(kTag, "ImGui Vulkan call failed (%d)", static_cast<int>(r)); };
    if (!ImGui_ImplVulkan_Init(&ii)) {
        if (error) *error = "ImGui_ImplVulkan_Init failed";
        ImGui::DestroyContext(s.imgui);
        s.imgui = nullptr;
        return nullptr;
    }
    s.backend_ready = true;
    NXLOG_I(kTag, "overlay ready (toggle key %s, gamepad chord %s)", SDL_GetScancodeName(static_cast<SDL_Scancode>(s.toggle_sc)),
            s.pad_chord ? chord.c_str() : "none");
    return o;
}

Overlay::~Overlay() {
    if (!impl_) return;
    if (impl_->ctx.device) impl_->ctx.device->wait_idle();
    impl_->composite.shutdown();
    if (impl_->backend_ready) ImGui_ImplVulkan_Shutdown();
    if (impl_->imgui) ImGui::DestroyContext(impl_->imgui);
    delete impl_;
}

void Overlay::set_visible(bool v) {
    Impl& s = *impl_;
    if (s.visible == v) return;
    s.visible = v;
    if (v) {
        s.focus_next = true;
        s.restore_relative = platform::has_window() && platform::relative_mouse();
        if (s.restore_relative) platform::set_relative_mouse(false);
    } else {
        s.hides.fetch_add(1);
        if (s.restore_relative && platform::has_window()) platform::set_relative_mouse(true);
        s.restore_relative = false;
        std::lock_guard<std::mutex> lk(s.mu);
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s.imgui);
        ImGuiIO& io = ImGui::GetIO();   // drop pressed keys and buttons so nothing stays latched
        io.ClearInputKeys();
        io.ClearInputMouse();
        ImGui::SetCurrentContext(prev);
    }
    NXLOG_I(kTag, "overlay %s", v ? "shown" : "hidden");
}

bool Overlay::visible() const { return impl_->visible; }
void Overlay::toggle() { set_visible(!impl_->visible); }
int Overlay::toggle_scancode() const { return impl_->toggle_sc; }
void Overlay::set_toggle_scancode(int sc) { impl_->toggle_sc = sc; }
uint64_t Overlay::frames_drawn() const { return impl_->drawn; }
uint64_t Overlay::hide_count() const { return impl_->hides.load(); }
uint32_t Overlay::pad_chord() const { return impl_->pad_chord; }

void Overlay::set_pointer(bool show, float x, float y, bool in_screenshots) {
    Impl& s = *impl_;
    s.ptr_x.store(x, std::memory_order_relaxed);
    s.ptr_y.store(y, std::memory_order_relaxed);
    s.ptr_in_shots.store(in_screenshots, std::memory_order_relaxed);
    s.ptr_show.store(show, std::memory_order_relaxed);
}

void Overlay::set_counter(const CounterConfig& c) {
    Impl& s = *impl_;
    s.counter_corner.store(std::clamp(c.corner, 0, 3), std::memory_order_relaxed);
    s.counter_detail.store(std::clamp(c.detail, 0, 2), std::memory_order_relaxed);
    s.counter_in_shots.store(c.in_screenshots, std::memory_order_relaxed);
    s.counter_show.store(c.show, std::memory_order_relaxed);   // last: the other settings are in place when it turns on
}

namespace {
bool imgui_wants(std::mutex& mu, ImGuiContext* ctx, bool mouse, bool keyboard) {
    std::lock_guard<std::mutex> lk(mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx);
    const ImGuiIO& io = ImGui::GetIO();
    const bool w = (mouse && io.WantCaptureMouse) || (keyboard && io.WantCaptureKeyboard);
    ImGui::SetCurrentContext(prev);
    return w;
}
}  // namespace

bool Overlay::wants_input() const { return impl_->visible && imgui_wants(impl_->mu, impl_->imgui, true, true); }
bool Overlay::wants_mouse() const { return impl_->visible && imgui_wants(impl_->mu, impl_->imgui, true, false); }
bool Overlay::wants_keyboard() const { return impl_->visible && imgui_wants(impl_->mu, impl_->imgui, false, true); }

void Overlay::add_panel(const std::string& title, std::function<void()> fn) { impl_->panels.push_back({title, std::move(fn)}); }

bool Overlay::handle_event(const input::Event& e) {
    Impl& s = *impl_;
    using input::EventType;
    if ((e.type == EventType::KeyDown || e.type == EventType::KeyUp) && e.u.key.scancode == s.toggle_sc) {
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) toggle();
        return true;   // the toggle key never reaches the game (press, repeat and release)
    }
    if (e.type == EventType::GamepadButtonDown || e.type == EventType::GamepadButtonUp) {
        // the buttons held on this pad, for the chord
        Impl::PadHeld* h = nullptr;
        for (auto& x : s.held)
            if (x.used && x.id == e.u.gpbutton.id) h = &x;
        for (auto& x : s.held) {
            if (!h && !x.used) {
                h = &x;
                h->used = true;
                h->id = e.u.gpbutton.id;
                h->mask = 0;
            }
        }
        const uint32_t bit = e.u.gpbutton.button < 32 ? 1u << e.u.gpbutton.button : 0u;
        if (h) h->mask = e.type == EventType::GamepadButtonDown ? (h->mask | bit) : (h->mask & ~bit);
        if (e.type == EventType::GamepadButtonDown && s.pad_chord && (s.pad_chord & bit) && h && (h->mask & s.pad_chord) == s.pad_chord) {
            toggle();
            return true;
        }
        // Start alone closes the menu (not while it completes the chord: that toggled above)
        if (s.visible && e.type == EventType::GamepadButtonDown && e.u.gpbutton.button == kPadStart) {
            set_visible(false);
            return true;
        }
    } else if (e.type == EventType::GamepadRemoved) {
        for (auto& x : s.held)
            if (x.used && x.id == e.u.gamepad.id) x = Impl::PadHeld{};
    }
    if (!s.visible) return false;
    std::lock_guard<std::mutex> lk(s.mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.imgui);
    ImGuiIO& io = ImGui::GetIO();
    bool consumed = false;
    switch (e.type) {
        case EventType::GamepadButtonDown:
        case EventType::GamepadButtonUp: {
            const ImGuiKey k = map_pad_button(e.u.gpbutton.button);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == EventType::GamepadButtonDown);
            consumed = true;   // while the menu is up the pads belong to it
            break;
        }
        case EventType::GamepadAxis: {
            const float v = std::clamp(static_cast<float>(e.u.gpaxis.value) / 32767.f, -1.f, 1.f);
            auto analog = [&](ImGuiKey neg, ImGuiKey pos, float x) {
                constexpr float kDead = 0.30f;   // stick travel before it navigates
                const float n = std::clamp((-x - kDead) / (1.f - kDead), 0.f, 1.f), p = std::clamp((x - kDead) / (1.f - kDead), 0.f, 1.f);
                io.AddKeyAnalogEvent(neg, n > 0.f, n);
                io.AddKeyAnalogEvent(pos, p > 0.f, p);
            };
            switch (e.u.gpaxis.axis) {
                case kAxisLx: analog(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, v); break;
                case kAxisLy: analog(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, v); break;   // SDL: down = +
                case kAxisRx: analog(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, v); break;
                case kAxisRy: analog(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, v); break;
                case kAxisLt: io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, v > 0.5f, std::max(v, 0.f)); break;
                case kAxisRt: io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, v > 0.5f, std::max(v, 0.f)); break;
                default: break;
            }
            consumed = true;
            break;
        }
        case EventType::KeyDown:
        case EventType::KeyUp: {
            const ImGuiKey k = map_key(e.u.key.scancode);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == EventType::KeyDown);
            consumed = io.WantCaptureKeyboard;
            break;
        }
        case EventType::Text:
            io.AddInputCharactersUTF8(e.u.text.utf8);
            consumed = io.WantTextInput;
            break;
        case EventType::MouseMotion:
            io.AddMousePosEvent(e.u.motion.x, e.u.motion.y);
            consumed = io.WantCaptureMouse;
            break;
        case EventType::MouseButtonDown:
        case EventType::MouseButtonUp: {
            const int b = e.u.button.button;   // SDL: 1 left, 2 middle, 3 right
            const int ib = b == 1 ? 0 : b == 3 ? 1 : b == 2 ? 2 : b - 1;
            io.AddMousePosEvent(e.u.button.x, e.u.button.y);
            io.AddMouseButtonEvent(ib, e.type == EventType::MouseButtonDown);
            consumed = io.WantCaptureMouse;
            break;
        }
        case EventType::MouseWheel:
            io.AddMouseWheelEvent(e.u.wheel.dx, e.u.wheel.dy);
            consumed = io.WantCaptureMouse;
            break;
        case EventType::WindowFocusLost: io.AddFocusEvent(false); break;
        case EventType::WindowFocusGained: io.AddFocusEvent(true); break;
        default: break;
    }
    ImGui::SetCurrentContext(prev);
    return consumed;
}

void Overlay::apply_pending() {
    apply_present_pending();
    apply_window_pending();
}

void Overlay::apply_present_pending() {
    Impl& s = *impl_;
    if (s.want_mode) {
        s.want_mode = false;
        s.ctx.renderer->set_present_mode(s.new_mode);
    }
}

void Overlay::apply_window_pending() {
    Impl& s = *impl_;
    if (s.want_fullscreen) {
        s.want_fullscreen = false;
        platform::set_fullscreen(s.new_fullscreen);
    }
}

void Overlay::Impl::refresh_counter_text(double now_s) {
    if (now_s - text_s < 0.25) return;    // the text is rebuilt at most every 250 ms
    text_s = now_s;
    snap = stats.snapshot(now_s);
    const int detail = counter_detail.load(std::memory_order_relaxed);
    char buf[64];
    counter_n = 0;
    if (snap.valid) std::snprintf(buf, sizeof buf, "%.0f FPS", snap.fps);
    else std::snprintf(buf, sizeof buf, "-- FPS");
    counter_lines[counter_n++] = buf;
    if (detail >= 1) {
        if (snap.valid) std::snprintf(buf, sizeof buf, "%.1f ms", snap.avg_ms);
        else std::snprintf(buf, sizeof buf, "-- ms");
        counter_lines[counter_n++] = buf;
    }
    if (detail >= 2) {
        if (snap.valid) std::snprintf(buf, sizeof buf, "1%% low %.0f", snap.low1_fps);
        else std::snprintf(buf, sizeof buf, "1%% low --");
        counter_lines[counter_n++] = buf;
        if (snap.valid) std::snprintf(buf, sizeof buf, "max %.1f ms", snap.max_ms);
        else std::snprintf(buf, sizeof buf, "max -- ms");
        counter_lines[counter_n++] = buf;
    }
}

// Draws the counter with the foreground draw list: no window, so it never takes mouse or keyboard input.
void Overlay::Impl::draw_counter(const FrameInfo& f, double now_s) {
    refresh_counter_text(now_s);
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 45.f), 14.f, 40.f);
    const float pad = size * 0.4f;
    const float gap = size * 0.15f;
    const float margin = size * 0.8f;
    const int n = counter_n;
    float lines_w[4] = {};
    float line_h = 0, text_w = 0;
    for (int i = 0; i < n; ++i) {
        const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, counter_lines[i].c_str());
        lines_w[i] = sz.x;
        line_h = std::max(line_h, sz.y);
        text_w = std::max(text_w, sz.x);
    }
    const float box_w = text_w + 2.f * pad;
    const float box_h = static_cast<float>(n) * line_h + static_cast<float>(n - 1) * gap + 2.f * pad;
    const int corner = counter_corner.load(std::memory_order_relaxed);
    const bool right = corner == 1 || corner == 3;
    const bool bottom = corner >= 2;
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float x0 = right ? W - margin - box_w : margin;
    const float y0 = bottom ? H - margin - box_h : margin;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + box_w, y0 + box_h), IM_COL32(0, 0, 0, 140), size * 0.3f);
    for (int i = 0; i < n; ++i) {
        const float x = right ? x0 + box_w - pad - lines_w[i] : x0 + pad;
        const float y = y0 + pad + static_cast<float>(i) * (line_h + gap);
        dl->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 255, 255), counter_lines[i].c_str());
    }
}

bool Overlay::Impl::note_visible(double now_s) {
    if (!note_read) {
        note_read = true;
        note_enabled = nxrt::config_get_bool("nvn", "compile_note", true);
    }
    const int pending = shader_backlog();
    if (!note_enabled) return false;
    if (pending > 0) {
        if (note_since_s < 0) { note_since_s = now_s; note_peak = 0; }
        note_empty_s = -1.0;
        note_peak = std::max(note_peak, pending);
        return now_s - note_since_s >= 1.5;
    }
    if (note_since_s < 0) return false;
    if (note_empty_s < 0) note_empty_s = now_s;
    if (now_s - note_empty_s >= 1.0) { note_since_s = -1.0; return false; }
    return now_s - note_since_s >= 1.5;
}

void Overlay::Impl::draw_note(const FrameInfo& f) {
    char text[96];
    const int pending = shader_backlog();
    if (pending > 0) std::snprintf(text, sizeof text, "Preparing shaders (%d left)", pending);
    else std::snprintf(text, sizeof text, "Preparing shaders (done)");
    ImFont* font = ImGui::GetFont();
    const float size = std::clamp(std::round(static_cast<float>(f.extent.height) / 60.f), 12.f, 28.f);
    const float pad = size * 0.4f;
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float W = static_cast<float>(f.extent.width), H = static_cast<float>(f.extent.height);
    const float x0 = (W - sz.x) * 0.5f - pad, y0 = H - size * 0.8f - sz.y - 2.f * pad;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sz.x + 2.f * pad, y0 + sz.y + 2.f * pad), IM_COL32(0, 0, 0, 120), size * 0.3f);
    dl->AddText(font, size, ImVec2(x0 + pad, y0 + pad), IM_COL32(255, 255, 255, 220), text);
}

void Overlay::draw(const FrameInfo& f) {
    Impl& s = *impl_;
    const bool counter_on = s.counter_show.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const double now_s = std::chrono::duration<double>(now.time_since_epoch()).count();
    const bool note_on = s.note_visible(now_s) && (s.counter_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    const bool ptr_on = s.ptr_show.load(std::memory_order_relaxed) && (s.ptr_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    if (!s.visible && !counter_on && !note_on && !ptr_on) {
        s.last_frame = now;
        s.counter_was_on = false;
        return;
    }
    if (counter_on) {
        if (!s.counter_was_on) {          // just switched on: start from an empty history
            s.counter_was_on = true;
            s.counter_have_prev = false;
            s.stats.reset();
            s.text_s = -1e9;
        }
        if (s.counter_have_prev) {
            const double dt_ms = std::chrono::duration<double, std::milli>(now - s.counter_prev).count();
            if (dt_ms <= 1000.0) s.stats.push(now_s, dt_ms);   // over a second: a pause (debugger, window in the background), not a frame
        }
        s.counter_prev = now;
        s.counter_have_prev = true;
    } else {
        s.counter_was_on = false;
    }
    // a screenshot leaves the counter out unless asked for; with the menu hidden there is then nothing to draw
    const bool show_counter = counter_on && (s.counter_in_shots.load(std::memory_order_relaxed) || !s.ctx.renderer->capture_pending());
    if (!s.visible && !show_counter && !note_on && !ptr_on) {
        s.last_frame = now;
        return;
    }
    const bool hdr_frame = s.hdr && f.hdr_output;
    if (s.hdr && !f.hdr_output) return;      // the display left HDR mode: the RGBA8 overlay pipeline does not match the SDR swapchain, so no overlay until the next start
    if (hdr_frame && !s.composite.ensure_target(f.extent)) return;
    std::lock_guard<std::mutex> lk(s.mu);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(s.imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = std::clamp(std::chrono::duration<float>(now - s.last_frame).count(), 1.0f / 1000.f, 0.25f);
    s.last_frame = now;
    io.DisplaySize = ImVec2(static_cast<float>(f.extent.width), static_cast<float>(f.extent.height));

    s.update_scale(f.extent);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (s.visible) s.build_ui();
    if (show_counter) s.draw_counter(f, now_s);
    if (note_on) s.draw_note(f);
    if (ptr_on) {   // a plain arrow, white with a dark outline, sized with the frame height
        const float k = std::max(1.f, static_cast<float>(f.extent.height) / 720.f);
        const float x = s.ptr_x.load(std::memory_order_relaxed), y = s.ptr_y.load(std::memory_order_relaxed);
        const ImVec2 pts[7] = {{0, 0}, {0, 16 * k}, {4 * k, 12.5f * k}, {7.5f * k, 19 * k}, {10 * k, 18 * k}, {6.5f * k, 11.5f * k}, {12 * k, 11.5f * k}};
        ImVec2 poly[7];
        for (int i = 0; i < 7; ++i) poly[i] = ImVec2(x + pts[i].x, y + pts[i].y);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddConvexPolyFilled(poly, 7, IM_COL32(255, 255, 255, 255));
        dl->AddPolyline(poly, 7, IM_COL32(0, 0, 0, 255), ImDrawFlags_Closed, 1.5f * k);
    }
    ImGui::Render();

    // one dynamic-rendering pass that draws the ImGui draw data into view
    auto render_into = [&](VkImageView view, VkAttachmentLoadOp load) {
        VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = load;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, f.extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(f.cmd, &ri);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), f.cmd);
        vkCmdEndRendering(f.cmd);
    };
    if (hdr_frame) {
        // HDR: the overlay goes into the cleared RGBA8 target, then the composite maps it onto the swapchain image (UI white = paper white)
        const extras::HdrState hs = extras::hdr_state();
        s.composite.begin_overlay(f.cmd);
        render_into(s.composite.target_view(), VK_ATTACHMENT_LOAD_OP_CLEAR);
        s.composite.composite(f.cmd, f.view, f.format, f.extent, s.ctx.renderer->output_kind(), hs.paper_white_nits, hs.peak_nits);
    } else {
        render_into(f.view, VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    s.drawn++;
    ImGui::SetCurrentContext(prev);
}

void Overlay::install_on_renderer() {
    impl_->ctx.renderer->set_overlay_hook([this](const FrameInfo& f) { draw(f); });
}

}  // namespace nxrt::gfx
