// SPDX-License-Identifier: GPL-3.0-or-later
// lm, fatal:u and fatal:p (S3c). Request bytes and expected results are written out from Switchbrew:
//   Log services   https://switchbrew.org/wiki/Log_services   (LogPacketHeader 0x18 bytes, flags Head 1 / Tail 2, severity 0..4,
//                                                             chunks = LEB128 key, LEB128 size, data; keys 2 TextLog, 3 Line, 4 File,
//                                                             5 Function, 6 Module, 7 Thread, 8 DropCount)
//   Fatal services https://switchbrew.org/wiki/Fatal_services (ThrowFatal*, GetFatalEvent, CPU context 0x250 bytes: stack count at
//                                                             0x240, stack addresses from 0x130)
// The result code 0x201 decodes to module 1 (2001) and description 1 (0001) by the rule module = code & 0x1FF, desc = code >> 9.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "svc_test_util.h"
#include "nxrt/config.h"

namespace nxrt::services {
uint32_t logfault_destination();   // src/services/logfault.cpp (test hook)
}

NXTEST_GROUP(svc_logfault)

using namespace nxrt;

namespace {

struct Capture {
    std::vector<std::string> lines;
    std::vector<LogLevel> levels;
};

void capture_sink(LogLevel l, const char* line, void* user) {
    auto* c = static_cast<Capture*>(user);
    c->lines.emplace_back(line);
    c->levels.push_back(l);
}

struct SinkGuard {
    explicit SinkGuard(Capture* c) { log_set_sink(capture_sink, c); }
    ~SinkGuard() { log_set_sink(nullptr, nullptr); }
};

// Log to the sink only: no file, no stderr, empty config, every level (so Trace lines are visible too).
void quiet_log() {
    config_clear();
    LogOptions o;
    o.path = "";
    o.level = LogLevel::Trace;
    o.to_stderr = false;
    o.keep_previous = false;
    log_init(o);
}

// Number of captured lines that contain `needle`; index of the first such line in *at (or -1).
int count_lines(const Capture& c, const std::string& needle, int* at = nullptr) {
    int n = 0;
    for (size_t i = 0; i < c.lines.size(); ++i) {
        if (c.lines[i].find(needle) != std::string::npos) {
            if (n == 0 && at) *at = static_cast<int>(i);
            ++n;
        }
    }
    if (n == 0 && at) *at = -1;
    return n;
}

// LogPacketHeader, written field by field (Switchbrew Log services): 0x0 pid u64, 0x8 thread id u64, 0x10 flags u8,
// 0x11 padding, 0x12 severity u8, 0x13 verbosity u8, 0x14 payload size u32. Then the chunks.
std::vector<uint8_t> packet(uint8_t flags, uint8_t severity, const std::vector<uint8_t>& chunks) {
    std::vector<uint8_t> p(0x18, 0);
    p[0x00] = 0x34;
    p[0x01] = 0x12;                        // pid 0x1234
    p[0x08] = 0x01;                        // thread id 1
    p[0x10] = flags;
    p[0x12] = severity;
    p[0x13] = 0;                           // verbosity
    const uint32_t n = static_cast<uint32_t>(chunks.size());
    std::memcpy(&p[0x14], &n, 4);          // little endian
    p.insert(p.end(), chunks.begin(), chunks.end());
    return p;
}

// One chunk: key and size (both one LEB128 byte here, so below 0x80), then the data.
void chunk(std::vector<uint8_t>& v, uint8_t key, const std::string& data) {
    v.push_back(key);
    v.push_back(static_cast<uint8_t>(data.size()));
    v.insert(v.end(), data.begin(), data.end());
}

void chunk_bytes(std::vector<uint8_t>& v, uint8_t key, std::vector<uint8_t> data) {
    v.push_back(key);
    v.push_back(static_cast<uint8_t>(data.size()));
    v.insert(v.end(), data.begin(), data.end());
}

// Opens a logger through lm (OpenLogger(u64 pid) -> ILogger, move handle). Returns the logger session.
nxsvc::Session open_logger(nxsvc::Session& lm, uint32_t& rc) {
    nxsvc::Reply r = lm.call(0, nxsvc::Args().u64(0x1234));
    rc = r.result;
    if (!r.ok() || r.move.size() != 1) return nxsvc::Session();
    return lm.sub(r);
}

std::string read_file(const char* path) {
    std::string s;
    FILE* f = std::fopen(path, "rb");
    if (!f) return s;
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}

}  // namespace

NXTEST(lm_open_logger_and_set_destination) {
    nxsvc::Env env;
    quiet_log();
    nxsvc::Session lm = nxsvc::connect("lm");
    REQUIRE(lm.valid());
    uint32_t rc = 0xFFFFFFFFu;
    nxsvc::Session logger = open_logger(lm, rc);
    CHECK_EQ(rc, 0u);
    REQUIRE(logger.valid());
    // Default destination mask (this runtime's choice: TMA = 1; Switchbrew does not give a default).
    CHECK_EQ(services::logfault_destination(), 1u);
    // SetDestination(u32 mask): no output, stored as a global value.
    nxsvc::Reply d = logger.call(1, nxsvc::Args().u32(0x4));
    CHECK_EQ(d.result, 0u);
    CHECK_EQ(services::logfault_destination(), 4u);
    d = logger.call(1, nxsvc::Args().u32(0xFFFF));
    CHECK_EQ(d.result, 0u);
    CHECK_EQ(services::logfault_destination(), 0xFFFFu);
}

NXTEST(lm_single_packet_all_fields) {
    nxsvc::Env env;
    quiet_log();
    Capture cap;
    SinkGuard sg(&cap);
    nxsvc::Session lm = nxsvc::connect("lm");
    uint32_t rc = 0xFFFFFFFFu;
    nxsvc::Session logger = open_logger(lm, rc);
    REQUIRE(logger.valid());

    std::vector<uint8_t> chunks;
    chunk(chunks, 2, "hello world");                    // TextLog
    chunk_bytes(chunks, 3, {0x2A, 0x00, 0x00, 0x00});   // LineNumber 42, u32 little endian
    chunk(chunks, 4, "main.cpp");                       // FileName
    chunk(chunks, 5, "boot");                           // FunctionName
    chunk(chunks, 6, "GameMod");                        // ModuleName
    std::vector<uint8_t> p = packet(0x3 /* Head | Tail */, 1 /* Info */, chunks);
    nxsvc::Reply r = logger.call(0, nxsvc::Args().buf_a(p.data(), p.size()));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(count_lines(cap, "guestlog"), 1);
    int at = -1;
    CHECK_EQ(count_lines(cap, "guestlog: [GameMod] main.cpp:42 boot: hello world", &at), 1);
    if (at >= 0) CHECK(cap.levels[static_cast<size_t>(at)] == LogLevel::Info);
}

NXTEST(lm_head_tail_multi_packet_assembles_text) {
    nxsvc::Env env;
    quiet_log();
    Capture cap;
    SinkGuard sg(&cap);
    nxsvc::Session lm = nxsvc::connect("lm");
    uint32_t rc = 0xFFFFFFFFu;
    nxsvc::Session logger = open_logger(lm, rc);
    REQUIRE(logger.valid());

    // Head packet (flags 1, severity 2 = Warn): module, file, line and the first text.
    std::vector<uint8_t> c1;
    chunk(c1, 6, "Net");
    chunk(c1, 4, "net.cpp");
    chunk_bytes(c1, 3, {0x07, 0x00, 0x00, 0x00});
    chunk(c1, 2, "one ");
    // Middle packet (flags 0): text only.
    std::vector<uint8_t> c2;
    chunk(c2, 2, "two ");
    // Tail packet (flags 2): the rest, completes the message.
    std::vector<uint8_t> c3;
    chunk(c3, 2, "three");

    nxsvc::Reply r1 = logger.call(0, nxsvc::Args().buf_a(packet(0x1, 2, c1).data(), 0x18 + c1.size()));
    CHECK_EQ(r1.result, 0u);
    CHECK_EQ(count_lines(cap, "guestlog"), 0);   // not complete yet
    std::vector<uint8_t> p2 = packet(0x0, 2, c2);
    nxsvc::Reply r2 = logger.call(0, nxsvc::Args().buf_a(p2.data(), p2.size()));
    CHECK_EQ(r2.result, 0u);
    std::vector<uint8_t> p3 = packet(0x2, 2, c3);
    nxsvc::Reply r3 = logger.call(0, nxsvc::Args().buf_a(p3.data(), p3.size()));
    CHECK_EQ(r3.result, 0u);
    CHECK_EQ(count_lines(cap, "guestlog"), 1);
    int at = -1;
    CHECK_EQ(count_lines(cap, "guestlog: [Net] net.cpp:7: one two three", &at), 1);
    if (at >= 0) CHECK(cap.levels[static_cast<size_t>(at)] == LogLevel::Warn);
}

NXTEST(lm_severity_maps_to_runtime_level) {
    nxsvc::Env env;
    quiet_log();
    Capture cap;
    SinkGuard sg(&cap);
    nxsvc::Session lm = nxsvc::connect("lm");
    uint32_t rc = 0xFFFFFFFFu;
    nxsvc::Session logger = open_logger(lm, rc);
    REQUIRE(logger.valid());
    // severity 0 Trace, 1 Info, 2 Warn, 3 Error, 4 Fatal (nn::diag::LogSeverity)
    const LogLevel want[5] = {LogLevel::Trace, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Error};
    for (uint8_t sev = 0; sev < 5; ++sev) {
        std::vector<uint8_t> c;
        chunk(c, 2, "s" + std::to_string(sev));
        std::vector<uint8_t> p = packet(0x3, sev, c);
        CHECK_EQ(logger.call(0, nxsvc::Args().buf_a(p.data(), p.size())).result, 0u);
        int at = -1;
        CHECK_EQ(count_lines(cap, "guestlog: s" + std::to_string(sev), &at), 1);
        if (at >= 0) CHECK(cap.levels[static_cast<size_t>(at)] == want[sev]);
    }
}

NXTEST(lm_malformed_packets_answer_success) {
    nxsvc::Env env;
    quiet_log();
    Capture cap;
    SinkGuard sg(&cap);
    nxsvc::Session lm = nxsvc::connect("lm");
    uint32_t rc = 0xFFFFFFFFu;
    nxsvc::Session logger = open_logger(lm, rc);
    REQUIRE(logger.valid());

    // a) shorter than the 0x18-byte header
    uint8_t shortp[10] = {};
    CHECK_EQ(logger.call(0, nxsvc::Args().buf_a(shortp, sizeof shortp)).result, 0u);
    CHECK_EQ(count_lines(cap, "shorter than its 0x18-byte header"), 1);

    // b) payload size in the header (5) does not match the buffer (0x18 + 3)
    std::vector<uint8_t> c;
    chunk(c, 2, "x");
    std::vector<uint8_t> b = packet(0x3, 1, c);
    b[0x14] = 5;
    CHECK_EQ(logger.call(0, nxsvc::Args().buf_a(b.data(), b.size())).result, 0u);
    CHECK_EQ(count_lines(cap, "size mismatch"), 1);

    // c) a chunk that claims 100 bytes in a 3-byte payload
    std::vector<uint8_t> bad = {2, 100, 'a', 'b', 'c'};
    std::vector<uint8_t> pc = packet(0x3, 1, bad);
    CHECK_EQ(logger.call(0, nxsvc::Args().buf_a(pc.data(), pc.size())).result, 0u);
    CHECK_EQ(count_lines(cap, "malformed log chunk"), 1);

    // d) a LEB128 number that never ends (0x82 has the continuation bit, no byte follows)
    std::vector<uint8_t> trunc = {0x82};
    std::vector<uint8_t> pd = packet(0x3, 1, trunc);
    CHECK_EQ(logger.call(0, nxsvc::Args().buf_a(pd.data(), pd.size())).result, 0u);
    CHECK_EQ(count_lines(cap, "malformed log chunk"), 2);

    // The logger still works afterwards.
    std::vector<uint8_t> ok;
    chunk(ok, 2, "after");
    std::vector<uint8_t> po = packet(0x3, 1, ok);
    CHECK_EQ(logger.call(0, nxsvc::Args().buf_a(po.data(), po.size())).result, 0u);
    CHECK_EQ(count_lines(cap, "guestlog: after"), 1);
}

NXTEST(lm_new_head_discards_unfinished_message) {
    nxsvc::Env env;
    quiet_log();
    Capture cap;
    SinkGuard sg(&cap);
    nxsvc::Session lm = nxsvc::connect("lm");
    uint32_t rc = 0xFFFFFFFFu;
    nxsvc::Session logger = open_logger(lm, rc);
    REQUIRE(logger.valid());
    std::vector<uint8_t> c1;
    chunk(c1, 2, "abandoned");
    std::vector<uint8_t> p1 = packet(0x1, 1, c1);      // head without tail
    CHECK_EQ(logger.call(0, nxsvc::Args().buf_a(p1.data(), p1.size())).result, 0u);
    std::vector<uint8_t> c2;
    chunk(c2, 2, "fresh");
    std::vector<uint8_t> p2 = packet(0x3, 1, c2);      // a new complete message
    CHECK_EQ(logger.call(0, nxsvc::Args().buf_a(p2.data(), p2.size())).result, 0u);
    CHECK_EQ(count_lines(cap, "a new message started before the previous one was completed"), 1);
    CHECK_EQ(count_lines(cap, "abandoned"), 0);
    CHECK_EQ(count_lines(cap, "guestlog: fresh"), 1);
}

NXTEST(lm_drop_count_thread_and_multibyte_length) {
    nxsvc::Env env;
    quiet_log();
    Capture cap;
    SinkGuard sg(&cap);
    nxsvc::Session lm = nxsvc::connect("lm");
    uint32_t rc = 0xFFFFFFFFu;
    nxsvc::Session logger = open_logger(lm, rc);
    REQUIRE(logger.valid());
    // TextLog of 200 bytes: its size is LEB128 200 = 0xC8 0x01 (two bytes), so the size itself is a multi-byte number.
    std::string text(200, 'x');
    std::vector<uint8_t> c;
    chunk(c, 7, "worker");                                       // ThreadName
    chunk_bytes(c, 8, {3, 0, 0, 0, 0, 0, 0, 0});                 // DropCount 3, u64 little endian
    c.push_back(2);                                              // TextLog key
    c.push_back(0xC8);
    c.push_back(0x01);                                           // size 200
    c.insert(c.end(), text.begin(), text.end());
    std::vector<uint8_t> p = packet(0x3, 1, c);
    CHECK_EQ(logger.call(0, nxsvc::Args().buf_a(p.data(), p.size())).result, 0u);
    CHECK_EQ(count_lines(cap, "guestlog: " + text + " (thread worker, 3 packets dropped)"), 1);
}

NXTEST(lm_unknown_commands_and_missing_buffer) {
    nxsvc::Env env;
    quiet_log();
    Capture cap;
    SinkGuard sg(&cap);
    nxsvc::Session lm = nxsvc::connect("lm");
    uint32_t rc = 0xFFFFFFFFu;
    nxsvc::Session logger = open_logger(lm, rc);
    REQUIRE(logger.valid());
    CHECK_EQ(lm.call(9).result, kernel::kStubService);
    CHECK_EQ(logger.call(7).result, kernel::kStubService);
    // Log with no buffer: success, a warning, nothing written.
    nxsvc::Reply r = logger.call(0);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(count_lines(cap, "Log: no message buffer"), 1);
}

NXTEST(fatal_throw_writes_report_and_signals_event) {
    nxsvc::Env env;
    quiet_log();
    config_set("fatal", "terminate", "false");
    config_set("fatal", "report_path", "nxrt_fatal_test.txt");
    std::remove("nxrt_fatal_test.txt");
    Capture cap;
    SinkGuard sg(&cap);

    nxsvc::Session fp = nxsvc::connect("fatal:p");
    REQUIRE(fp.valid());
    nxsvc::Reply ev = fp.call(0);                                 // GetFatalEvent
    CHECK_EQ(ev.result, 0u);
    REQUIRE(ev.copy.size() == 1);
    const uint32_t h = ev.copy[0];
    int32_t idx = -1;
    CHECK_EQ(kernel::wait_synchronization(&h, 1, 0, &idx), kernel::kTimedOut);

    nxsvc::Session fu = nxsvc::connect("fatal:u");
    REQUIRE(fu.valid());
    nxsvc::Reply t = fu.call(0, nxsvc::Args().u64(0x201));        // ThrowFatal(0x201)
    CHECK_EQ(t.result, 0u);                                        // returns because terminate = false
    CHECK_EQ(kernel::wait_synchronization(&h, 1, 0, &idx), 0u);   // the event is signalled

    const std::string rep = read_file("nxrt_fatal_test.txt");
    REQUIRE(!rep.empty());
    CHECK(rep.find("ResultCode: 2001-0001 (0x00000201)") != std::string::npos);
    CHECK(rep.find("FatalPolicy: ErrorReportAndErrorScreen (0)") != std::string::npos);
    CHECK(rep.find("CPU context: (none)") != std::string::npos);
    CHECK(rep.find("threads:") != std::string::npos);
    CHECK(rep.find("recent files (oldest first):") != std::string::npos);
    int at = -1;
    CHECK(count_lines(cap, "ResultCode: 2001-0001", &at) == 1);
    if (at >= 0) CHECK(cap.levels[static_cast<size_t>(at)] == LogLevel::Error);
    kernel::handle_close(h);
    std::remove("nxrt_fatal_test.txt");
    config_clear();
}

NXTEST(fatal_policy_and_cpu_context_report) {
    nxsvc::Env env;
    quiet_log();
    config_set("fatal", "terminate", "false");
    config_set("fatal", "report_path", "nxrt_fatal_test2.txt");
    std::remove("nxrt_fatal_test2.txt");

    nxsvc::Session fu = nxsvc::connect("fatal:u");
    REQUIRE(fu.valid());
    // ThrowFatalWithPolicy(u64 result_code = 0x201, u32 policy = 1 ErrorReport)
    nxsvc::Reply p = fu.call(1, nxsvc::Args().u64(0x201).u32(1));
    CHECK_EQ(p.result, 0u);
    CHECK(read_file("nxrt_fatal_test2.txt").find("FatalPolicy: ErrorReport (1)") != std::string::npos);

    // ThrowFatalWithCpuContext(u64 result_code, u32 policy at offset 8 (as Ryujinx reads it), buffer 0x250 bytes).
    // Context bytes written by hand from the Switchbrew offsets:
    //   X0 at 0x000 = 0x1122334455667788, PC at 0x100 = 0x401000, StartAddress at 0x230 = 0x400000,
    //   RegisterSetFlags at 0x238 = 1, stack count at 0x240 (u32) = 2, stack[0] at 0x130 = 0x402000, stack[1] at 0x138 = 0x403000.
    uint8_t ctx[0x250];
    std::memset(ctx, 0, sizeof ctx);
    const uint8_t x0[8] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    std::memcpy(ctx + 0x000, x0, 8);
    const uint64_t pc = 0x401000, start = 0x400000, flags = 1, s0 = 0x402000, s1 = 0x403000;
    std::memcpy(ctx + 0x100, &pc, 8);
    std::memcpy(ctx + 0x230, &start, 8);
    std::memcpy(ctx + 0x238, &flags, 8);
    const uint32_t count = 2;
    std::memcpy(ctx + 0x240, &count, 4);
    std::memcpy(ctx + 0x130, &s0, 8);
    std::memcpy(ctx + 0x138, &s1, 8);
    nxsvc::Reply c = fu.call(2, nxsvc::Args().u64(0x201).u32(2).buf_a(ctx, sizeof ctx));
    CHECK_EQ(c.result, 0u);
    const std::string rep = read_file("nxrt_fatal_test2.txt");
    CHECK(rep.find("FatalPolicy: ErrorScreen (2)") != std::string::npos);
    CHECK(rep.find("CPU context:") != std::string::npos);
    CHECK(rep.find("StartAddress: 0x0000000000400000") != std::string::npos);
    CHECK(rep.find("StackTrace (2):") != std::string::npos);
    CHECK(rep.find("0x0000000000402000") != std::string::npos);
    CHECK(rep.find("0x0000000000403000") != std::string::npos);
    CHECK(rep.find("X[00]: 0x1122334455667788") != std::string::npos);
    CHECK(rep.find("PC: 0x0000000000401000") != std::string::npos);
    std::remove("nxrt_fatal_test2.txt");
    config_clear();
}

NXTEST(fatal_context_absent_or_short) {
    nxsvc::Env env;
    quiet_log();
    Capture cap;
    SinkGuard sg(&cap);
    config_set("fatal", "terminate", "false");
    config_set("fatal", "report_path", "nxrt_fatal_test3.txt");
    std::remove("nxrt_fatal_test3.txt");
    nxsvc::Session fu = nxsvc::connect("fatal:u");
    REQUIRE(fu.valid());
    // No buffer at all.
    CHECK_EQ(fu.call(2, nxsvc::Args().u64(0x201).u32(0)).result, 0u);
    CHECK(read_file("nxrt_fatal_test3.txt").find("CPU context: (none)") != std::string::npos);
    // A buffer shorter than 0x250 bytes: reported without the context, with a warning.
    uint8_t small[0x100] = {};
    CHECK_EQ(fu.call(2, nxsvc::Args().u64(0x201).u32(0).buf_a(small, sizeof small)).result, 0u);
    CHECK_EQ(count_lines(cap, "CPU context buffer is 256 bytes, need 0x250"), 1);
    std::remove("nxrt_fatal_test3.txt");
    config_clear();
}

NXTEST(lm_fatal_state_resets) {
    {
        nxsvc::Env env;
        quiet_log();
        nxsvc::Session lm = nxsvc::connect("lm");
        uint32_t rc = 0xFFFFFFFFu;
        nxsvc::Session logger = open_logger(lm, rc);
        REQUIRE(logger.valid());
        CHECK_EQ(logger.call(1, nxsvc::Args().u32(0x4)).result, 0u);
        CHECK_EQ(services::logfault_destination(), 4u);
    }
    {
        nxsvc::Env env;   // reset_all() runs logfault_reset()
        CHECK_EQ(services::logfault_destination(), 1u);
        nxsvc::Session fp = nxsvc::connect("fatal:p");
        nxsvc::Reply ev = fp.call(0);
        REQUIRE(ev.copy.size() == 1);
        int32_t idx = -1;
        CHECK_EQ(kernel::wait_synchronization(&ev.copy[0], 1, 0, &idx), kernel::kTimedOut);
        kernel::handle_close(ev.copy[0]);
    }
    config_clear();
}
