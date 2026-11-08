// SPDX-License-Identifier: GPL-3.0-or-later
// Scene tests: validation over 300 frames (offscreen and with a window), screenshots against the reference image, the same scene on the integrated GPU, the overlay.
// Reference image: tests/data/gfx/test_scene_640x360_f60.png, made by the screenshot test with NXRT_GFX_UPDATE_REF=1 on the RTX 3080.
#include <SDL3/SDL.h>

#include "gfx_test_util.h"
#include "nxrt/extras.h"
NXTEST_GROUP(gfx_scene)

using namespace gfxtest;

NXTEST(validation_clean_over_300_frames_offscreen) {
    SceneRunDesc d = base_desc();
    d.frames = 300;
    const SceneRunResult r = run_scene(d);
    print_result("offscreen", r);
    REQUIRE(r.ok);
    CHECK_EQ(r.frames, 300ull);
    CHECK_EQ(r.validation.errors, 0ull);
    CHECK_EQ(r.validation.warnings, 0ull);
    CHECK_EQ(r.memory_end.allocations, 0ull);   // everything the scene made is gone before the device is
    std::printf("    GPU frame time (timestamp queries): last %.4f ms, average %.4f ms\n", r.renderer.gpu_ms_last, r.renderer.gpu_ms_avg);
    CHECK(r.renderer.gpu_ms_avg > 0.0);
    CHECK(r.renderer.gpu_ms_avg < 100.0);
}

NXTEST(environment_switches_force_headless_and_capture) {
    // The switches this test needs: no visible window for automated runs, a screenshot on request, both without touching code.
    _putenv_s("NXRT_GFX_HEADLESS", "1");
    _putenv_s("NXRT_GFX_SCREENSHOT", "env_shot.png");
    _putenv_s("NXRT_GFX_SCREENSHOT_FRAME", "12");
    SceneRunDesc d = base_desc();
    d.headless = false;          // the caller asked for a window ...
    d.frames = 30;
    const SceneRunResult r = run_scene(d);
    _putenv_s("NXRT_GFX_HEADLESS", "");
    _putenv_s("NXRT_GFX_SCREENSHOT", "");
    _putenv_s("NXRT_GFX_SCREENSHOT_FRAME", "");
    REQUIRE(r.ok);
    CHECK_EQ(r.renderer.presents, 0ull);        // ... but the environment made it offscreen: no swapchain, no present
    CHECK_EQ(r.renderer.screenshots, 1ull);
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    CHECK(load_png_rgba("env_shot.png", &px, &w, &h));
    CHECK_EQ(w, 640u);
    CHECK_EQ(h, 360u);
}

NXTEST(validation_clean_over_300_frames_with_a_window) {
    SceneRunDesc d = base_desc();
    d.headless = false;
    d.frames = 300;
    d.width = 800;
    d.height = 450;
    const SceneRunResult r = run_scene(d);
    print_result("window", r);
    REQUIRE(r.ok);
    CHECK_EQ(r.frames, 300ull);
    CHECK_EQ(r.validation.errors, 0ull);
    CHECK_EQ(r.validation.warnings, 0ull);
    CHECK_EQ(r.renderer.presents, 300ull);
    CHECK_EQ(r.memory_end.allocations, 0ull);
}

NXTEST(headless_screenshot_matches_the_reference) {
    SceneRunDesc d = base_desc();
    d.width = 640;
    d.height = 360;
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    SceneRunResult r;
    REQUIRE(capture_scene(d, &px, &w, &h, &r));
    CHECK_EQ(w, 640u);
    CHECK_EQ(h, 360u);
    CHECK_EQ(r.validation.errors, 0ull);
    if (const char* upd = std::getenv("NXRT_GFX_UPDATE_REF"); upd && *upd == '1') {
        CHECK(save_png_rgba(kRefPath, px.data(), w, h));
        std::printf("    reference image rewritten: %s\n", kRefPath.c_str());
        return;
    }
    std::vector<uint8_t> ref;
    uint32_t rw = 0, rh = 0;
    if (!load_png_rgba(kRefPath, &ref, &rw, &rh)) { CHECK_MSG(false, "reference image missing: run once with NXRT_GFX_UPDATE_REF=1"); return; }
    const ImageDiff df = compare_rgba(px.data(), w, h, ref.data(), rw, rh);
    std::printf("    vs reference: mean abs %.5f, max %d, fraction over 8: %.6f\n", df.mean_abs, df.max_abs, df.fraction_over_8);
    CHECK(df.same_size);
    CHECK(df.mean_abs <= kMeanAbsTolerance);
    CHECK(df.max_abs <= kMaxAbsTolerance);
    CHECK(df.fraction_over_8 <= kFractionOver8);
    // the picture is not blank: the clear colour and the texture are both there
    CHECK(px[(5 * 640 + 5) * 4 + 2] > px[(5 * 640 + 5) * 4 + 0]);   // top-left corner is the dark blue clear colour
    size_t bright = 0;
    for (size_t i = 0; i < px.size(); i += 4) bright += (px[i] > 200 || px[i + 1] > 200 || px[i + 2] > 200) ? 1 : 0;
    CHECK(bright > 5000);
}

NXTEST(windowed_screenshot_matches_the_same_reference) {
    // The swapchain path (B8G8R8A8 swapchain image, window size) gives the same picture as the offscreen path.
    // SDR output pinned: with [hdr] mode = auto and Windows HDR on, the swapchain is HDR10 (PQ) and the screenshot holds PQ-encoded
    // values (mean abs 26.5 against the SDR reference on the RTX 4080 machine with HDR on, 2026-11-08); HDR output has its own tests.
    const nxrt::extras::Settings saved = nxrt::extras::settings();   // read once per process: earlier window tests already did
    nxrt::extras::Settings sdr = saved;
    sdr.hdr.mode = nxrt::extras::HdrMode::Off;
    nxrt::extras::set_settings(sdr);
    SceneRunDesc d = base_desc();
    d.headless = false;
    d.width = 640;
    d.height = 360;
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    const bool captured = capture_scene(d, &px, &w, &h);
    nxrt::extras::set_settings(saved);
    REQUIRE(captured);
    std::vector<uint8_t> ref;
    uint32_t rw = 0, rh = 0;
    REQUIRE(load_png_rgba(kRefPath, &ref, &rw, &rh));
    if (w != rw || h != rh) {
        std::printf("    window client size %ux%u differs from the reference %ux%u (DPI scaling)\n", w, h, rw, rh);
        nxtest::skip("window pixel size differs from the reference size");
        return;
    }
    const ImageDiff df = compare_rgba(px.data(), w, h, ref.data(), rw, rh);
    std::printf("    windowed vs reference: mean abs %.5f, max %d\n", df.mean_abs, df.max_abs);
    CHECK(df.mean_abs <= kMeanAbsTolerance);
    CHECK(df.max_abs <= kMaxAbsTolerance);
}

NXTEST(same_scene_on_the_integrated_gpu) {
    SceneRunDesc d = base_desc();
    d.width = 640;
    d.height = 360;
    d.gpu = "integrated";
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    SceneRunResult r;
    const std::string tmp = "capture_igpu.png";
    d.screenshot_frame = 60;
    d.screenshot_path = tmp;
    d.frames = 100;
    r = run_scene(d);
    if (!r.ok && r.error.find("no usable") != std::string::npos) {
        nxtest::skip("no integrated GPU");
        return;
    }
    print_result("integrated GPU", r);
    REQUIRE(r.ok);
    if (r.gpu_type != 1) {
        nxtest::skip("override did not select an integrated GPU on this machine");
        return;
    }
    CHECK_EQ(r.validation.errors, 0ull);
    REQUIRE(load_png_rgba(tmp, &px, &w, &h));
    std::vector<uint8_t> ref;
    uint32_t rw = 0, rh = 0;
    REQUIRE(load_png_rgba(kRefPath, &ref, &rw, &rh));
    const ImageDiff df = compare_rgba(px.data(), w, h, ref.data(), rw, rh);
    std::printf("    UHD vs RTX reference: mean abs %.5f, max %d, fraction over 8: %.6f\n", df.mean_abs, df.max_abs, df.fraction_over_8);
    CHECK(df.same_size);
    CHECK(df.mean_abs <= kMeanAbsTolerance);
    CHECK(df.max_abs <= kMaxAbsTolerance);
}

NXTEST(overlay_shows_through_the_key_and_changes_the_picture) {
    SceneRunDesc d = base_desc();
    d.width = 640;
    d.height = 360;
    std::vector<uint8_t> plain, with_overlay;
    uint32_t w = 0, h = 0;
    REQUIRE(capture_scene(d, &plain, &w, &h));
    d.overlay = true;
    d.overlay_visible_from_frame = 10;
    SceneRunResult r;
    REQUIRE(capture_scene(d, &with_overlay, &w, &h, &r));
    CHECK_EQ(r.validation.errors, 0ull);
    CHECK_EQ(r.validation.warnings, 0ull);
    const ImageDiff df = compare_rgba(plain.data(), w, h, with_overlay.data(), w, h);
    std::printf("    overlay vs plain: mean abs %.4f, pixels over 8: %.4f\n", df.mean_abs, df.fraction_over_8);
    // With no panel added, the overlay is the Diagnostics window folded up at the top right (overlay.cpp build_ui): a 420x22 title bar on
    // a 640x360 frame is 4.0% of the picture. Half of that is the least a visible overlay must change.
    CHECK(df.fraction_over_8 > 0.02);

    // overlay created but never shown: the picture is the plain one
    d.overlay_visible_from_frame = -1;
    std::vector<uint8_t> hidden;
    REQUIRE(capture_scene(d, &hidden, &w, &h, &r));
    const ImageDiff dh = compare_rgba(plain.data(), w, h, hidden.data(), w, h);
    CHECK_EQ(dh.max_abs, 0);
}
