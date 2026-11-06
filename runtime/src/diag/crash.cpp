// SPDX-License-Identifier: GPL-3.0-or-later
// Crash handler (declared in nxrt/diag.h): an unhandled-exception filter that writes the "HOST EXCEPTION" report, plus a
// vectored handler that catches stack overflow on a fresh helper thread. Guest traps (0xE0000001 / 0xE0000002) arrive at
// the unhandled filter like any other exception. The layout of the report follows crash_build_report() in diag.h.
// Idea from smo-recomp runtime/core/hostrt.c crash_filter; written here from scratch.
#include "nxrt/diag.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/gauge.h"
#include "nxrt/instance.h"
#include "nxrt/log.h"
#include "nxrt/symbols.h"
#include "stackwalk.h"

namespace nxrt {
namespace {

constexpr DWORD kTrapCode = 0xE0000001u;
constexpr DWORD kUntranslatedCode = 0xE0000002u;
constexpr DWORD kGuestBreakCode = 0xE0000003u;   // svcBreak / svcReturnFromException (svc_thread.cpp)
constexpr size_t kMaxHostFrames = 32;
constexpr int kMaxGuestFrames = 24;
constexpr DWORD kOverflowWaitMs = 30000;
constexpr DWORD kOtherReportWaitMs = 15000;   // another thread's report: a handler stuck longer than this is given up on

std::mutex g_install_mu;
bool g_installed = false;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
void* g_veh = nullptr;

// Options copied at install time and read by the handler.
bool g_path_given = false;  // false: default "<exe>_crash.txt" next to the exe; true with "" : no file
std::string g_path;
bool g_terminate = true;
uint32_t g_exit_code = 4;
void (*g_on_report)(const char*, void*) = nullptr;
void* g_user = nullptr;

std::string g_report_file;                      // resolved at install time: the handlers must not depend on more than they need

// The thread whose report is being built (0: none). Several guest threads often fault at once: the others wait for the first
// report (which terminates the process). The same thread again means the handler itself faulted.
std::atomic<DWORD> g_owner{0};
std::string g_partial;                          // the report the handler is building, written as it is when the handler faults
DWORD g_first_code = 0;                         // the exception the handler is reporting
uint64_t g_first_addr = 0;
std::atomic<bool> g_overflow_reported{false};   // the vectored handler already reported this stack overflow
std::atomic<uint32_t> g_reports{0};

// Stack overflow: the faulting thread has no stack left, so its record and context are copied into these statics and a
// helper thread builds the report from them.
EXCEPTION_RECORD g_ov_rec{};
CONTEXT g_ov_ctx{};
DWORD g_ov_tid = 0;

#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void appendf(std::string& out, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n >= 0 && static_cast<size_t>(n) < sizeof buf) {
        out.append(buf, static_cast<size_t>(n));
    } else if (n > 0) {
        std::string big(static_cast<size_t>(n) + 1, '\0');
        std::vsnprintf(big.data(), big.size(), fmt, ap2);
        out.append(big.data(), static_cast<size_t>(n));
    }
    va_end(ap);
    va_end(ap2);
}

// True when every byte of [addr, addr+n) is committed, readable and not a guard page. Never faults.
bool readable(uint64_t addr, uint64_t n) {
    if (n == 0) return true;
    uint64_t p = addr;
    const uint64_t end = addr + n;
    if (end < addr) return false;
    const DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(p)), &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & kReadable)) return false;
        const uint64_t next = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) + mbi.RegionSize;
        if (next <= p) return false;
        p = next;
    }
    return true;
}

// <exe path without extension>_crash.txt; a named instance: <instance folder>\<exe stem>_crash.txt (nxrt/instance.h)
std::string default_report_path() { return instance_report_path("_crash.txt"); }

bool write_file(const std::string& path, const std::string& text) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD done = 0;
    BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &done, nullptr);
    CloseHandle(h);
    return ok && done == text.size();
}

// Builds the whole report text for an exception raised on thread `tid` into out (cleared first).
void build_report(std::string& out, const EXCEPTION_RECORD* rec, const CONTEXT* cx, DWORD tid) {
    g_reports.fetch_add(1, std::memory_order_relaxed);
    out.clear();
    const uint64_t rip = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress));
    const std::vector<ThreadRecord> threads = diag_threads();

    // The faulting thread: its registered name and Ctx. The current thread uses its thread-local Ctx; another thread
    // (stack-overflow helper) uses the Ctx its registry record holds.
    std::string tname = "unregistered";
    bool named = false;
    ::Ctx* gctx = nullptr;
    for (const auto& t : threads) {
        if (t.host_tid != tid) continue;
        if (!named) {
            tname = t.name.empty() ? std::string("(unnamed)") : t.name;
            named = true;
        }
        if (!gctx && t.ctx) gctx = t.ctx;
    }
    if (tid == GetCurrentThreadId()) gctx = diag_thread_ctx();
    if (gctx && !readable(reinterpret_cast<uint64_t>(gctx), sizeof(::Ctx))) gctx = nullptr;

    // Header.
    const char* code_name = nullptr;
    if (rec->ExceptionCode == kTrapCode) code_name = "GUEST TRAP";
    else if (rec->ExceptionCode == kUntranslatedCode) code_name = "UNTRANSLATED INSTRUCTION";
    else if (rec->ExceptionCode == kGuestBreakCode) code_name = "GUEST BREAK (svcBreak)";
    if (code_name) {
        appendf(out, "HOST EXCEPTION 0x%08lX (%s) at host address 0x%llx on thread %s (tid %lu)\n",
                static_cast<unsigned long>(rec->ExceptionCode), code_name, static_cast<unsigned long long>(rip),
                tname.c_str(), static_cast<unsigned long>(tid));
    } else {
        appendf(out, "HOST EXCEPTION 0x%08lX at host address 0x%llx on thread %s (tid %lu)\n",
                static_cast<unsigned long>(rec->ExceptionCode), static_cast<unsigned long long>(rip), tname.c_str(),
                static_cast<unsigned long>(tid));
    }
    appendf(out, "  in %s\n", symbols_describe(rip).c_str());
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR kind = rec->ExceptionInformation[0];
        const char* what = kind == 0 ? "reading" : kind == 1 ? "writing" : kind == 8 ? "executing" : "accessing";
        appendf(out, "  %s address 0x%llx\n", what, static_cast<unsigned long long>(rec->ExceptionInformation[1]));
    }

    // Last guest function: the lifted function whose host code contains the faulting instruction.
    if (const uint64_t g = guest_function_of(rip))
        appendf(out, "last guest function entered: f_%llx\n", static_cast<unsigned long long>(g));
    else
        out += "last guest function entered: (unknown: fault address is not inside lifted code)\n";

    // Guest registers.
    if (!gctx) {
        out += "guest registers: (no guest context on this thread)\n";
    } else {
        out += "guest registers:\n";
        for (int n = 0; n <= 30; ++n) {
            out += (n % 3 == 0) ? (n == 0 ? "  " : "\n  ") : "   ";
            appendf(out, "x%-2d %016llx", n, static_cast<unsigned long long>(reg(gctx, n)));
        }
        out += "\n";
        appendf(out, "  sp %016llx   tpidr %016llx   tpidrro %016llx\n", static_cast<unsigned long long>(sp(gctx)),
                static_cast<unsigned long long>(tpidr_el0(gctx)), static_cast<unsigned long long>(tpidrro_el0(gctx)));
    }

    // Guest frame chain: lr of the Ctx, then the return address of every frame reachable through x29.
    if (!gctx) {
        out += "guest frame chain (x29): (none)\n";
    } else {
        appendf(out, "guest frame chain (x29): lr=0x%llx", static_cast<unsigned long long>(lr(gctx)));
        uint64_t f = fp(gctx);
        for (int i = 0; i < kMaxGuestFrames && f && readable(f, 16); ++i) {
            const uint64_t next = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f));
            const uint64_t ret = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f + 8));
            if (ret) appendf(out, " <- 0x%llx", static_cast<unsigned long long>(ret));
            if (next <= f) break;  // frames must go up the stack; anything else is a broken chain
            f = next;
        }
        out += "\n";
    }

    // Host call stack, symbolised.
    uint64_t frames[kMaxHostFrames] = {};
    const size_t nframes = detail::capture_stack(cx, frames, kMaxHostFrames);
    out += "host call stack (innermost first):\n";
    if (!nframes) out += "  (none)\n";
    for (size_t i = 0; i < nframes; ++i) appendf(out, "  %s\n", symbols_describe(frames[i]).c_str());

    // Guest call stack: the lifted functions among the host frames, consecutive repeats dropped.
    out += "guest call stack (innermost first):";
    uint64_t last = 0;
    bool any = false;
    for (size_t i = 0; i < nframes; ++i) {
        const uint64_t g = guest_function_of(frames[i]);
        if (!g || g == last) continue;
        appendf(out, " f_%llx", static_cast<unsigned long long>(g));
        last = g;
        any = true;
    }
    if (!any) out += " (none)";
    out += "\n";

    // Threads of the registry.
    out += "threads:\n";
    if (threads.empty()) out += "  (none)\n";
    for (const auto& t : threads) {
        char lrbuf[32] = "-";
        if (t.ctx && readable(reinterpret_cast<uint64_t>(t.ctx), sizeof(::Ctx)))
            std::snprintf(lrbuf, sizeof lrbuf, "0x%llx", static_cast<unsigned long long>(lr(t.ctx)));
        appendf(out, "  %s tid=%lu guest_tid=%llu lr=%s\n", t.name.empty() ? "(unnamed)" : t.name.c_str(),
                static_cast<unsigned long>(t.host_tid), static_cast<unsigned long long>(t.guest_tid), lrbuf);
    }

    // Recent files, oldest first.
    const std::vector<std::string> files = diag_recent_files();
    out += "last files opened (oldest first):\n";
    if (files.empty()) out += "  (none)\n";
    for (const auto& f : files) appendf(out, "  %s\n", f.c_str());

    // The last 30 s of frame times: what the game was doing before it went wrong.
    out += frame_times_summary(30.0);
}

// Log block, report file, stderr, then the on_report callback. Terminating is the caller's decision.
void emit(const std::string& text) {
    for (size_t pos = 0; pos < text.size();) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        NXLOG_E("crash", "%.*s", static_cast<int>(eol - pos), text.data() + pos);
        pos = eol + 1;
    }
    log_flush();
    if (!g_report_file.empty()) write_file(g_report_file, text);
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
    if (g_on_report) g_on_report(text.c_str(), g_user);
}

// Last resort, with no allocation: the report built so far plus one line on both exceptions, to the report file and stderr.
[[noreturn]] void emergency_report(const EXCEPTION_RECORD* rec, const char* why) {
    char line[320];
    const int n = std::snprintf(line, sizeof line,
                                "\nnxrt: %s (exception 0x%08lX at 0x%llx) while reporting HOST EXCEPTION 0x%08lX at host address "
                                "0x%llx; report above is cut short\n",
                                why, rec ? static_cast<unsigned long>(rec->ExceptionCode) : 0ul,
                                rec ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)) : 0ull,
                                static_cast<unsigned long>(g_first_code), static_cast<unsigned long long>(g_first_addr));
    const DWORD len = n > 0 ? static_cast<DWORD>(n < static_cast<int>(sizeof line) ? n : sizeof line - 1) : 0;
    DWORD done = 0;
    if (!g_report_file.empty()) {
        HANDLE h = CreateFileA(g_report_file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, g_partial.data(), static_cast<DWORD>(g_partial.size()), &done, nullptr);
            WriteFile(h, line, len, &done, nullptr);
            CloseHandle(h);
        }
    }
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) {
        WriteFile(err, g_partial.data(), static_cast<DWORD>(g_partial.size()), &done, nullptr);
        WriteFile(err, line, len, &done, nullptr);
    }
    TerminateProcess(GetCurrentProcess(), g_exit_code);
    for (;;) Sleep(1000);
}

// Makes this thread the one that reports. Returns false when it already is (the handler faulted). Another thread's report is
// waited for; one that is stuck (a lock held by a crashed thread) is given up on with an emergency report.
bool enter_handler(const EXCEPTION_RECORD* rec) {
    const DWORD me = GetCurrentThreadId();
    for (DWORD waited = 0;; waited += 10) {
        DWORD owner = 0;
        if (g_owner.compare_exchange_strong(owner, me)) break;
        if (owner == me) return false;
        if (waited >= kOtherReportWaitMs) emergency_report(rec, "crash handler of another thread did not finish");
        Sleep(10);
    }
    g_first_code = rec ? rec->ExceptionCode : 0;
    g_first_addr = rec ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)) : 0;
    g_partial.clear();
    return true;
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW &&
        g_overflow_reported.exchange(false))
        return EXCEPTION_CONTINUE_SEARCH;  // the vectored handler already wrote this report
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    if (!enter_handler(rec)) emergency_report(rec, "second exception inside the crash handler");
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        g_owner = 0;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    build_report(g_partial, ep->ExceptionRecord, ep->ContextRecord, GetCurrentThreadId());
    emit(g_partial);
    if (g_terminate) TerminateProcess(GetCurrentProcess(), g_exit_code);
    g_owner = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI overflow_thread(LPVOID) {
    build_report(g_partial, &g_ov_rec, &g_ov_ctx, g_ov_tid);
    emit(g_partial);
    if (g_terminate) TerminateProcess(GetCurrentProcess(), g_exit_code);
    return 0;
}

LONG WINAPI crash_vectored(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    if (!enter_handler(ep->ExceptionRecord)) emergency_report(ep->ExceptionRecord, "second exception inside the crash handler");
    g_ov_rec = *ep->ExceptionRecord;
    g_ov_ctx = *ep->ContextRecord;
    g_ov_tid = GetCurrentThreadId();
    HANDLE th = CreateThread(nullptr, 0, overflow_thread, nullptr, 0, nullptr);
    if (th) {
        WaitForSingleObject(th, kOverflowWaitMs);
        CloseHandle(th);
        g_overflow_reported = true;
    }
    g_owner = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void crash_install(const CrashOptions& opt) {
    std::lock_guard<std::mutex> lk(g_install_mu);
    g_path_given = opt.report_path != nullptr;
    g_path = opt.report_path ? opt.report_path : "";
    g_terminate = opt.terminate;
    g_exit_code = opt.exit_code;
    g_on_report = opt.on_report;
    g_user = opt.user;
    g_report_file = g_path_given ? g_path : default_report_path();
    if (g_installed) return;
    g_prev_filter = SetUnhandledExceptionFilter(crash_filter);
    ULONG guarantee = 64 * 1024;  // stack reserved for the handlers if the thread overflows
    SetThreadStackGuarantee(&guarantee);
    g_veh = AddVectoredExceptionHandler(0, crash_vectored);
    g_installed = true;
}

void crash_uninstall() {
    std::lock_guard<std::mutex> lk(g_install_mu);
    if (!g_installed) return;
    SetUnhandledExceptionFilter(g_prev_filter);
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
    g_installed = false;
}

std::string crash_build_report(void* exception_pointers) {
    auto* ep = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return "HOST EXCEPTION: no exception information\n";
    std::string out;
    build_report(out, ep->ExceptionRecord, ep->ContextRecord, GetCurrentThreadId());
    return out;
}

std::string crash_report_path() { return g_path_given ? g_path : default_report_path(); }

uint32_t crash_exceptions_reported() { return g_reports.load(std::memory_order_relaxed); }

}  // namespace nxrt
