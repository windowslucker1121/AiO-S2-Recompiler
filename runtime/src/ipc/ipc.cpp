// SPDX-License-Identifier: GPL-3.0-or-later
// Structure adapted from SuperMarioWonderRecomp runtime/src/ipc.cpp (MIT, Copyright (c) 2026 BelmanteGu); see THIRD_PARTY_NOTICES.md.
// Service registry, sessions and domains, request routing (CMIF, TIPC, control messages, Release) and the reply writer.
// The wire format itself (parse/build of the message words) is nxrt::hipc in hipc.cpp; this file only interprets the result.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/hipc.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/gaps.h"

namespace nxrt::ipc {
namespace detail {
// Defined below. Counts one unhandled command of `s` (on the object and per service name) and returns the per-name count
// of that command, including this one. Used by Service::handle and by StubService::handle (stub.cpp).
uint64_t note_unhandled(Service& s, uint32_t cmd);
}  // namespace detail

namespace {

constexpr uint32_t kSfci = 0x49434653;                                   // 'SFCI': CMIF request header
constexpr uint32_t kSfco = 0x4F434653;                                   // 'SFCO': CMIF response header
constexpr uint32_t kCmifInvalidObject = 0xF601;                          // CMIF: unknown domain object or bad magic
constexpr uint32_t kInvalidCombination = kernel::make_result(1, 116);   // 0xE801: malformed message (kernel)
constexpr uint64_t kMaxMsgBytes = 0x1000;
constexpr size_t kHeaderBytes = 16;                                      // CmifIn/OutHeader, CmifDomain*Header, SFCI payload start
constexpr size_t kConnectNameMax = 12;                                   // ConnectToNamedPort reads at most 12 bytes

// Gauges (include/nxrt/gauge.h). Domain objects: +1 when an object enters a domain's table, -1 when it leaves it (erased, or the domain dies).
GaugeCounter g_domain_objects("ipc.domain_objects");
GaugeCounter g_requests_pending("ipc.requests_pending");   // svcSendSyncRequest calls in flight

enum class Wire { Cmif, Domain, Tipc };                                  // how a reply is laid out

// Everything below is guarded by one recursive mutex: the registry, per-name counters, and session/domain state. It is never held
// while a service handler runs (handlers may block).
std::recursive_mutex& mu() {
    static std::recursive_mutex m;
    return m;
}

struct Registry {
    std::map<std::string, ServiceFactory> factories;
    std::vector<std::string> stubs_requested;
    std::map<std::string, ServiceStats> stats;   // per service name, accumulated over sessions
};

Registry& reg() {
    static Registry r = [] {
        Registry x;
        x.factories["sm:"] = [] { return make_sm(); };   // built-in
        return x;
    }();
    return r;
}

ServiceStats& stats_for(const std::string& name) {   // caller holds mu()
    ServiceStats& s = reg().stats[name];
    if (s.name.empty()) s.name = name;
    return s;
}

void count_request(Service& s) {
    std::lock_guard<std::recursive_mutex> lk(mu());
    s.requests++;
    ServiceStats& st = stats_for(s.name());
    st.requests++;
    st.stub = s.is_stub();
}

uint32_t rd16(const std::vector<uint8_t>& v, size_t off) {
    if (off + 2 > v.size()) return 0;
    uint16_t x;
    std::memcpy(&x, v.data() + off, 2);
    return x;
}

uint32_t rd32(const std::vector<uint8_t>& v, size_t off) {
    if (off + 4 > v.size()) return 0;
    uint32_t x;
    std::memcpy(&x, v.data() + off, 4);
    return x;
}

template <class T>
void push_raw(std::vector<uint8_t>& v, T x) {
    const size_t o = v.size();
    v.resize(o + sizeof(T));
    std::memcpy(v.data() + o, &x, sizeof(T));
}

std::vector<uint8_t> sfco(uint32_t result) {   // CmifOutHeader: 'SFCO', version 0, reserved, result, interface id 0
    std::vector<uint8_t> v;
    push_raw(v, kSfco);
    push_raw<uint16_t>(v, 0);
    push_raw<uint16_t>(v, 0);
    push_raw(v, result);
    push_raw<uint32_t>(v, 0);
    return v;
}

// Guest buffers the request describes, as the handler sees them.
void fill_request_buffers(const hipc::Message& m, Request& req) {
    for (const hipc::BufDesc& d : m.send) req.a.push_back(Buffer{d.addr, d.size});
    for (const hipc::BufDesc& d : m.recv) req.b.push_back(Buffer{d.addr, d.size});
    for (const hipc::BufDesc& d : m.exch) req.w.push_back(Buffer{d.addr, d.size});
    for (const hipc::PtrDesc& p : m.ptrs) req.x.push_back(Buffer{p.addr, p.size});
    for (const hipc::RecvDesc& r : m.recv_list) req.c.push_back(Buffer{r.addr, r.size});
    req.in_copy = m.copy;
    req.in_move = m.move;
    req.pid = m.has_pid ? m.pid : 0;
}

// Runs the handler of `svc` (not under the lock). A C++ exception becomes kInvalidState and drops the outputs.
uint32_t run_handler(const std::shared_ptr<Service>& svc, Request& req, Response& rsp) {
    count_request(*svc);
    try {
        const uint32_t result = svc->handle(req, rsp);
        NXLOG_T("ipc", "%s cmd %u (in %zu bytes, %zu copy, %zu move) -> 0x%x (out %zu bytes, %zu copy, %zu objects)", svc->name().c_str(), req.cmd,
                req.in_size, req.in_copy.size(), req.in_move.size(), result, rsp.out.size(), rsp.out_copy.size(), rsp.out_objects.size());
        return result;
    } catch (const std::exception& e) {
        NXLOG_E("ipc", "service '%s' cmd %u threw: %s", svc->name().c_str(), req.cmd, e.what());
    } catch (...) {
        NXLOG_E("ipc", "service '%s' cmd %u threw a non-standard exception", svc->name().c_str(), req.cmd);
    }
    rsp = Response{};
    return kernel::kInvalidState;
}

// Closes the move handles of a request (a move transfers them to the service) once it has been handled.
void consume_moved_handles(const hipc::Message& m) {
    for (uint32_t h : m.move) kernel::handle_close(h);
}

// Writes the reply for `result` over `words` (cap words). `rsp` is null for a reply without outputs. Domain replies carry
// `dom` to register returned objects in. Only the results of a successful handler (result 0) carry outputs, handles and objects.
// When the reply does not fit the buffer, the handles and objects made for it are undone and a header-only reply with
// 0xF601 is written instead.
void write_reply(uint32_t* words, size_t cap, Wire wire, uint32_t result, Response* rsp, const std::shared_ptr<DomainState>& dom) {
    const bool ok = result == 0 && rsp != nullptr;
    std::vector<uint32_t> made;   // session handles created for returned objects (CMIF and TIPC): moved to the client
    std::vector<uint32_t> ids;    // domain object ids registered for returned objects (domain replies)
    std::string why;
    hipc::Message m;
    // TIPC: raw data without padding (Switchbrew: "RawData field directly contains the payload"). hipc::build pads type 0 but not
    // a TIPC type, so the reply is built as a TIPC message and the type is then set to 0.
    m.type = wire == Wire::Tipc ? hipc::kTipcBase : hipc::kInvalid;

    if (ok) {
        for (const std::shared_ptr<Service>& o : rsp->out_objects) {
            if (!o) continue;
            if (wire == Wire::Domain) {
                std::lock_guard<std::recursive_mutex> lk(mu());
                ids.push_back(dom->next_object++);
                dom->objects[ids.back()] = o;
                g_domain_objects.add(1);
            } else if (const uint32_t h = create_session_handle(o)) {
                made.push_back(h);
            } else {
                why = "no free handle for a returned object";
                break;
            }
        }
    }

    if (why.empty()) {
        if (wire == Wire::Domain) {   // CmifDomainMessageOutHeader: object count, 12 reserved bytes
            push_raw<uint32_t>(m.raw, ok ? static_cast<uint32_t>(ids.size()) : 0u);
            m.raw.resize(kHeaderBytes, 0);
        }
        if (wire == Wire::Tipc) {
            push_raw(m.raw, result);
        } else {
            const std::vector<uint8_t> h = sfco(result);
            m.raw.insert(m.raw.end(), h.begin(), h.end());
        }
        if (ok) {
            m.raw.insert(m.raw.end(), rsp->out.begin(), rsp->out.end());
            for (uint32_t id : ids) push_raw(m.raw, id);
            m.copy = rsp->out_copy;
            m.move = rsp->out_move;
            m.move.insert(m.move.end(), made.begin(), made.end());
        }
        std::string err;
        if (hipc::build(m, words, cap, &err) != 0) {
            if (wire == Wire::Tipc) words[0] &= 0xFFFF0000u;   // type 0
            return;
        }
        why = err.empty() ? "message buffer too small" : err;
    }

    // The reply does not fit (or a handle could not be made): undo what was made for it and answer with the header only.
    NXLOG_E("ipc", "reply does not fit the message buffer (%s): answering 0x%x", why.c_str(), kCmifInvalidObject);
    for (uint32_t h : made) kernel::handle_close(h);
    if (rsp)
        for (uint32_t h : rsp->out_move) kernel::handle_close(h);   // dropped: never reached the client
    if (!ids.empty()) {
        std::lock_guard<std::recursive_mutex> lk(mu());
        for (uint32_t id : ids)
            if (dom->objects.erase(id)) g_domain_objects.sub(1);
    }
    hipc::Message f;
    f.type = wire == Wire::Tipc ? hipc::kTipcBase : hipc::kInvalid;
    if (wire == Wire::Tipc) {
        push_raw(f.raw, kCmifInvalidObject);
    } else {
        if (wire == Wire::Domain) f.raw.resize(kHeaderBytes, 0);
        const std::vector<uint8_t> h = sfco(kCmifInvalidObject);
        f.raw.insert(f.raw.end(), h.begin(), h.end());
    }
    if (hipc::build(f, words, cap, nullptr) == 0) {
        NXLOG_E("ipc", "reply header does not fit the message buffer");
    } else if (wire == Wire::Tipc) {
        words[0] &= 0xFFFF0000u;
    }
}

// Control messages (types 5 and 7): the raw data starts with the SFCI header, never with a domain header.
uint32_t control(const std::shared_ptr<KSession>& s, const hipc::Message& m, Response& rsp) {
    if (rd32(m.raw, 0) != kSfci) {
        NXLOG_E("ipc", "control message without an SFCI header (magic 0x%08x)", rd32(m.raw, 0));
        return kCmifInvalidObject;
    }
    const uint32_t method = rd32(m.raw, 8);
    switch (method) {
        case 0: {   // ConvertCurrentObjectToDomain: the session's service becomes object 1; output: that id
            std::lock_guard<std::recursive_mutex> lk(mu());
            if (!s->domain) {
                s->dom = std::make_shared<DomainState>();
                s->dom->objects[1] = s->svc;
                g_domain_objects.add(1);
                s->dom->next_object = 2;
                s->domain = true;
            }
            rsp.push<uint32_t>(1);
            return 0;
        }
        case 1: {   // CopyFromCurrentDomain: input object id; output a move handle to a new session for that object
            std::shared_ptr<Service> obj;
            {
                std::lock_guard<std::recursive_mutex> lk(mu());
                if (!s->domain) return kernel::kInvalidState;
                auto it = s->dom->objects.find(rd32(m.raw, kHeaderBytes));
                if (it == s->dom->objects.end()) return kCmifInvalidObject;
                obj = it->second;
            }
            const uint32_t h = create_session_handle(obj);
            if (!h) return kernel::kOutOfResource;
            rsp.out_move.push_back(h);
            return 0;
        }
        case 2:     // CloneCurrentObject
        case 4: {   // CloneCurrentObjectEx (input: a u32 tag, ignored): a new session on the same service and domain
            auto clone = std::make_shared<KSession>();
            {
                std::lock_guard<std::recursive_mutex> lk(mu());
                clone->svc = s->svc;
                clone->domain = s->domain;
                clone->dom = s->dom;
            }
            const uint32_t h = kernel::handle_add(clone);
            if (!h) return kernel::kOutOfResource;
            rsp.out_move.push_back(h);
            return 0;
        }
        case 3:   // QueryPointerBufferSize: a u16 (0x8000) in a 4-byte slot
            rsp.push<uint32_t>(0x8000u);
            return 0;
        default:
            NXLOG_W("ipc", "control command %u not implemented", method);
            return kernel::kNotImplemented;
    }
}

}  // namespace

// ---- Service ------------------------------------------------------------------------------------------------------------

uint32_t Service::handle(Request& req, Response& rsp) {
    auto it = cmds_.find(req.cmd);
    if (it != cmds_.end()) return it->second.fn(req, rsp);
    // [ipc] stub_result (NXRT_IPC_STUB_RESULT) also answers unknown commands of real services. The default refuses with kStubService
    // (the guest sees a failure at the call that has no answer); 0 is a bring-up probe only: an empty "success" hands the guest zeros
    // and no objects, so the crash comes later and far from its cause. Logged at Warn once per (service, command); the end-of-run
    // summary has the counts.
    static const uint32_t answer = static_cast<uint32_t>(config_get_int("ipc", "stub_result", kernel::kStubService));
    if (detail::note_unhandled(*this, req.cmd) == 1)
        NXLOG_W("ipc", "%s cmd %u not implemented (in %zu bytes) -> 0x%x", name_.c_str(), req.cmd, req.in_size, answer);
    return answer;
}

namespace detail {
uint64_t note_unhandled(Service& s, uint32_t cmd) {
    nxrt::gap("ipc", "unhandled command", s.name() + " cmd " + std::to_string(cmd));
    std::lock_guard<std::recursive_mutex> lk(mu());
    s.unhandled[cmd]++;
    ServiceStats& st = stats_for(s.name());
    st.stub = s.is_stub();
    return ++st.unhandled[cmd];
}
}  // namespace detail

// ---- registry -----------------------------------------------------------------------------------------------------------

void register_service(const std::string& name, ServiceFactory f) {
    std::lock_guard<std::recursive_mutex> lk(mu());
    reg().factories[name] = std::move(f);
}

bool has_service(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lk(mu());
    return reg().factories.count(name) != 0;
}

std::shared_ptr<Service> make_service(const std::string& name) {
    ServiceFactory f;
    {
        std::lock_guard<std::recursive_mutex> lk(mu());
        auto it = reg().factories.find(name);
        if (it != reg().factories.end()) f = it->second;
    }
    if (f) {
        if (std::shared_ptr<Service> s = f()) return s;
    }
    bool first = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mu());
        auto& v = reg().stubs_requested;
        if (std::find(v.begin(), v.end(), name) == v.end()) {
            v.push_back(name);
            first = true;
        }
    }
    if (first) NXLOG_W("ipc", "ipc: no implementation for service '%s': using a stub", name.c_str());
    return std::make_shared<StubService>(name);
}

std::vector<std::string> service_names() {
    std::lock_guard<std::recursive_mutex> lk(mu());
    std::vector<std::string> v;
    for (const auto& kv : reg().factories) v.push_back(kv.first);
    return v;
}

std::vector<std::string> stub_names_requested() {
    std::lock_guard<std::recursive_mutex> lk(mu());
    return reg().stubs_requested;
}

void registry_reset() {
    std::lock_guard<std::recursive_mutex> lk(mu());
    Registry& r = reg();
    ServiceFactory sm = r.factories["sm:"];
    r.factories.clear();
    r.factories["sm:"] = sm;
    r.stubs_requested.clear();
    r.stats.clear();
}

// ---- sessions -----------------------------------------------------------------------------------------------------------

KSession::~KSession() {
    std::lock_guard<std::recursive_mutex> lk(mu());
    // The domain dies with this session when nothing else holds it; its objects then die too unless another holder keeps them.
    const bool dom_dies = dom && dom.use_count() == 1;
    if (dom_dies) {
        g_domain_objects.sub(static_cast<int64_t>(dom->objects.size()));
        for (const auto& kv : dom->objects)
            if (kv.second != svc && kv.second.use_count() == 1) kv.second->on_close();
    }
    if (svc) {
        long own = 1;   // this session's reference
        if (dom_dies)
            for (const auto& kv : dom->objects)
                if (kv.second == svc) own++;   // the domain's reference to this service, which dies with it
        if (svc.use_count() == own) svc->on_close();
    }
}

uint32_t create_session_handle(std::shared_ptr<Service> svc) {
    if (!svc) return 0;
    auto s = std::make_shared<KSession>();
    s->svc = std::move(svc);
    return kernel::handle_add(s);
}

uint32_t connect_named_port(const char* name, uint32_t* out_handle) {
    if (!name || !name[0] || !out_handle) return kernel::kInvalidAddress;
    const std::string n(name, strnlen(name, kConnectNameMax));
    std::shared_ptr<Service> svc = n == "sm:" ? make_sm() : make_service(n);
    const uint32_t h = create_session_handle(svc);
    if (!h) return kernel::kOutOfResource;
    *out_handle = h;
    NXLOG_I("ipc", "ConnectToNamedPort(%s) -> handle 0x%x", n.c_str(), h);
    return 0;
}

uint32_t send_sync_request(::Ctx* c, uint32_t session_handle, uint64_t msg_addr, uint64_t msg_size) {
    struct Pending {   // counts this request while it is in flight, whatever way it returns
        Pending() { g_requests_pending.add(1); }
        ~Pending() { g_requests_pending.sub(1); }
    } pending;
    auto s = kernel::handle_as<KSession>(session_handle);
    if (!s) return kernel::kInvalidHandle;
    if (!msg_addr) return kernel::kInvalidAddress;
    const size_t nbytes = static_cast<size_t>(std::min<uint64_t>(msg_size, kMaxMsgBytes)) & ~size_t{3};
    const size_t cap = nbytes / 4;
    uint32_t* words = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(msg_addr));

    hipc::Message m;
    std::string err;
    if (!hipc::parse(words, cap, m, &err)) {
        NXLOG_E("ipc", "SendSyncRequest(0x%x): malformed message (%zu bytes): %s", session_handle, nbytes, err.c_str());
        return kInvalidCombination;
    }

    // Release (2, and 15): the client tells the server it is done with the object; the reply is an empty message. The client handle
    // stays valid: libnx and the SDK follow the message with svcCloseHandle (a handle closed here made that call fail, and the SDK
    // aborts on a failed CloseHandle). The service sees on_close when the handle is closed.
    if (m.type == hipc::kRelease || m.type == 15) {
        words[0] = 0;
        words[1] = 0;
        return 0;
    }

    // Control messages (5, 7).
    if (m.type == hipc::kInvoke2ManagerMethodOld || m.type == hipc::kInvoke2ManagerMethod) {
        if (m.raw.size() < kHeaderBytes) return kInvalidCombination;
        Response rsp;
        const uint32_t result = control(s, m, rsp);
        write_reply(words, cap, Wire::Cmif, result, &rsp, nullptr);
        return 0;
    }

    // TIPC (type >= 16): the command is type - 16, the raw data is the payload.
    if (hipc::is_tipc(m.type)) {
        Request req;
        req.ctx = c;
        req.cmd = m.type - hipc::kTipcBase;
        req.msg_type = m.type;
        req.tipc = true;
        req.in = m.raw.data();
        req.in_size = m.raw.size();
        fill_request_buffers(m, req);
        for (uint32_t h : m.move) {
            auto ks = kernel::handle_as<KSession>(h);
            req.in_services.push_back(ks ? ks->svc : nullptr);
        }
        Response rsp;
        const uint32_t result = run_handler(s->svc, req, rsp);
        consume_moved_handles(m);
        write_reply(words, cap, Wire::Tipc, result, &rsp, nullptr);
        return 0;
    }

    // CMIF requests (1, 3, 4, 6). Anything else is not a request type.
    if (m.type != hipc::kInvokeMethod && m.type != hipc::kInvokeManagerMethod && m.type != hipc::kInvoke2MethodOld &&
        m.type != hipc::kInvoke2Method) {
        NXLOG_E("ipc", "SendSyncRequest(0x%x): unknown message type %u", session_handle, m.type);
        return kInvalidCombination;
    }
    if (m.raw.size() < kHeaderBytes) return kInvalidCombination;

    bool is_domain;
    std::shared_ptr<DomainState> dom;
    {
        std::lock_guard<std::recursive_mutex> lk(mu());
        is_domain = s->domain;
        dom = s->dom;
    }

    if (is_domain) {
        // CmifDomainMessageInHeader: kind, object count, raw size, target object id, reserved, token.
        const uint32_t kind = m.raw[0];
        const size_t n_obj = m.raw[1];
        const size_t in_raw = rd16(m.raw, 2);
        const uint32_t target = rd32(m.raw, 4);
        if (kind == 2) {   // close the object: erased from the domain
            std::shared_ptr<Service> dying;
            {
                std::lock_guard<std::recursive_mutex> lk(mu());
                auto it = dom->objects.find(target);
                if (it != dom->objects.end()) {
                    dying = it->second;
                    dom->objects.erase(it);
                    g_domain_objects.sub(1);
                }
            }
            if (dying && dying.use_count() == 1) dying->on_close();
            Response rsp;
            write_reply(words, cap, Wire::Domain, 0, &rsp, dom);
            return 0;
        }
        if (kind != 1) {
            NXLOG_E("ipc", "domain request kind %u not supported", kind);
            write_reply(words, cap, Wire::Domain, kCmifInvalidObject, nullptr, dom);
            return 0;
        }
        if (in_raw < kHeaderBytes || kHeaderBytes + in_raw + 4 * n_obj > m.raw.size()) return kInvalidCombination;

        Request req;
        req.ctx = c;
        req.msg_type = m.type;
        fill_request_buffers(m, req);
        {
            std::lock_guard<std::recursive_mutex> lk(mu());
            for (size_t i = 0; i < n_obj; ++i) {
                auto it = dom->objects.find(rd32(m.raw, kHeaderBytes + in_raw + 4 * i));
                req.in_services.push_back(it == dom->objects.end() ? nullptr : it->second);
            }
        }
        if (rd32(m.raw, kHeaderBytes) != kSfci) {
            NXLOG_E("ipc", "domain request to object %u: bad SFCI magic 0x%08x", target, rd32(m.raw, kHeaderBytes));
            write_reply(words, cap, Wire::Domain, kCmifInvalidObject, nullptr, dom);
            return 0;
        }
        std::shared_ptr<Service> obj;
        {
            std::lock_guard<std::recursive_mutex> lk(mu());
            auto it = dom->objects.find(target);
            if (it != dom->objects.end()) obj = it->second;
        }
        if (!obj) {
            NXLOG_W("ipc", "service '%s': domain object %u not found", s->svc ? s->svc->name().c_str() : "?", target);
            write_reply(words, cap, Wire::Domain, kCmifInvalidObject, nullptr, dom);
            return 0;
        }
        req.cmd = rd32(m.raw, kHeaderBytes + 8);
        req.in = m.raw.data() + kHeaderBytes + kHeaderBytes;
        req.in_size = in_raw - kHeaderBytes;
        Response rsp;
        const uint32_t result = run_handler(obj, req, rsp);
        consume_moved_handles(m);
        write_reply(words, cap, Wire::Domain, result, &rsp, dom);
        return 0;
    }

    // Plain CMIF request on a session.
    if (rd32(m.raw, 0) != kSfci) {
        NXLOG_E("ipc", "service '%s': bad SFCI magic 0x%08x", s->svc ? s->svc->name().c_str() : "?", rd32(m.raw, 0));
        write_reply(words, cap, Wire::Cmif, kCmifInvalidObject, nullptr, nullptr);
        return 0;
    }
    Request req;
    req.ctx = c;
    req.cmd = rd32(m.raw, 8);
    req.msg_type = m.type;
    req.in = m.raw.data() + kHeaderBytes;
    req.in_size = m.raw.size() - kHeaderBytes;
    fill_request_buffers(m, req);
    for (uint32_t h : m.move) {
        auto ks = kernel::handle_as<KSession>(h);
        req.in_services.push_back(ks ? ks->svc : nullptr);
    }
    Response rsp;
    const uint32_t result = run_handler(s->svc, req, rsp);
    consume_moved_handles(m);
    write_reply(words, cap, Wire::Cmif, result, &rsp, nullptr);
    return 0;
}

// ---- statistics ---------------------------------------------------------------------------------------------------------

std::vector<ServiceStats> service_stats() {
    std::lock_guard<std::recursive_mutex> lk(mu());
    std::vector<ServiceStats> v;
    for (const auto& kv : reg().stats) v.push_back(kv.second);
    return v;
}

std::string ipc_summary() {
    std::string out;
    for (const ServiceStats& st : service_stats()) {
        uint64_t unhandled = 0;
        for (const auto& kv : st.unhandled) unhandled += kv.second;
        char line[256];
        std::snprintf(line, sizeof line, "%s%s: %llu requests, %llu unhandled commands", out.empty() ? "" : "\n", st.name.c_str(),
                      static_cast<unsigned long long>(st.requests), static_cast<unsigned long long>(unhandled));
        out += line;
        if (st.stub) out += " (stub)";
    }
    return out.empty() ? std::string("ipc: no service requests") : out;
}

}  // namespace nxrt::ipc
