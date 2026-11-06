// SPDX-License-Identifier: GPL-3.0-or-later
// Bug capture bundles (nxrt/bugcapture.h): the folder, the worker that fills it, the request-file poller.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include "nxrt/bugcapture.h"

#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/services.h"

#pragma comment(lib, "bcrypt")

namespace nxrt::bug {
namespace {

namespace stdfs = std::filesystem;
constexpr const char* kTag = "bug";

struct Global {
    std::mutex mu;   // the hooks, the build info, folder creation
    ScreenshotFn shot = nullptr;
    void (*trace)() = nullptr;
    std::string commit;
    bool dirty = false;
    std::mutex sha_mu;
    std::string exe_sha;   // computed once (the running exe cannot change)
    // request poller
    std::mutex pmu;
    std::condition_variable cv;
    bool stop = false;
    std::thread th;
};
Global& g() {
    static Global* p = new Global;   // never destroyed: workers may still run while the process exits
    return *p;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string u8(const stdfs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::wstring exe_path_w() {
    std::wstring b(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
    b.resize(n);
    return b;
}

stdfs::path exe_dir() { return stdfs::path(exe_path_w()).parent_path(); }

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o + "\"";
}

bool write_file(const stdfs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

std::string read_file(const stdfs::path& p, uint64_t max_tail = UINT64_MAX) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(f.tellg());
    const uint64_t from = size > max_tail ? size - max_tail : 0;
    f.seekg(static_cast<std::streamoff>(from));
    std::string s(static_cast<size_t>(size - from), '\0');
    f.read(s.data(), static_cast<std::streamsize>(s.size()));
    if (from) {   // a cut read starts mid-line: drop the partial first line
        const size_t nl = s.find('\n');
        s.erase(0, nl == std::string::npos ? 0 : nl + 1);
    }
    return s;
}

std::string local_time(const SYSTEMTIME& t) {
    char b[40];
    std::snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::string file_time(const stdfs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return {};
    FILETIME lt;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&a.ftLastWriteTime, &lt);
    FileTimeToSystemTime(&lt, &st);
    return local_time(st);
}

std::string sha256_file(const stdfs::path& p) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::vector<uint8_t> buf(4u << 20);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got) ok = BCryptHashData(h, buf.data(), got, 0) >= 0;
        uint8_t d[32];
        if (ok && BCryptFinishHash(h, d, sizeof d, 0) >= 0) {
            char hex[65];
            for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
            out = hex;
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

std::string exe_sha256() {
    std::lock_guard<std::mutex> lk(g().sha_mu);
    if (g().exe_sha.empty()) g().exe_sha = sha256_file(exe_path_w());
    return g().exe_sha;
}

std::string sanitize(const std::string& s) {
    std::string o;
    for (char c : s) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (keep) o += c;
        else if (!o.empty() && o.back() != '-') o += '-';
        if (o.size() >= 40) break;
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o;
}

std::string environment_text() {
    std::string out;
    wchar_t* env = GetEnvironmentStringsW();
    if (!env) return out;
    for (const wchar_t* e = env; *e; e += wcslen(e) + 1) {
        const std::string line = narrow(e);
        // only the runtime's and SDL's switches: the rest of a user's environment can hold anything (tokens, paths)
        if (line.rfind("NXRT_", 0) == 0 || line.rfind("SDL_", 0) == 0) out += line + "\n";
    }
    FreeEnvironmentStringsW(env);
    return out;
}

std::string env_value(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// The committed state of the save data: every folder and file under save_dir except the journal's work folders (<key>.work.<pid>) and
// journals (<key>.journal.<pid>). While a commit is being applied (<key>.commit present) the committed folder is changing: wait for it.
size_t copy_save(const stdfs::path& from, const stdfs::path& to, std::string* why) {
    std::error_code ec;
    if (!stdfs::is_directory(from, ec)) {
        *why = "no save folder";
        return 0;
    }
    auto busy = [&] {
        std::error_code e2;
        for (stdfs::recursive_directory_iterator it(from, e2), end; !e2 && it != end; it.increment(e2))
            if (it->path().extension() == ".commit") return true;
        return false;
    };
    for (int i = 0; i < 40 && busy(); ++i) Sleep(50);
    size_t files = 0;
    for (stdfs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = u8(it->path().filename());
        if (name.find(".work.") != std::string::npos || name.find(".journal.") != std::string::npos || it->path().extension() == ".commit") {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        const stdfs::path dst = to / it->path().lexically_relative(from);
        std::error_code e2;
        if (it->is_directory(e2)) {
            stdfs::create_directories(dst, e2);
        } else {
            stdfs::create_directories(dst.parent_path(), e2);
            stdfs::copy_file(it->path(), dst, stdfs::copy_options::overwrite_existing, e2);
            if (!e2) ++files;
        }
    }
    return files;
}

struct Moment {
    std::string time, origin;
    uint64_t frame = 0;
    double uptime_s = 0;
    bool shot = false, trace = false, threads = true;
};

double process_uptime_s() {
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    const auto v = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(v(now) - v(c)) / 1e7;
}

void fill(const stdfs::path dir, const Moment m) {
    std::vector<std::string> files;
    if (stdfs::exists(dir / "note.txt")) files.push_back("note.txt");
    if (m.threads) {
        write_file(dir / "threads.txt", diag_thread_dump());
        files.push_back("threads.txt");
    }
    {
        std::string text = config_effective_text();
        const std::string ini = config_file_path();
        text += "\n; ---- ini file: " + (ini.empty() ? std::string("(none)") : ini) + "\n";
        if (!ini.empty()) text += read_file(stdfs::u8path(ini), 1u << 20);
        write_file(dir / "config.txt", text);
        write_file(dir / "env.txt", environment_text());
        files.push_back("config.txt");
        files.push_back("env.txt");
    }
    const std::string save = services::save_dir();
    std::string save_why;
    const size_t save_files = copy_save(stdfs::u8path(save), dir / "save", &save_why);
    if (stdfs::exists(dir / "save")) files.push_back("save/");
    const stdfs::path exe = exe_path_w();
    {
        std::string stem = u8(exe.stem());
        std::error_code ec;
        for (const char* k : {"crash", "freeze", "fatal"}) {
            const stdfs::path rep = exe.parent_path() / stdfs::u8path(stem + "_" + k + ".txt");
            if (!stdfs::exists(rep, ec)) continue;
            stdfs::create_directories(dir / "reports", ec);
            stdfs::copy_file(rep, dir / "reports" / rep.filename(), stdfs::copy_options::overwrite_existing, ec);
            files.push_back("reports/" + u8(rep.filename()));
        }
    }
    // the renderer writes the screenshot on a worker after the next presented frame (a PNG encode of a few hundred ms)
    bool shot_ok = false;
    if (m.shot) {
        std::error_code ec;
        uintmax_t last = 0;
        for (int i = 0; i < 100; ++i) {   // up to 10 s
            const uintmax_t sz = stdfs::exists(dir / "screen.png", ec) ? stdfs::file_size(dir / "screen.png", ec) : 0;
            if (sz && sz == last) {
                shot_ok = true;
                break;
            }
            last = sz;
            Sleep(100);
        }
        if (shot_ok) files.push_back("screen.png");
    }
    log_flush();
    const std::string logp = log_path();
    if (!logp.empty()) {
        write_file(dir / "log.txt", read_file(stdfs::u8path(logp), 32u << 20));
        files.push_back("log.txt");
    }
    const game::Info& gi = game::info();
    std::string commit;
    bool dirty;
    {
        std::lock_guard<std::mutex> lk(g().mu);
        commit = g().commit;
        dirty = g().dirty;
    }
    std::error_code ec;
    const uintmax_t exe_size = stdfs::file_size(exe, ec);
    char pid[24], prog[24];
    std::snprintf(pid, sizeof pid, "%lu", GetCurrentProcessId());
    std::snprintf(prog, sizeof prog, "0x%016llx", static_cast<unsigned long long>(services::program_id_now()));
    std::wstring cwd(32768, L'\0');
    cwd.resize(GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data()));
    std::ostringstream j;
    j << "{\n \"format\": 1,\n \"complete\": true,\n \"time\": " << json_str(m.time) << ",\n \"origin\": " << json_str(m.origin)
      << ",\n \"game\": {\"id\": " << json_str(gi.id) << ", \"title\": " << json_str(gi.title) << ", \"version\": " << json_str(gi.version)
      << ", \"program_id\": " << json_str(prog) << "},\n \"build\": {\"exe\": " << json_str(u8(exe)) << ", \"exe_size\": " << exe_size
      << ", \"exe_modified\": " << json_str(file_time(exe)) << ", \"exe_sha256\": " << json_str(exe_sha256()) << ", \"commit\": " << json_str(commit)
      << ", \"dirty\": " << (dirty ? "true" : "false") << "},\n \"process\": {\"pid\": " << pid << ", \"command_line\": " << json_str(narrow(GetCommandLineW()))
      << ", \"cwd\": " << json_str(narrow(cwd)) << ", \"uptime_s\": " << m.uptime_s << ", \"frame\": " << m.frame << "},\n \"state\": "
      << json_str(env_value("NXRT_AI_STATE")) << ",\n \"save_dir\": " << json_str(save) << ",\n \"save_files\": " << save_files
      << (save_why.empty() ? "" : ",\n \"save_missing\": " + json_str(save_why)) << ",\n \"log\": " << json_str(logp)
      << ",\n \"screenshot\": " << (shot_ok ? "true" : "false") << ",\n \"trace_frame\": " << (m.trace ? "true" : "false") << ",\n \"files\": [";
    for (size_t i = 0; i < files.size(); ++i) j << (i ? ", " : "") << json_str(files[i]);
    j << "]\n}\n";
    // bug.json last, through a rename: a reader never sees half of it
    write_file(dir / "bug.json.tmp", j.str());
    stdfs::rename(dir / "bug.json.tmp", dir / "bug.json", ec);
    NXLOG_I(kTag, "bundle complete %s (%zu save files%s)", u8(dir).c_str(), save_files, shot_ok ? ", screenshot" : ", no screenshot");
}

void poll_loop() {
    log_set_thread_name("bug-requests");
    const uint32_t poll_ms = static_cast<uint32_t>(config_get_int("bug", "poll_ms", 250));
    const stdfs::path own = stdfs::u8path(request_path(true)), any = stdfs::u8path(request_path(false));
    const stdfs::path taken = exe_dir() / ("capture_bug.taken." + std::to_string(GetCurrentProcessId()));
    const stdfs::path shot_own = stdfs::u8path(screenshot_request_path(true)), shot_any = stdfs::u8path(screenshot_request_path(false));
    const stdfs::path shot_taken = exe_dir() / ("screenshot.taken." + std::to_string(GetCurrentProcessId()));
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g().pmu);
            if (g().cv.wait_for(lk, std::chrono::milliseconds(poll_ms ? poll_ms : 250), [] { return g().stop; })) return;
        }
        // a screenshot request: the file names the PNG to write (tools/ai screenshot: PrintWindow does not see a Vulkan swapchain)
        for (const stdfs::path* p : {&shot_own, &shot_any}) {
            if (!MoveFileExW(p->c_str(), shot_taken.c_str(), MOVEFILE_REPLACE_EXISTING)) continue;
            Sleep(20);
            std::string target = read_file(shot_taken);
            DeleteFileW(shot_taken.c_str());
            while (!target.empty() && (target.back() == '\n' || target.back() == '\r' || target.back() == ' ')) target.pop_back();
            ScreenshotFn fn;
            {
                std::lock_guard<std::mutex> lk(g().mu);
                fn = g().shot;
            }
            if (target.empty() || !fn || !fn(target.c_str())) NXLOG_W(kTag, "screenshot request '%s': no presented frame to write", target.c_str());
            break;
        }
        // whichever process renames the shared file first takes it; the own file belongs to this process
        bool got = false;
        for (const stdfs::path* p : {&own, &any})
            if (!got && MoveFileExW(p->c_str(), taken.c_str(), MOVEFILE_REPLACE_EXISTING)) got = true;
        if (!got) continue;
        Sleep(20);   // the writer may still be closing it
        const std::string text = read_file(taken);
        DeleteFileW(taken.c_str());
        Request r;
        r.origin = "request";
        std::istringstream in(text);
        std::string line, note;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("@trace", 0) == 0) r.trace_frame = true;
            else if (line.rfind("@no_threads", 0) == 0) r.thread_dump = false;
            else if (line.rfind("@name ", 0) == 0) r.label = line.substr(6);
            else if (line.rfind("@origin ", 0) == 0) r.origin = line.substr(8);
            else note += line + "\n";
        }
        while (!note.empty() && (note.back() == '\n' || note.back() == ' ')) note.pop_back();
        r.note = note;
        capture(r);
    }
}

}  // namespace

std::string root() {
    const std::string d = config_get_str("bug", "dir", "");
    return d.empty() ? u8(exe_dir() / "bugs") : d;
}

std::string request_path(bool own_process) {
    std::string p = u8(exe_dir() / "capture_bug");
    if (own_process) p += "." + std::to_string(GetCurrentProcessId());
    return p;
}

std::string screenshot_request_path(bool own_process) {
    std::string p = u8(exe_dir() / "screenshot");
    if (own_process) p += "." + std::to_string(GetCurrentProcessId());
    return p;
}

std::string capture(const Request& r) {
    const stdfs::path base = stdfs::u8path(root());
    std::error_code ec;
    stdfs::create_directories(base, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::string name = stamp;
    if (const std::string l = sanitize(r.label); !l.empty()) name += "-" + l;
    stdfs::path dir;
    ScreenshotFn shot;
    void (*trace)();
    {
        std::lock_guard<std::mutex> lk(g().mu);
        for (int i = 1;; ++i) {
            dir = base / stdfs::u8path(i == 1 ? name : name + "-" + std::to_string(i));
            if (stdfs::create_directory(dir, ec)) break;
            if (ec || i > 99) {
                NXLOG_E(kTag, "capture: cannot create a folder under %s", u8(base).c_str());
                return {};
            }
        }
        shot = g().shot;
        trace = g().trace;
    }
    Moment m;
    m.time = local_time(t);
    m.origin = r.origin.empty() ? "api" : r.origin;
    m.frame = diag_frame_count();
    m.uptime_s = process_uptime_s();
    m.threads = r.thread_dump;
    NXLOG_I(kTag, "capture %s (%s)", u8(dir).c_str(), m.origin.c_str());
    if (!r.note.empty()) write_file(dir / "note.txt", r.note + "\n");
    if (r.trace_frame && trace) {
        trace();
        m.trace = true;
    }
    if (shot) m.shot = shot(u8(dir / "screen.png").c_str());
    std::thread(fill, dir, m).detach();
    return u8(dir);
}

bool add_note(const std::string& dir, const std::string& note) { return write_file(stdfs::u8path(dir) / "note.txt", note + "\n"); }

bool wait_complete(const std::string& dir, uint32_t timeout_ms) {
    const stdfs::path p = stdfs::u8path(dir) / "bug.json";
    for (uint32_t waited = 0;; waited += 50) {
        std::error_code ec;
        if (stdfs::exists(p, ec)) return true;
        if (waited >= timeout_ms) return false;
        Sleep(50);
    }
}

void set_screenshot_fn(ScreenshotFn fn) {
    std::lock_guard<std::mutex> lk(g().mu);
    g().shot = fn;
}

void set_trace_fn(void (*fn)()) {
    std::lock_guard<std::mutex> lk(g().mu);
    g().trace = fn;
}

void set_build_info(const char* commit, bool dirty) {
    std::lock_guard<std::mutex> lk(g().mu);
    g().commit = commit ? commit : "";
    g().dirty = dirty;
}

void start_requests() {
    if (!config_get_bool("bug", "requests", true)) return;
    std::lock_guard<std::mutex> lk(g().pmu);
    if (g().th.joinable()) return;
    g().stop = false;
    g().th = std::thread(poll_loop);
}

void stop_requests() {
    {
        std::lock_guard<std::mutex> lk(g().pmu);
        if (!g().th.joinable()) return;
        g().stop = true;
    }
    g().cv.notify_all();
    g().th.join();
}

}  // namespace nxrt::bug
