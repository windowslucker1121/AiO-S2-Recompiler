// SPDX-License-Identifier: GPL-3.0-or-later
// Bug capture: a folder with what is needed to look at a moment of a run later (tools/ai `bugs`, `load_state(bug=...)`).
//   <[bug] dir>\<yyyymmdd-hhmmss>[-instance][-label]\        (the instance of a named run, nxrt/instance.h)
//     bug.json      what, when, which build (exe size, time, SHA-256, commit), process, instance, frame, save folder, files; written LAST
//                   ("complete": true), so a reader that sees it sees a finished bundle
//     note.txt      the note (the hotkey's prompt or the request file's text)
//     screen.png    the next presented frame (written by the renderer's worker; missing when no frame is presented)
//     log.txt       the tail of the log, copied after the screenshot (so a requested frame trace is in it)
//     config.txt    every config key read so far with its value and source, then the ini file that was loaded
//     env.txt       the NXRT_* and SDL_* environment variables of the process
//     threads.txt   the watchdog's thread dump (guest and host stacks of every thread)
//     save\         a copy of the COMMITTED save data ([fs] save_dir without work folders and journals)
//     reports\      <exe stem>_crash.txt / _freeze.txt / _fatal.txt of this run (next to the exe, or in the instance folder)
// Triggers: the play layer's hotkey ([bug] key, default F9; it asks for a note in a small window) and request files polled from start-up
// in the run's request folder (the exe's folder; a named instance's own folder): capture_bug (whichever process sees it first) or
// capture_bug.<pid> (this process only). A request file's text is
// the note; lines starting with '@' are options: @trace (trace the next NVN frame into the log), @name <label> (folder suffix),
// @no_threads (no thread dump). Game independent: nothing here knows the game.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt::bug {

struct Request {
    std::string note;          // note.txt (empty: none yet; add_note can add it later)
    std::string origin;        // "hotkey", "request", "test", ...
    std::string label;         // folder name suffix (letters, digits, '-', '_' kept; the rest becomes '-')
    bool trace_frame = false;  // trace the next presented NVN frame into the log (as [nvn] trace_frame)
    bool thread_dump = true;
};

// Where bundles go: [bug] dir (env NXRT_BUG_DIR); empty: <exe dir>\bugs. Development builds of the game exe set their checkout's work\bugs.
std::string root();
// Creates the bundle folder and asks for the screenshot at once; the rest is written on a worker thread. Returns the folder ("" when it
// could not be created). Logs "bug: capture <folder> (<origin>)" at once and "bug: bundle complete <folder>" when bug.json is written.
std::string capture(const Request& r);
// Writes (replaces) note.txt of a bundle and the note in bug.json when that is already written.
bool add_note(const std::string& dir, const std::string& note);
// True once bug.json of the bundle exists (tests and tools wait for it).
bool wait_complete(const std::string& dir, uint32_t timeout_ms);

// Graphics hooks, registered by the play layer: write the next presented frame as a PNG (true when asked), trace the next frame.
using ScreenshotFn = bool (*)(const char* png_path);
void set_screenshot_fn(ScreenshotFn fn);
void set_trace_fn(void (*fn)());
// The build the exe was made from (game_main: the commit compiled in and whether the checkout had uncommitted changes then).
void set_build_info(const char* commit, bool dirty);

// The request-file poller ([bug] requests, default on; [bug] poll_ms, default 250). Idempotent; stop_requests joins the thread.
void start_requests();
void stop_requests();
// The request file paths: <request folder>\capture_bug and <request folder>\capture_bug.<pid> (nxrt/instance.h instance_request_dir).
std::string request_path(bool own_process);
// Screenshot requests, polled by the same thread: <request folder>\screenshot[.<pid>] holding the path of a PNG; the next presented frame is
// written there (tools/ai `screenshot`: PrintWindow does not see the game's Vulkan swapchain, and a covered window has nothing on screen).
std::string screenshot_request_path(bool own_process);

}  // namespace nxrt::bug
