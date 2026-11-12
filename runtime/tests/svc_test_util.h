// SPDX-License-Identifier: GPL-3.0-or-later
// Test client for the service library (S3c): a very small libnx. It builds CMIF requests the way libnx packs them
// (https://switchbrew.org/wiki/IPC_Marshalling: SFCI header, raw arguments, A/B/X/C buffers, copy/move handles, pid), sends them
// through ipc::send_sync_request on a real session handle and unpacks the reply. A test says what the bytes of the request are
// (taken from Switchbrew or libnx, never from the service under test) and what the reply bytes must be.
//
//   nxsvc::Env env;                                   // runtime bring-up (memory, svc table) + register_all(), once per test
//   auto fs = nxsvc::connect("fsp-srv");              // through sm: like a real client (ConnectToNamedPort + RegisterClient + GetServiceHandle)
//   nxsvc::Reply r = fs.call(1, nxsvc::Args().u64(0)); // SetCurrentProcess(u64 pid) ... CHECK_EQ(r.result, 0u);
//   auto sub = fs.sub(r);                              // the interface returned as a move handle
//   ... Args().u32(1).u64(2) packs inputs, Reply::get<uint32_t>(0) reads outputs ...
#pragma once
#include <malloc.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/hipc.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/runtime.h"
#include "nxrt/services.h"
#include "nxrt/svc.h"
#include "test_framework.h"

namespace nxsvc {

constexpr uint32_t kSfci = 0x49434653u;
constexpr uint32_t kSfco = 0x4F434653u;

// Request arguments: raw data in order, plus handles and buffers.
struct Args {
    std::vector<uint8_t> raw;
    std::vector<uint32_t> copy, move;
    bool pid = false;
    std::vector<nxrt::hipc::BufDesc> a, b, w;
    std::vector<nxrt::hipc::PtrDesc> x;
    std::vector<nxrt::hipc::RecvDesc> c;
    uint32_t recv_flags = 0;
    Args& bytes(const void* p, size_t n) {
        const uint8_t* s = static_cast<const uint8_t*>(p);
        raw.insert(raw.end(), s, s + n);
        return *this;
    }
    template <class T>
    Args& put(T v) { return bytes(&v, sizeof v); }
    Args& u8(uint8_t v) { return put(v); }
    Args& u16(uint16_t v) { return put(v); }
    Args& u32(uint32_t v) { return put(v); }
    Args& u64(uint64_t v) { return put(v); }
    Args& i32(int32_t v) { return put(v); }
    Args& i64(int64_t v) { return put(v); }
    Args& f32(float v) { return put(v); }
    Args& pad(size_t n) { raw.resize(raw.size() + n, 0); return *this; }
    Args& align(size_t n) { while (raw.size() % n) raw.push_back(0); return *this; }   // CMIF aligns u64 arguments to 8 within the raw data
    Args& hcopy(uint32_t h) { copy.push_back(h); return *this; }
    Args& hmove(uint32_t h) { move.push_back(h); return *this; }
    Args& with_pid() { pid = true; return *this; }
    // Buffers by Switchbrew type: A = send (service reads), B = receive (service writes), X = pointer (service reads), C = receive list.
    Args& buf_a(const void* p, uint64_t n, uint32_t attr = 0) { a.push_back({reinterpret_cast<uint64_t>(p), n, attr}); return *this; }
    Args& buf_b(void* p, uint64_t n, uint32_t attr = 0) { b.push_back({reinterpret_cast<uint64_t>(p), n, attr}); return *this; }
    Args& buf_w(void* p, uint64_t n, uint32_t attr = 0) { w.push_back({reinterpret_cast<uint64_t>(p), n, attr}); return *this; }
    Args& buf_x(const void* p, uint32_t n, uint32_t index = 0) { x.push_back({reinterpret_cast<uint64_t>(p), n, index}); return *this; }
    Args& buf_c(void* p, uint32_t n) { c.push_back({reinterpret_cast<uint64_t>(p), n}); recv_flags = static_cast<uint32_t>(c.size()) + 2; return *this; }
};

struct Reply {
    uint32_t svc_rc = 0xFFFFFFFFu;      // result of the SendSyncRequest SVC itself
    uint32_t result = 0xFFFFFFFFu;      // CMIF result in the SFCO header
    std::vector<uint8_t> out;           // raw output data after the SFCO header (padding included; read by offset)
    std::vector<uint32_t> copy, move;   // handles returned
    std::vector<uint32_t> objects;      // domain replies: object ids (needs the output size; see Session::call_obj)
    bool ok() const { return svc_rc == 0 && result == 0; }
    template <class T>
    T get(size_t off) const {
        T v{};
        if (off + sizeof(T) <= out.size()) std::memcpy(&v, out.data() + off, sizeof(T));
        return v;
    }
    std::string str(size_t off, size_t max) const {
        std::string s;
        for (size_t i = off; i < out.size() && i < off + max && out[i]; ++i) s += static_cast<char>(out[i]);
        return s;
    }
};

// A 16-byte aligned 0x1000-byte message buffer, like the TLS region.
struct MsgBuf {
    uint32_t* p;
    MsgBuf() : p(static_cast<uint32_t*>(_aligned_malloc(0x1000, 16))) { std::memset(p, 0, 0x1000); }
    ~MsgBuf() { _aligned_free(p); }
    MsgBuf(const MsgBuf&) = delete;
    MsgBuf& operator=(const MsgBuf&) = delete;
};

class Session {
public:
    Session() = default;
    explicit Session(uint32_t h) : h_(h) {}
    Session(Session&& o) noexcept : h_(o.h_), domain_(o.domain_) { o.h_ = 0; }
    Session& operator=(Session&& o) noexcept {
        close();
        h_ = o.h_;
        domain_ = o.domain_;
        o.h_ = 0;
        return *this;
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { close(); }
    uint32_t handle() const { return h_; }
    bool valid() const { return h_ != 0; }
    // Gives the handle up without closing it (for a handle that is then moved to the service).
    uint32_t release() {
        const uint32_t h = h_;
        h_ = 0;
        return h;
    }
    void close() {
        if (h_) {
            nxrt::kernel::handle_close(h_);
            h_ = 0;
        }
    }

    // One CMIF request (message type 4) on this session. In domain mode the request goes to `object_id` (default: object 1).
    Reply call(uint32_t cmd, const Args& args = {}, uint32_t object_id = 1, size_t domain_out_size = 0) {
        using namespace nxrt;
        Reply rep;
        MsgBuf mb;
        hipc::Message m;
        m.type = hipc::kInvoke2MethodOld;
        std::vector<uint8_t> sf;
        push32(sf, kSfci);
        push32(sf, 0);          // version + reserved halves
        push32(sf, cmd);
        push32(sf, 0);          // token
        sf.insert(sf.end(), args.raw.begin(), args.raw.end());
        if (domain_) {          // CmifDomainInHeader: kind 1 (send message), object count, raw size, object id, padding, token
            std::vector<uint8_t> d;
            d.push_back(1);
            d.push_back(0);
            const uint16_t sz = static_cast<uint16_t>(sf.size());
            d.insert(d.end(), reinterpret_cast<const uint8_t*>(&sz), reinterpret_cast<const uint8_t*>(&sz) + 2);
            push32(d, object_id);
            push32(d, 0);
            push32(d, 0);
            d.insert(d.end(), sf.begin(), sf.end());
            m.raw = d;
        } else {
            m.raw = sf;
        }
        m.has_pid = args.pid;
        m.copy = args.copy;
        m.move = args.move;
        m.send = args.a;
        m.recv = args.b;
        m.exch = args.w;
        m.ptrs = args.x;
        m.recv_list = args.c;
        m.recv_list_flags = args.recv_flags;
        std::string err;
        if (hipc::build(m, mb.p, 0x100 / 4, &err) == 0) {
            nxtest::fail(__FILE__, __LINE__, "hipc::build", err);
            return rep;
        }
        rep.svc_rc = ipc::send_sync_request(nullptr, h_, reinterpret_cast<uint64_t>(mb.p), 0x100);
        if (rep.svc_rc != 0) return rep;
        hipc::Message r;
        if (!hipc::parse(mb.p, 0x100 / 4, r, &err)) {
            nxtest::fail(__FILE__, __LINE__, "hipc::parse(reply)", err);
            return rep;
        }
        const size_t base = domain_ ? 16 : 0;       // domain replies: CmifDomainOutHeader (object count + 12 reserved) first
        if (r.raw.size() >= base + 16) {
            std::memcpy(&rep.result, r.raw.data() + base + 8, 4);
            rep.out.assign(r.raw.begin() + static_cast<long>(base + 16), r.raw.end());
        }
        rep.copy = r.copy;
        rep.move = r.move;
        if (domain_ && r.raw.size() >= 4) {
            uint32_t n = 0;
            std::memcpy(&n, r.raw.data(), 4);
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t id = 0;
                const size_t at = 16 + 16 + domain_out_size + 4 * i;
                if (at + 4 <= r.raw.size()) std::memcpy(&id, r.raw.data() + at, 4);
                rep.objects.push_back(id);
            }
        }
        return rep;
    }

    // The interface returned as the n-th move handle of a reply (non-domain session).
    Session sub(const Reply& r, size_t n = 0) const { return Session(n < r.move.size() ? r.move[n] : 0); }

    // ConvertCurrentObjectToDomain (control message type 5); afterwards call() addresses objects by id (this one is object 1).
    uint32_t to_domain() {
        using namespace nxrt;
        MsgBuf mb;
        hipc::Message m;
        m.type = hipc::kInvoke2ManagerMethodOld;
        std::vector<uint8_t> sf;
        push32(sf, kSfci);
        push32(sf, 0);
        push32(sf, 0);    // method 0
        push32(sf, 0);
        m.raw = sf;
        std::string err;
        if (hipc::build(m, mb.p, 0x100 / 4, &err) == 0) return 0;
        if (ipc::send_sync_request(nullptr, h_, reinterpret_cast<uint64_t>(mb.p), 0x100) != 0) return 0;
        hipc::Message r;
        if (!hipc::parse(mb.p, 0x100 / 4, r, &err) || r.raw.size() < 20) return 0;
        domain_ = true;
        uint32_t id = 0;
        std::memcpy(&id, r.raw.data() + 16, 4);
        return id;
    }
    bool domain() const { return domain_; }

private:
    static void push32(std::vector<uint8_t>& v, uint32_t x) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
        v.insert(v.end(), p, p + 4);
    }
    uint32_t h_ = 0;
    bool domain_ = false;
};

// Brings the runtime far enough up for service tests (memory map for kernel allocations, SVC table) and registers every service.
// Cheap to construct again; resets the service state and the ipc statistics.
struct Env {
    Env() {
        nxrt::InitOptions io;
        io.crash_handler = false;
        io.watchdog = false;
        io.memguard = false;
        nxrt::init(io);
        nxrt::ipc::registry_reset();
        nxrt::services::register_all();
        nxrt::services::reset_all();
    }
};

// Like a real client: ConnectToNamedPort("sm:"), RegisterClient, GetServiceHandle(name). An invalid Session when sm: refuses.
inline Session connect(const char* name) {
    uint32_t smh = 0;
    if (nxrt::ipc::connect_named_port("sm:", &smh) != 0) return Session();
    Session sm(smh);
    sm.call(0, Args().with_pid().u64(0));
    char n[8] = {};
    std::strncpy(n, name, 8);
    Reply r = sm.call(1, Args().bytes(n, 8));
    if (!r.ok() || r.move.empty()) return Session();
    return Session(r.move[0]);
}

}  // namespace nxsvc
