// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for bug capture bundles (nxrt/bugcapture.h): what a bundle holds, the committed-only save copy, the request files.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include "test_framework.h"

#include "nxrt/bugcapture.h"
#include "nxrt/config.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

NXTEST_GROUP(bugcapture)

namespace {

namespace stdfs = std::filesystem;
using namespace std::chrono;

struct Fixture {
    stdfs::path dir, bugs, save;
    Fixture() {
        dir = stdfs::temp_directory_path() / ("nxrt_bug_" + std::to_string(GetCurrentProcessId()));
        stdfs::remove_all(dir);
        bugs = dir / "bugs";
        save = dir / "save";
        // a committed save, a work folder of a running process and a journal: only the first is copied
        stdfs::create_directories(save / "account" / "0100");
        std::ofstream(save / "account" / "0100" / "slot.sav") << "slot data";
        std::ofstream(save / "account" / "0100.meta") << "size=1024\n";
        stdfs::create_directories(save / "account" / "0100.work.1234");
        std::ofstream(save / "account" / "0100.work.1234" / "slot.sav") << "uncommitted";
        std::ofstream(save / "account" / "0100.journal.1234") << "journal";
        nxrt::config_set("bug", "dir", bugs.string().c_str());
        nxrt::config_set("fs", "save_dir", save.string().c_str());
        nxrt::config_set("bug", "poll_ms", "20");
    }
    ~Fixture() {
        nxrt::bug::set_screenshot_fn(nullptr);
        nxrt::bug::set_trace_fn(nullptr);
        nxrt::config_clear();
        std::error_code ec;
        stdfs::remove_all(dir, ec);
    }
};

std::string read(const stdfs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

bool wait_for(const std::function<bool()>& pred, int ms) {
    const auto end = steady_clock::now() + milliseconds(ms);
    while (!pred()) {
        if (steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(milliseconds(10));
    }
    return true;
}

int g_traces = 0;
bool fake_shot(const char* path) {
    std::ofstream(path, std::ios::binary) << "PNG stand-in";   // the renderer writes the real one on a worker
    return true;
}
void fake_trace() { ++g_traces; }

}  // namespace

NXTEST(bundle_has_note_config_save_screenshot_and_build) {
    Fixture f;
    nxrt::bug::set_screenshot_fn(&fake_shot);
    nxrt::bug::set_trace_fn(&fake_trace);
    nxrt::bug::set_build_info("0123456789abcdef0123456789abcdef01234567", true);
    (void)nxrt::config_get_str("bug", "dir");   // read once, so the effective config lists it
    g_traces = 0;
    nxrt::bug::Request r;
    r.note = "Link walks through the wall";
    r.origin = "test";
    r.label = "wall clip!";
    r.trace_frame = true;
    r.thread_dump = false;   // the dump suspends every thread of the test process: covered by the watchdog group
    const std::string d = nxrt::bug::capture(r);
    CHECK_MSG(!d.empty(), "no bundle folder");
    CHECK_MSG(has(d, "-wall-clip"), "label not in the folder name");
    CHECK(stdfs::u8path(d).parent_path() == f.bugs);
    CHECK_EQ(g_traces, 1);
    CHECK_MSG(nxrt::bug::wait_complete(d, 15000), "bug.json not written");
    const stdfs::path b = stdfs::u8path(d);
    CHECK_EQ(read(b / "note.txt"), std::string("Link walks through the wall\n"));
    const std::string j = read(b / "bug.json");
    CHECK(has(j, "\"complete\": true"));
    CHECK(has(j, "\"origin\": \"test\""));
    CHECK(has(j, "\"commit\": \"0123456789abcdef0123456789abcdef01234567\", \"dirty\": true"));
    CHECK(has(j, "\"screenshot\": true"));
    CHECK(has(j, "\"trace_frame\": true"));
    CHECK(has(j, "\"save_files\": 2"));
    const size_t sha = j.find("\"exe_sha256\": \"");
    CHECK_MSG(sha != std::string::npos && j.find('"', sha + 15) == sha + 15 + 64, "exe_sha256 is not 64 hex digits");
    CHECK(stdfs::exists(b / "screen.png"));
    CHECK(!stdfs::exists(b / "threads.txt"));
    CHECK(has(read(b / "config.txt"), "[bug] dir = "));
    CHECK_EQ(read(b / "save" / "account" / "0100" / "slot.sav"), std::string("slot data"));
    CHECK(stdfs::exists(b / "save" / "account" / "0100.meta"));
    CHECK_MSG(!stdfs::exists(b / "save" / "account" / "0100.work.1234"), "a work folder was copied");
    CHECK_MSG(!stdfs::exists(b / "save" / "account" / "0100.journal.1234"), "a journal was copied");
    CHECK(nxrt::bug::add_note(d, "second thought"));
    CHECK_EQ(read(b / "note.txt"), std::string("second thought\n"));
    // the same second again: its own folder
    const std::string d2 = nxrt::bug::capture(r);
    CHECK(!d2.empty() && d2 != d);
    CHECK(nxrt::bug::wait_complete(d2, 15000));
}

NXTEST(request_file_is_taken_with_note_and_options) {
    Fixture f;
    nxrt::bug::stop_requests();    // picks up [bug] poll_ms of the fixture
    nxrt::bug::start_requests();
    const std::string req = nxrt::bug::request_path(true);
    CHECK(has(req, "capture_bug."));
    {
        std::ofstream o(req, std::ios::binary);
        o << "@name from-request\r\n@no_threads\r\nthe door does not open\r\nsecond line\r\n";
    }
    stdfs::path found;
    CHECK_MSG(wait_for([&] {
                  std::error_code ec;
                  for (const auto& e : stdfs::directory_iterator(f.bugs, ec))
                      if (e.path().filename().string().find("from-request") != std::string::npos && stdfs::exists(e.path() / "bug.json")) found = e.path();
                  return !found.empty();
              }, 15000),
              "no bundle for the request file");
    CHECK_MSG(!stdfs::exists(req), "the request file was not taken");
    CHECK_EQ(read(found / "note.txt"), std::string("the door does not open\nsecond line\n"));
    CHECK(has(read(found / "bug.json"), "\"origin\": \"request\""));
    CHECK(!stdfs::exists(found / "threads.txt"));
    nxrt::bug::stop_requests();
}
