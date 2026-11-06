// SPDX-License-Identifier: GPL-3.0-or-later
// The Debug page of the F1 menu (overlay.h draw_debug_page): performance and the frame-time graph, hitches, the GPU and its video memory, shader
// and pipeline work, the NVN back end's counters, the log tail and every gauge. Game independent: it reads the runtime's own counters.
//
// Threads: the page is drawn on the presenting thread. What could wait on a lock another thread holds while it presents (the gauges call into
// every subsystem, the shader statistics take the pipeline builder's lock, the unsupported-operation report) is read by a small sampler thread
// once a second while the page is on screen; the page shows that copy. Lock-free counters (frame times, back-end counters) are read directly.
#include "nxrt/overlay.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/nvn.h"
#include "nxrt/nvn_backend.h"

namespace nxrt::gfx {
namespace {

const ImVec4 kRed(1.0f, 0.40f, 0.40f, 1.0f);
const ImVec4 kOrange(1.0f, 0.70f, 0.30f, 1.0f);
const ImVec4 kGrey(0.62f, 0.62f, 0.66f, 1.0f);

float px(float v) { return v * ImGui::GetFontSize() / 16.0f; }
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }

// ---- the sampler ----------------------------------------------------------------------------------------------------------------------------
struct Sample {
    bool have = false;
    double at_s = 0;
    std::vector<std::pair<std::string, int64_t>> gauges;
    nvn::ShaderStats shaders;
    std::string unsupported;
    uint64_t working_set = 0, private_bytes = 0, peak_working_set = 0;
};

class Sampler {
public:
    // The page is on screen: keep sampling for the next two seconds. Starts the thread the first time.
    void want() {
        wanted_until_.store(now_s() + 2.0, std::memory_order_relaxed);
        std::call_once(started_, [this] { std::thread([this] { run(); }).detach(); });
        cv_.notify_one();
    }
    Sample copy() {
        std::lock_guard<std::mutex> lk(mu_);
        return snap_;
    }

private:
    void run() {
        log_set_thread_name("debug-page");
        for (;;) {
            if (now_s() < wanted_until_.load(std::memory_order_relaxed)) {
                Sample s;
                s.have = true;
                s.at_s = now_s();
                s.gauges = gauge_read_all();
                s.shaders = nvn::shader_stats();
                if (nvn::backend_installed()) s.unsupported = nvn::backend_unsupported_report();
                PROCESS_MEMORY_COUNTERS_EX pm{};
                pm.cb = sizeof pm;
                if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm)) {
                    s.working_set = pm.WorkingSetSize;
                    s.private_bytes = pm.PrivateUsage;
                    s.peak_working_set = pm.PeakWorkingSetSize;
                }
                std::lock_guard<std::mutex> lk(mu_);
                snap_ = std::move(s);
            }
            std::unique_lock<std::mutex> lk(wait_mu_);
            cv_.wait_for(lk, std::chrono::seconds(1));
        }
    }
    std::once_flag started_;
    std::atomic<double> wanted_until_{0.0};
    std::mutex mu_, wait_mu_;
    std::condition_variable cv_;
    Sample snap_;
};

Sampler& sampler() {
    static Sampler* s = new Sampler;   // never destroyed: its thread is detached and may outlive static destruction
    return *s;
}

int64_t gauge_value(const Sample& s, const char* name, int64_t def = -1) {
    for (const auto& g : s.gauges)
        if (g.first == name) return g.second;
    return def;
}

// The state of the page (one per process; it is drawn on one thread).
struct PageState {
    std::vector<float> graph;
    double graph_at = -1;
    FrameTimeStats st5, st30;
    std::vector<LogTailLine> log;
    uint64_t log_seq = 0;
    double log_at = -1;
    int log_level = 2;                    // 0 errors, 1 warnings and errors, 2 info and above, 3 everything kept
    char log_filter[96] = {};
    bool log_follow = true;
    char gauge_filter[64] = {};
};
PageState& page() {
    static PageState p;
    return p;
}

void kv_row(const char* name, const char* fmt, ...) IM_FMTARGS(2);
void kv_row(const char* name, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(name);
    ImGui::TableSetColumnIndex(1);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}

bool begin_kv(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, px(270.0f));
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// ---- sections -----------------------------------------------------------------------------------------------------------------------------
void performance(const Overlay::Context& ctx, PageState& p) {
    const double t = now_s();
    if (t - p.graph_at >= 0.1) {      // ten refreshes a second are plenty for a graph of the last few hundred frames
        p.graph_at = t;
        p.st5 = frame_time_stats(5.0, &p.graph, 300);
        p.st30 = frame_time_stats(30.0);
    }
    const FrameTimeStats& s = p.st5;
    if (s.frames < 2) {
        ImGui::TextDisabled(s.total_frames ? "No frame presented in the last 5 s (the game is loading or paused)." : "No frame presented yet.");
    } else {
        ImGui::Text("%.1f fps   (1%% low %.1f fps, median %.2f ms, worst %.1f ms, last 5 s)", s.mean_fps, s.low1_fps, s.median_ms, s.worst_ms);
    }
    if (!p.graph.empty()) {
        float hi = 33.4f;
        for (float v : p.graph) hi = std::max(hi, v * 1.1f);
        hi = std::min(hi, 250.0f);
        char over[64];
        std::snprintf(over, sizeof over, "frame time, last %d frames (0..%.0f ms)", static_cast<int>(p.graph.size()), hi);
        ImGui::PlotLines("##frametimes", p.graph.data(), static_cast<int>(p.graph.size()), 0, over, 0.0f, hi, ImVec2(-FLT_MIN, px(90.0f)));
        // where 16.7 ms (60 fps) and 33.3 ms (30 fps) sit in the graph
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (float ms : {16.667f, 33.333f}) {
            if (ms >= hi) continue;
            const float y = b.y - (b.y - a.y) * (ms / hi);
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 50));
            char lbl[16];
            std::snprintf(lbl, sizeof lbl, "%.0f fps", 1000.0f / ms);
            dl->AddText(ImVec2(b.x - ImGui::CalcTextSize(lbl).x - px(4.0f), y - ImGui::GetFontSize()), IM_COL32(255, 255, 255, 110), lbl);
        }
    }
    if (begin_kv("perf_kv")) {
        if (ctx.renderer) {
            const RendererStats rs = ctx.renderer->stats();
            kv_row("GPU time of the output pass", "%.2f ms (average %.2f ms)", rs.gpu_ms_last, rs.gpu_ms_avg);
            const VkExtent2D e = ctx.renderer->extent();
            kv_row("Swapchain", "%u x %u, %u images, %u frames in flight, %s", e.width, e.height, ctx.renderer->image_count(), ctx.renderer->frames_in_flight(),
                   present_mode_name(ctx.renderer->present_mode()));
            kv_row("Frames", "%llu submitted, %llu presented, %llu skipped, %llu swapchains, %llu out of date", ull(rs.frames), ull(rs.presents), ull(rs.skipped),
                   ull(rs.swapchain_creates), ull(rs.out_of_date));
        }
        if (ctx.limiter) {
            const double cap = ctx.limiter->target_fps();
            if (cap > 0) kv_row("Frame limiter", "%.2f fps", cap);
            else kv_row("Frame limiter", "off");
        }
        kv_row("Presented since start", "%llu frames", ull(s.total_frames));
        ImGui::EndTable();
    }
}

void hitches(PageState& p) {
    uint64_t total = 0;
    double limit = 0;
    const std::vector<std::string> lines = hitch_recent(12, &total, &limit);
    const FrameTimeStats& s = p.st30;
    if (begin_kv("hitch_kv")) {
        if (limit > 0) kv_row("Slow frames since start", "%llu over %.0f ms ([diag] hitch_ms)", ull(total), limit);
        else kv_row("Slow frames since start", "not counted ([diag] hitch_ms = 0)");
        if (s.frames >= 2) {
            kv_row("Last 30 s", "%zu frames, %.1f fps, 1%% low %.1f fps, worst %.1f ms", s.frames, s.mean_fps, s.low1_fps, s.worst_ms);
            kv_row("Over twice the median", "%zu frames (median %.2f ms)", s.over2x, s.median_ms);
        }
        ImGui::EndTable();
    }
    if (lines.empty()) {
        ImGui::TextDisabled("No hitch line logged yet.");
        return;
    }
    ImGui::TextDisabled("The worst slow frame of each second, newest first (as in the log, tag 'hitch'):");
    if (ImGui::BeginChild("hitch_lines", ImVec2(-FLT_MIN, px(150.0f)), ImGuiChildFlags_Borders)) {
        for (size_t i = lines.size(); i-- > 0;) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(lines[i].c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
        }
    }
    ImGui::EndChild();
}

void gpu(const Overlay::Context& ctx, const Sample& smp) {
    if (begin_kv("gpu_kv")) {
        if (ctx.device) {
            const GpuInfo& g = ctx.device->gpu();
            kv_row("GPU", "%s", g.name.c_str());
            kv_row("Vendor / device", "0x%04x / 0x%04x, driver 0x%08x, Vulkan %u.%u", g.vendor_id, g.device_id, g.driver_version, VK_API_VERSION_MAJOR(g.api_version),
                   VK_API_VERSION_MINOR(g.api_version));
            kv_row("Video memory", "%.1f GiB device local", static_cast<double>(g.vram_bytes) / (1024.0 * 1024.0 * 1024.0));
            const ValidationStats vs = ctx.device->validation_stats();
            kv_row("Validation layer", "%s: %llu errors, %llu warnings", ctx.device->validation_enabled() ? "on" : "off", ull(vs.errors), ull(vs.warnings));
        } else {
            kv_row("GPU", "no device");
        }
        if (smp.have) {
            kv_row("Process memory", "working set %.2f GB (peak %.2f GB), private %.2f GB", smp.working_set / 1e9, smp.peak_working_set / 1e9, smp.private_bytes / 1e9);
        }
        ImGui::EndTable();
    }
    const int64_t used = gauge_value(smp, "vma.device_usage_mb"), budget = gauge_value(smp, "vma.device_budget_mb");
    if (used >= 0 && budget > 0) {
        char lbl[96];
        std::snprintf(lbl, sizeof lbl, "%lld of %lld MB in use (the driver's budget for this process)", static_cast<long long>(used), static_cast<long long>(budget));
        const float f = std::clamp(static_cast<float>(used) / static_cast<float>(budget), 0.0f, 1.0f);
        if (f > 0.9f) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kRed);
        ImGui::ProgressBar(f, ImVec2(-FLT_MIN, 0.0f), lbl);
        if (f > 0.9f) ImGui::PopStyleColor();
        const int64_t allocs = gauge_value(smp, "vma.alloc_count"), alloc_mb = gauge_value(smp, "vma.alloc_mb"), blocks = gauge_value(smp, "vma.block_mb");
        if (allocs >= 0) ImGui::TextDisabled("%lld allocations, %lld MB allocated in %lld MB of blocks", static_cast<long long>(allocs), static_cast<long long>(alloc_mb),
                                             static_cast<long long>(blocks));
    } else if (smp.have) {
        ImGui::TextDisabled("Video memory use: not known yet (the memory allocator reports it once the game draws).");
    } else {
        ImGui::TextDisabled("Reading ...");
    }
}

void shaders(const Sample& smp) {
    const nvn::BackendStats b = nvn::backend_stats();
    const nvn::ShaderStats& s = smp.shaders;
    if (!smp.have) {
        ImGui::TextDisabled("Reading ...");
        return;
    }
    if (begin_kv("shader_kv")) {
        kv_row("Shader preparation", "%s", s.prepare_status.c_str());
        if (s.prepared_programs) kv_row("Programs it covers", "%llu", ull(s.prepared_programs));
        kv_row("Waiting to be built", "%d (the 'Preparing shaders' note)", s.backlog);
        kv_row("Pipeline parts built", "%llu from the preparation (%.2f ms each), %llu new (%.1f ms each)", ull(s.parts_prepared),
               s.parts_prepared ? s.prepared_ms / static_cast<double>(s.parts_prepared) : 0.0, ull(s.parts_new),
               s.parts_new ? s.new_ms / static_cast<double>(s.parts_new) : 0.0);
        kv_row("Pipelines linked", "%llu (%.1f ms in all), %llu failed", ull(s.linked), s.link_ms, ull(s.link_failed));
        kv_row("Built at the draw", "%llu parts (%.1f ms), waited for %llu (%.1f ms)", ull(s.parts_at_draw), s.draw_part_ms, ull(s.parts_waited), s.wait_ms);
        kv_row("Draws deferred", "%llu (their pipeline was not ready)", ull(s.draws_deferred));
        kv_row("Background jobs", "%llu queued or running%s", ull(s.jobs_pending), s.pipelines_ahead ? "" : " (the background builder is off)");
        kv_row("Pipelines created", "%llu, cache hits %llu", ull(b.pipelines_created), ull(b.pipeline_cache_hits));
        ImGui::EndTable();
    }
}

void nvn_counters(const Sample& smp) {
    const nvn::BackendStats b = nvn::backend_stats();
    if (!nvn::backend_installed()) ImGui::TextDisabled("The NVN back end is not running.");
    if (begin_kv("nvn_kv")) {
        const uint64_t all = b.draws + b.draws_skipped;
        kv_row("Draws", "%llu drawn, %llu skipped (%.1f%%)", ull(b.draws), ull(b.draws_skipped), all ? 100.0 * static_cast<double>(b.draws_skipped) / static_cast<double>(all) : 0.0);
        kv_row("Dispatches, clears, copies", "%llu, %llu, %llu", ull(b.dispatches), ull(b.clears), ull(b.copies));
        kv_row("Submissions", "%llu (%llu command lists), %llu passes (%llu breaks)", ull(b.submits), ull(b.lists_replayed), ull(b.passes), ull(b.pass_breaks));
        kv_row("Textures", "%llu created, %llu uploads (%.0f MB), %llu BC7 transcodes, %llu failed", ull(b.textures_created), ull(b.texture_uploads),
               static_cast<double>(b.texture_upload_bytes) / (1024.0 * 1024.0), ull(b.bc7_transcodes), ull(b.textures_failed));
        kv_row("Memory pools imported", "%llu", ull(b.pools_imported));
        kv_row("Not supported", "%llu operations, %llu unresolved addresses, %llu missing handles", ull(b.unsupported_ops), ull(b.unresolved_addresses), ull(b.missing_handles));
        kv_row("NVN entry points", "%zu, %zu implemented; stub calls %llu (%zu distinct)", nvn::entry_count(), nvn::implemented_count(), ull(nvn::stub_calls_total()),
               nvn::stubs_called_distinct());
        ImGui::EndTable();
    }
    if (ImGui::TreeNode("Most called stubs")) {
        int shown = 0;
        for (const auto& c : nvn::called_entries()) {
            if (c.implemented) continue;
            ImGui::Text("%8llu  %s", ull(c.calls), c.entry->name);
            if (++shown >= 16) break;
        }
        if (!shown) ImGui::TextDisabled("none called");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Unsupported operations")) {
        if (smp.unsupported.empty()) ImGui::TextDisabled("none");
        else {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(smp.unsupported.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TreePop();
    }
}

bool contains_ci(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return true;
    const size_t n = std::strlen(needle);
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(hay[i + k])) == std::tolower(static_cast<unsigned char>(needle[k]))) ++k;
        if (k == n) return true;
    }
    return false;
}

void log_view(PageState& p) {
    const double t = now_s();
    if (t - p.log_at >= 0.25) {
        p.log_at = t;
        p.log_seq = log_tail(&p.log, kLogTailLines);
    }
    const char* path = log_path();
    ImGui::TextDisabled("%s, %llu lines written", path && *path ? path : "(no log file)", ull(p.log_seq));
    static const char* const kLevels[] = {"Errors", "Warnings and errors", "Info and above", "Everything"};
    ImGui::SetNextItemWidth(px(200.0f));
    ImGui::Combo("##log_level", &p.log_level, kLevels, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(220.0f));
    ImGui::InputTextWithHint("##log_filter", "filter (text)", p.log_filter, sizeof p.log_filter);
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &p.log_follow);
    ImGui::SameLine();
    const bool copy = ImGui::SmallButton("Copy");
    std::string copied;
    if (ImGui::BeginChild("log_lines", ImVec2(-FLT_MIN, px(260.0f)), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (const LogTailLine& l : p.log) {
            if (static_cast<int>(l.level) > p.log_level) continue;
            if (!contains_ci(l.text, p.log_filter)) continue;
            const bool col = l.level == LogLevel::Error || l.level == LogLevel::Warn || l.level >= LogLevel::Debug;
            if (col) ImGui::PushStyleColor(ImGuiCol_Text, l.level == LogLevel::Error ? kRed : l.level == LogLevel::Warn ? kOrange : kGrey);
            ImGui::TextUnformatted(l.text.c_str());
            if (col) ImGui::PopStyleColor();
            if (copy) copied += l.text + "\n";
        }
        if (p.log_follow && ImGui::GetScrollY() < ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    if (copy) ImGui::SetClipboardText(copied.c_str());
}

void gauges(PageState& p, const Sample& smp) {
    ImGui::SetNextItemWidth(px(260.0f));
    ImGui::InputTextWithHint("##gauge_filter", "filter (vk., nvn., vma. ...)", p.gauge_filter, sizeof p.gauge_filter);
    if (!smp.have) {
        ImGui::TextDisabled("Reading ...");
        return;
    }
    if (ImGui::BeginTable("gauges", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(-FLT_MIN, px(240.0f)))) {
        ImGui::TableSetupColumn("Gauge");
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, px(140.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& g : smp.gauges) {
            if (!contains_ci(g.first, p.gauge_filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(g.first.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%lld", static_cast<long long>(g.second));
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%zu gauges, read %.1f s ago. [diag] soak_csv writes them to a file every few seconds.", smp.gauges.size(), now_s() - smp.at_s);
}

}  // namespace

void draw_debug_page(const Overlay::Context& ctx) {
    sampler().want();
    const Sample smp = sampler().copy();
    PageState& p = page();
    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) performance(ctx, p);
    else if (now_s() - p.graph_at >= 0.1) {   // the hitch section reads the 30 s numbers too
        p.graph_at = now_s();
        p.st30 = frame_time_stats(30.0);
    }
    if (ImGui::CollapsingHeader("Hitches")) hitches(p);
    if (ImGui::CollapsingHeader("GPU and video memory", ImGuiTreeNodeFlags_DefaultOpen)) gpu(ctx, smp);
    if (ImGui::CollapsingHeader("Shaders and pipelines", ImGuiTreeNodeFlags_DefaultOpen)) shaders(smp);
    if (ImGui::CollapsingHeader("NVN back end")) nvn_counters(smp);
    if (ImGui::CollapsingHeader("Log")) log_view(p);
    if (ImGui::CollapsingHeader("Gauges")) gauges(p, smp);
}

}  // namespace nxrt::gfx
