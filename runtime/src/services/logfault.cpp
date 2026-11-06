// SPDX-License-Identifier: GPL-3.0-or-later
// Log and fatal services (S3c): lm (ILogService / ILogger), fatal:u (fatal service) and fatal:p (private fatal service).
// Command ids and layouts: https://switchbrew.org/wiki/Log_services (LogPacketHeader, LogPacketFlags, LogDataChunkKey, ILogger),
// https://switchbrew.org/wiki/Fatal_services (fatal:u, fatal:p, the 0x250-byte CPU context offsets) and the service list of
// https://switchbrew.org/wiki/Services_API. err:u is not in that list (see docs/S3c.md); it is not registered here.
// Adapted from Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors): src/Ryujinx.Horizon/LogManager/Ipc/LmLogger.cs (the
// message assembly and the TLV reading, with the size-driven numeric fields and the 5000-character cap), src/Ryujinx.Horizon/
// LogManager/Types/LogPacket.cs (the line fields) and src/Ryujinx.HLE/HOS/Services/Fatal/IService.cs (the report layout and the
// register names of the CPU context). Written for this runtime; the behaviour that differs from Ryujinx is noted at each place.
//
// lm: OpenLogger(pid) returns an ILogger. Log() takes one LogPacket (a 0x18-byte header, then TLV chunks: LEB128 type, LEB128 size,
// data). A packet with the Head flag starts a message, the Tail flag completes it; text chunks are concatenated. Each completed
// message becomes one line in the runtime log, tag "guestlog", level from the severity.
// fatal: ThrowFatal / ThrowFatalWithPolicy / ThrowFatalWithCpuContext write a crash report (<exe>_fatal.txt next to the exe, or
// [fatal] report_path), log it at Error, signal the fatal event of fatal:p and terminate the process with exit code 5 unless
// [fatal] terminate is false.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/instance.h"
#include "nxrt/ipc.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

// ---- LogPacketHeader (Switchbrew Log services) -----------------------------------------------------------------------------------
constexpr size_t kHeaderSize = 0x18;          // 0x0 pid u64, 0x8 tid u64, 0x10 flags u8, 0x11 pad, 0x12 severity u8, 0x13 verbosity, 0x14 payload u32
constexpr uint8_t kFlagHead = 0x1;            // LogPacketFlags::Head
constexpr uint8_t kFlagTail = 0x2;            // LogPacketFlags::Tail
constexpr size_t kMessageLimit = 5000;        // Ryujinx LmLogger MessageLengthLimit: a longer message is cut and completed
constexpr size_t kMaxBuffer = 1u << 20;       // a Log buffer above 1 MiB is refused (no legal packet is that large)

// LogDataChunkKey (Switchbrew Log services). Start/Stop and the session/time fields are read and skipped.
enum : uint64_t {
    kChunkSessionBegin = 0,
    kChunkSessionEnd = 1,
    kChunkTextLog = 2,
    kChunkLineNumber = 3,
    kChunkFileName = 4,
    kChunkFunctionName = 5,
    kChunkModuleName = 6,
    kChunkThreadName = 7,
    kChunkPacketDropCount = 8,
    kChunkUserSystemClock = 9,
    kChunkProcessName = 10,
};

// LEB128 unsigned. False when the data ends inside the number or the number is longer than 64 bits.
bool read_uleb(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    out = 0;
    unsigned shift = 0;
    for (;;) {
        if (p >= end || shift >= 64) return false;
        const uint8_t b = *p++;
        out |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
    }
}

// A numeric chunk: little-endian, as many bytes as the chunk has (the first 8 count).
uint64_t read_le(const uint8_t* d, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 8; ++i) v |= static_cast<uint64_t>(d[i]) << (8 * i);
    return v;
}

// Trailing blanks and NULs removed, control characters made printable (a line stays one line).
std::string clean(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (c == '\n') s += "\\n";
        else if (c == '\r') s += "\\r";
        else if (static_cast<unsigned char>(c) < 0x20 && c != '\t') s += '.';
        else s += c;
    }
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\0')) --e;
    s.resize(e);
    return s;
}

// severity (nn::diag::LogSeverity: Trace 0, Info 1, Warn 2, Error 3, Fatal 4) -> runtime log level. Fatal and unknown values log as Error.
LogLevel level_of(uint8_t severity) {
    switch (severity) {
        case 0: return LogLevel::Trace;
        case 1: return LogLevel::Info;
        case 2: return LogLevel::Warn;
        default: return LogLevel::Error;
    }
}

// One message while its packets arrive.
struct Message {
    bool open = false;
    uint8_t severity = 1;
    std::string text, file, function, module, thread;
    bool has_line = false;
    uint64_t line = 0;
    uint64_t drop_count = 0;
};

// Writes one line for a completed message: "[module] file:line function: text" with the fields that are present, then
// " (thread T, N packets dropped)" when those are present. A message with no text and no fields writes nothing.
void emit(const Message& m) {
    const std::string text = clean(m.text);
    std::string s;
    if (!m.module.empty()) s += "[" + clean(m.module) + "] ";
    std::string loc = clean(m.file);
    if (m.has_line) {
        const std::string n = std::to_string(m.line);
        if (loc.empty()) loc = "line " + n;
        else loc += ":" + n;
    }
    if (!loc.empty()) {
        s += loc;
        if (!m.function.empty()) s += " " + clean(m.function);
        s += ": ";
    } else if (!m.function.empty()) {
        s += clean(m.function) + ": ";
    }
    s += text;
    std::string extra;
    if (!m.thread.empty()) extra += "thread " + clean(m.thread);
    if (m.drop_count) {
        if (!extra.empty()) extra += ", ";
        extra += std::to_string(m.drop_count) + " packets dropped";
    }
    if (!extra.empty()) s += " (" + extra + ")";
    if (s.empty()) {
        NXLOG_D("guestlog", "empty message from the guest, nothing written");
        return;
    }
    logf(level_of(m.severity), "guestlog", "%s", s.c_str());
}

// Reads the TLV chunks of one packet into the open message. A malformed chunk stops the packet (what was read stays).
void parse_chunks(Message& m, const uint8_t* p, const uint8_t* end) {
    while (p < end) {
        uint64_t type = 0, size = 0;
        if (!read_uleb(p, end, type) || !read_uleb(p, end, size) || size > static_cast<uint64_t>(end - p)) {
            NXLOG_W("guestlog", "malformed log chunk (%zu bytes left in the packet), rest of the packet ignored",
                    static_cast<size_t>(end - p));
            return;
        }
        const uint8_t* d = p;
        const size_t n = static_cast<size_t>(size);
        p += n;
        switch (type) {
            case kChunkTextLog: m.text.append(reinterpret_cast<const char*>(d), n); break;
            case kChunkLineNumber: m.line = read_le(d, n); m.has_line = true; break;
            case kChunkFileName: m.file.assign(reinterpret_cast<const char*>(d), n); break;
            case kChunkFunctionName: m.function.assign(reinterpret_cast<const char*>(d), n); break;
            case kChunkModuleName: m.module.assign(reinterpret_cast<const char*>(d), n); break;
            case kChunkThreadName: m.thread.assign(reinterpret_cast<const char*>(d), n); break;
            case kChunkPacketDropCount: m.drop_count = read_le(d, n); break;
            // SessionBegin / SessionEnd carry no text; UserSystemClock and ProcessName are read and not printed; unknown keys skipped.
            case kChunkSessionBegin:
            case kChunkSessionEnd:
            case kChunkUserSystemClock:
            case kChunkProcessName:
            default: break;
        }
    }
}

// One ILogger's message assembly. Shared by the command handlers of that logger session.
struct LoggerState {
    std::mutex mu;
    Message msg;

    void accept(const uint8_t* p, size_t n) {
        if (n < kHeaderSize) {
            NXLOG_W("guestlog", "log packet of %zu bytes is shorter than its 0x18-byte header, dropped", n);
            return;
        }
        uint32_t payload = 0;
        std::memcpy(&payload, p + 0x14, 4);
        if (n != kHeaderSize + static_cast<size_t>(payload)) {
            NXLOG_W("guestlog", "log packet size mismatch (buffer 0x%zx, header says 0x%zx), dropped", n,
                    kHeaderSize + static_cast<size_t>(payload));
            return;
        }
        const uint8_t flags = p[0x10];
        const uint8_t severity = p[0x12];
        const bool head = (flags & kFlagHead) != 0;
        bool tail = (flags & kFlagTail) != 0;
        if (head) {
            if (msg.open) NXLOG_W("guestlog", "a new message started before the previous one was completed; %zu bytes dropped",
                                  msg.text.size());
            msg = Message{};
            msg.open = true;
            msg.severity = severity;
        } else if (!msg.open) {
            NXLOG_W("guestlog", "continuation packet without a head packet; it starts a message");
            msg = Message{};
            msg.open = true;
            msg.severity = severity;
        }
        parse_chunks(msg, p + kHeaderSize, p + n);
        if (msg.text.size() >= kMessageLimit) {
            msg.text.resize(kMessageLimit);
            tail = true;
        }
        if (tail) {
            emit(msg);
            msg = Message{};
        }
    }
};

// ---- process-wide state ----------------------------------------------------------------------------------------------------------
struct State {
    std::mutex mu;
    uint32_t destination = 1;      // ILogger SetDestination mask (TMA); stored as a global value, nothing else reads it
    ServiceEvent fatal_event;      // fatal:p GetFatalEvent; ThrowFatal* signals it
};
State& state() {
    static State s;
    return s;
}

// ---- lm ----------------------------------------------------------------------------------------------------------------------------
uint32_t log_cmd(const std::shared_ptr<LoggerState>& st, ipc::Request& req) {
    const ipc::Buffer b = req.in_buf(0);
    if (!b.addr || b.size == 0) {
        NXLOG_W("guestlog", "Log: no message buffer");
        return 0;
    }
    if (b.size > kMaxBuffer) {
        NXLOG_W("guestlog", "Log: message buffer of 0x%llx bytes refused", static_cast<unsigned long long>(b.size));
        return 0;
    }
    std::lock_guard<std::mutex> lk(st->mu);
    st->accept(buf_ptr(b), static_cast<size_t>(b.size));
    return 0;   // Switchbrew: Log always returns success
}

std::shared_ptr<ipc::Service> make_logger() {
    auto st = std::make_shared<LoggerState>();
    return make_iface("lm:ILogger", [st](ipc::Service& s) {
        s.add_cmd(0, "Log", [st](ipc::Request& req, ipc::Response&) -> uint32_t { return log_cmd(st, req); });
        s.add_cmd(1, "SetDestination", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint32_t mask = req.arg<uint32_t>(0);
            State& g = state();
            std::lock_guard<std::mutex> lk(g.mu);
            g.destination = mask;
            NXLOG_D("lm", "SetDestination 0x%x", mask);
            return 0;
        });
    });
}

// ---- fatal -----------------------------------------------------------------------------------------------------------------------
constexpr size_t kCpuContextSize = 0x250;     // the CPU context buffer of ThrowFatalWithCpuContext (Switchbrew Fatal services)

const char* policy_name(uint32_t policy) {
    switch (policy) {
        case 0: return "ErrorReportAndErrorScreen";
        case 1: return "ErrorReport";
        case 2: return "ErrorScreen";
        default: return "unknown";
    }
}

uint64_t rd64(const uint8_t* p, size_t off) {
    uint64_t v = 0;
    std::memcpy(&v, p + off, 8);
    return v;
}

// <exe path without extension>_fatal.txt, the way the crash handler names its file (a named instance: in its folder, nxrt/instance.h).
std::string default_report_path() { return instance_report_path("_fatal.txt"); }

// [fatal] report_path when set (empty = no file), else the default.
std::string report_path() {
    if (config_has("fatal", "report_path")) return config_get_str("fatal", "report_path");
    return default_report_path();
}

// Horizon result: module = code & 0x1FF, description = (code >> 9) & 0x3FFF, shown as "<module+2000>-<description, 4 digits>"
// (the form Ryujinx and the Horizon error viewer use).
std::string format_result(uint64_t code) {
    const uint32_t c = static_cast<uint32_t>(code);
    char b[64];
    std::snprintf(b, sizeof b, "%04u-%04u (0x%08X)", (c & 0x1FFu) + 2000u, (c >> 9) & 0x3FFFu, c);
    return b;
}

// The report text. `ctx` is the 0x250-byte CPU context, or null when the caller did not supply one.
std::string build_report(uint64_t code, uint32_t policy, uint64_t pid, const uint8_t* ctx) {
    std::string out;
    char b[256];
    out += "nxrt fatal report\n";
    out += "  ResultCode: " + format_result(code) + "\n";
    std::snprintf(b, sizeof b, "  FatalPolicy: %s (%u)\n", policy_name(policy), policy);
    out += b;
    std::snprintf(b, sizeof b, "  Pid: %llu\n", static_cast<unsigned long long>(pid));
    out += b;
    if (!ctx) {
        out += "CPU context: (none)\n";
    } else {
        out += "CPU context:\n";
        // Offsets from Switchbrew Fatal services: X0..X28 at 0x00 (8 bytes each), x29 FP, x30 LR, SP, PC, PState, Afsr0, Afsr1,
        // Esr, Far at 0xE8..0x128, stack trace (32 addresses) at 0x130, start address 0x230, register set flags 0x238, count 0x240.
        const uint32_t count32 = static_cast<uint32_t>(rd64(ctx, 0x240) & 0xFFFFFFFFu);
        const size_t count = count32 > 32 ? 32 : count32;
        std::snprintf(b, sizeof b, "  StartAddress: 0x%016llx\n", static_cast<unsigned long long>(rd64(ctx, 0x230)));
        out += b;
        std::snprintf(b, sizeof b, "  RegisterSetFlags: 0x%016llx\n", static_cast<unsigned long long>(rd64(ctx, 0x238)));
        out += b;
        std::snprintf(b, sizeof b, "  StackTrace (%u%s):\n", count32, count32 > 32 ? ", first 32 shown" : "");
        out += b;
        if (count == 0) out += "    (none)\n";
        for (size_t i = 0; i < count; ++i) {
            std::snprintf(b, sizeof b, "    0x%016llx\n", static_cast<unsigned long long>(rd64(ctx, 0x130 + 8 * i)));
            out += b;
        }
        out += "  Registers:\n";
        for (size_t i = 0; i < 29; ++i) {
            std::snprintf(b, sizeof b, "    X[%02zu]: 0x%016llx\n", i, static_cast<unsigned long long>(rd64(ctx, 8 * i)));
            out += b;
        }
        static const struct { const char* name; size_t off; } named[] = {
            {"FP", 0xE8}, {"LR", 0xF0}, {"SP", 0xF8}, {"PC", 0x100}, {"PState", 0x108},
            {"Afsr0", 0x110}, {"Afsr1", 0x118}, {"Esr", 0x120}, {"Far", 0x128},
        };
        for (const auto& r : named) {
            std::snprintf(b, sizeof b, "    %s: 0x%016llx\n", r.name, static_cast<unsigned long long>(rd64(ctx, r.off)));
            out += b;
        }
    }
    out += "threads:\n";
    out += diag_thread_dump();
    out += "recent files (oldest first):\n";
    const std::vector<std::string> files = diag_recent_files();
    if (files.empty()) out += "  (none)\n";
    for (const std::string& f : files) out += "  " + f + "\n";
    return out;
}

void write_file(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        NXLOG_E("fatal", "cannot write the fatal report to %s", path.c_str());
        return;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

// The whole flow of a fatal throw: report (log, file), fatal event, terminate unless [fatal] terminate is false.
uint32_t throw_fatal(uint64_t code, uint32_t policy, uint64_t pid, const uint8_t* ctx) {
    const std::string report = build_report(code, policy, pid, ctx);
    size_t s = 0;
    while (s < report.size()) {
        size_t e = report.find('\n', s);
        if (e == std::string::npos) e = report.size();
        NXLOG_E("fatal", "%.*s", static_cast<int>(e - s), report.data() + s);
        s = e + 1;
    }
    const std::string path = report_path();
    if (!path.empty()) {
        write_file(path, report);
        NXLOG_E("fatal", "report written to %s", path.c_str());
    }
    {
        State& g = state();
        g.fatal_event.signal();
    }
    log_flush();
    if (config_get_bool("fatal", "terminate", true)) TerminateProcess(GetCurrentProcess(), 5);
    return 0;
}

// Reads the CPU context buffer; false when it is absent or shorter than 0x250 bytes (a note goes to the log).
bool read_context(const ipc::Request& req, uint8_t* out) {
    const ipc::Buffer b = req.in_buf(0);
    if (!b.addr || b.size < kCpuContextSize) {
        NXLOG_W("fatal", "CPU context buffer is %llu bytes, need 0x%zx: reported without it",
                static_cast<unsigned long long>(b.size), kCpuContextSize);
        return false;
    }
    buf_read(b, out, kCpuContextSize);
    return true;
}

std::shared_ptr<ipc::Service> make_fatal_u() {
    return make_iface("fatal:u", [](ipc::Service& s) {
        // ThrowFatal(u64 result_code) with the pid from the request. Policy: ErrorReportAndErrorScreen.
        s.add_cmd(0, "ThrowFatal", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            return throw_fatal(req.arg<uint64_t>(0), 0, req.pid, nullptr);
        });
        // ThrowFatalWithPolicy(u64 result_code, u32 policy).
        s.add_cmd(1, "ThrowFatalWithPolicy", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            return throw_fatal(req.arg<uint64_t>(0), req.arg<uint32_t>(8), req.pid, nullptr);
        });
        // ThrowFatalWithCpuContext(u64 result_code, u64 unknown, buffer 0x250 bytes, pid). The policy is read as u32 at offset 8
        // as Ryujinx does; Switchbrew calls that word "unknown", so the report may show "unknown" for it.
        s.add_cmd(2, "ThrowFatalWithCpuContext", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            uint8_t ctx[kCpuContextSize] = {};
            const bool have = read_context(req, ctx);
            return throw_fatal(req.arg<uint64_t>(0), req.arg<uint32_t>(8), req.pid, have ? ctx : nullptr);
        });
    });
}

std::shared_ptr<ipc::Service> make_fatal_p() {
    return make_iface("fatal:p", [](ipc::Service& s) {
        // GetFatalEvent: a copy of the readable event that ThrowFatal* signals.
        s.add_cmd(0, "GetFatalEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            State& g = state();
            rsp.out_copy.push_back(g.fatal_event.new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_lm() {
    return make_iface("lm", [](ipc::Service& s) {
        // OpenLogger(u64 pid) -> ILogger (a move handle in a plain session).
        s.add_cmd(0, "OpenLogger", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            uint64_t pid = req.arg<uint64_t>(0);
            if (pid == 0) pid = req.pid;
            NXLOG_I("lm", "OpenLogger for pid %llu", static_cast<unsigned long long>(pid));
            rsp.push_object(make_logger());
            return 0;
        });
    });
}

}  // namespace

// Test hook: the destination mask last set by SetDestination (default TMA).
uint32_t logfault_destination() {
    State& g = state();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.destination;
}

void register_logfault() {
    add("lm", [] { return make_lm(); });
    add("fatal:u", [] { return make_fatal_u(); });
    add("fatal:p", [] { return make_fatal_p(); });
}

void logfault_reset() {
    State& g = state();
    std::lock_guard<std::mutex> lk(g.mu);
    g.destination = 1;
    g.fatal_event.clear();
}

}  // namespace nxrt::services
