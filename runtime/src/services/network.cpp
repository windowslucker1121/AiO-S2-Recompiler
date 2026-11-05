// SPDX-License-Identifier: GPL-3.0-or-later
// Offline network services (S3c): nifm, bsd, bsdcfg, ssl, nim:eca, friend, prepo, pctl, bcat, ldn, nsd, sfdnsres, ntc, ngct, notif.
// The console has no network connection, no friends and no BCAT data. Every command answers what a console in that state gives.
// Adapted from ScarletNX (GPL-3.0-or-later): src/nx/hle/services/network/nifm.swipc, bsd.swipc, ssl.swipc, dns.swipc, ldn.swipc and
//   src/nx/hle/services/misc.swipc (pctl, friend, prepo, bcat, ngct, nsd). Command ids, names and layouts.
// Adapted from Ryujinx (MIT): src/Ryujinx.HLE/HOS/Services/Nifm/*, Sockets/Bsd/IClient.cs, Ssl/ResultCode.cs, Nim/Ntc/*,
//   Notification/*, Ldn/UserServiceCreator/IUserLocalCommunicationService.cs, Pctl/*, Ngct/*, Time/ResultCode.cs. The offline answers
//   and the result codes.
// Switchbrew pages followed for the command tables: Network_Interface_services, Sockets_services, SSL_services, Friend_services,
//   Parental_Control_services. The result codes are module | (description << 9) (services.h).
//
// Each command that is answered logs once at Info with tag "net" (the first use per command). Process-wide state (client ids, the ldn
// state, the ssl context count, the pki ids) lives in st(); network_reset() puts it back to the start-up value.
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "services_internal.h"

namespace nxrt::services {
namespace {

// ---- result codes ---------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kNifmNoConnection = res(110, 300);         // nifm NoInternetConnection (Ryujinx Nifm/ResultCode.cs)
constexpr uint32_t kLdnInvalidState = res(203, 32);           // ldn InvalidState (Ryujinx Ldn/ResultCode.cs)
constexpr uint32_t kLdnDeviceNotAvailable = res(203, 16);     // ldn DeviceNotAvailable
constexpr uint32_t kLdnConnectNotFound = res(203, 65);        // ldn ConnectNotFound
constexpr uint32_t kTimeNetworkNotAvailable = res(116, 1000); // time NetworkTimeNotAvailable (Ryujinx Time/ResultCode.cs)
constexpr uint32_t kTimeTaskCanceled = res(116, 1003);        // time NetworkTimeTaskCanceled
constexpr uint32_t kSslInvalidSocket = res(123, 106);         // ssl InvalidSocket (Ryujinx Ssl/ResultCode.cs)
constexpr uint32_t kSslInvalidOption = res(123, 126);         // ssl InvalidOption
constexpr uint32_t kFsPathNotFound = res(2, 1002);            // fs PathNotFound 2002: the bcat delivery cache is empty
constexpr uint32_t kBsdNetDown = 100;                         // ENETDOWN (Linux errno numbers, as Switchbrew's bsd page says)
constexpr uint32_t kNetdbHostNotFound = 1;                    // HOST_NOT_FOUND
constexpr int32_t kEaiNoName = -2;                            // EAI_NONAME
constexpr uint32_t kLdnStateNone = 0, kLdnStateInitialized = 1;   // nn::ldn::State

// ---- process state -----------------------------------------------------------------------------------------------------------------------
struct NetState {
    std::mutex mu;
    std::set<std::string> seen;          // "<service> <cmd>" already logged
    uint32_t next_client_id = 1;         // nifm ClientId, one per IGeneralService
    uint32_t next_cancel_handle = 1;     // sfdnsres cancel handle
    uint64_t next_pki_id = 1;            // ssl Import*/Register* ids
    uint32_t ldn_state = kLdnStateNone;
    int ssl_contexts = 0;                // live ISslContext objects
};
NetState& st() {
    static NetState s;
    return s;
}

// Logs the first use of a command. One line per (service, command) for the life of the process (network_reset() clears it).
void note(const char* svc, uint32_t id, const char* name) {
    NetState& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.seen.insert(std::string(svc) + " " + std::to_string(id)).second) return;
    }
    NXLOG_I("net", "%s cmd %u %s: answered offline (first use)", svc, id, name);
}

uint32_t alloc_client_id() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.next_client_id++;
}
uint64_t alloc_pki_id() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.next_pki_id++;
}
uint32_t alloc_cancel_handle() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.next_cancel_handle++;
}

// ---- command tables -----------------------------------------------------------------------------------------------------------------------
// The common answer shapes. Rows are the commands whose answer is fixed (no state, no object, no event).
enum class K : uint8_t {
    Ok,       // result 0, no output (accepted and ignored)
    U8,       // result 0, one u8 = v
    U32,      // result 0, one u32 = v
    S32,      // result 0, one s32 = v
    U64,      // result 0, one u64 = v
    Fail,     // result v (no network, no such object ...)
    Zeros,    // result 0, n zero bytes of output
    BufZero,  // result 0, n zero bytes written into the output buffer
    Errno,    // BSD: (i32 ret = -1, u32 errno = ENETDOWN, then n u32 zeros)
    ErrnoOk,  // BSD: (i32 ret = 0, u32 errno = 0)
};
struct Row {
    uint32_t id;
    const char* name;
    K k;
    uint64_t v = 0;   // U8/U32/S32/U64 value, Fail result
    uint32_t n = 0;   // Zeros/BufZero byte count, Errno: extra u32 zeros
};

uint32_t run(const Row& r, ipc::Request& req, ipc::Response& rsp) {
    switch (r.k) {
        case K::Ok: return 0;
        case K::U8: rsp.push(static_cast<uint8_t>(r.v)); return 0;
        case K::U32: rsp.push(static_cast<uint32_t>(r.v)); return 0;
        case K::S32: rsp.push(static_cast<int32_t>(r.v)); return 0;
        case K::U64: rsp.push(static_cast<uint64_t>(r.v)); return 0;
        case K::Fail: return static_cast<uint32_t>(r.v);
        case K::Zeros: rsp.out.insert(rsp.out.end(), r.n, uint8_t{0}); return 0;
        case K::BufZero: {
            std::vector<uint8_t> z(r.n, 0);
            buf_write(req.out_buf(0), z.data(), r.n);
            return 0;
        }
        case K::Errno:
            rsp.push(static_cast<int32_t>(-1));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            for (uint32_t i = 0; i < r.n; ++i) rsp.push(static_cast<uint32_t>(0));
            return 0;
        case K::ErrnoOk:
            rsp.push(static_cast<int32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
    }
    return 0;
}

// One command with its own handler; logs the first use.
void cmd(ipc::Service& s, const char* svc, uint32_t id, const char* name, ipc::Handler h) {
    s.add_cmd(id, name, [svc, id, name, h = std::move(h)](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        note(svc, id, name);
        return h(req, rsp);
    });
}

// Every row of a table becomes a command.
void rows(ipc::Service& s, const char* svc, std::initializer_list<Row> list) {
    for (const Row& r : list) {
        Row row = r;
        cmd(s, svc, row.id, row.name, [row](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return run(row, req, rsp); });
    }
}

// A service object whose release is observed (ssl context and connection counts). on_close runs when the last reference goes.
class Tracked final : public ipc::Service {
public:
    Tracked(const char* name, std::function<void()> released) : ipc::Service(name), released_(std::move(released)) {}
    void on_close() override {
        if (released_) released_();
    }

private:
    std::function<void()> released_;
};

std::shared_ptr<ipc::Service> tracked(const char* name, std::function<void()> released, const std::function<void(ipc::Service&)>& setup) {
    auto s = std::make_shared<Tracked>(name, std::move(released));
    setup(*s);
    return s;
}

// Copy handles belong to the client (HIPC copies them: the guest closes its own afterwards), so there is nothing to close here (debug note 122).
void close_copied(const ipc::Request&) {}

// Event pairs held by the object that hands them out.
struct TwoEvents {
    ServiceEvent a;
    ServiceEvent b;
};

// =============================================================================================================================================
// nifm: nifm:u, nifm:a, nifm:s (IStaticService). Switchbrew "Network Interface services", ScarletNX nifm.swipc, Ryujinx Nifm/.
// No connection: GetCurrentIpAddress and the other address/profile answers return nifm 300 (NoInternetConnection).
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_scan_request() {
    auto ev = std::make_shared<ServiceEvent>();
    return make_iface("nn::nifm::detail::IScanRequest", [ev](ipc::Service& s) {
        rows(s, "nifm", {
            {0, "Submit", K::Ok},
            {1, "IsProcessing", K::U8, 0},
            {2, "GetResult", K::Ok},            // a scan that is over at once and found nothing
            {4, "SetChannels", K::Ok},
            {5, "Cancel", K::Ok},
        });
        cmd(s, "nifm", 3, "GetSystemEventReadableHandle", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_request() {
    auto ev = std::make_shared<TwoEvents>();
    return make_iface("nn::nifm::detail::IRequest", [ev](ipc::Service& s) {
        rows(s, "nifm", {
            {0, "GetRequestState", K::U32, 1},                   // 1 = Free: nothing was accepted (Switchbrew RequestState)
            {1, "GetResult", K::Fail, kNifmNoConnection},
            {3, "Cancel", K::Ok},
            {4, "Submit", K::Ok},
            {5, "SetRequirement", K::Ok},
            {6, "SetRequirementPreset", K::Ok},
            {8, "SetPriority", K::Ok},
            {9, "SetNetworkProfileId", K::Ok},
            {10, "SetRejectable", K::Ok},
            {11, "SetConnectionConfirmationOption", K::Ok},
            {12, "SetPersistent", K::Ok},
            {13, "SetInstant", K::Ok},
            {14, "SetSustainable", K::Ok},
            {15, "SetRawPriority", K::Ok},
            {16, "SetGreedy", K::Ok},
            {17, "SetSharable", K::Ok},
            {18, "SetRequirementByRevision", K::Ok},
            {19, "GetRequirement", K::Zeros, 0, 0x24},           // 0x20 bytes, plus 4 from 3.0.0
            {20, "GetRevision", K::U32, 0},
            {22, "GetAdditionalInfo", K::Ok},
            {23, "SetKeptInSleep", K::Ok},
            {24, "RegisterSocketDescriptor", K::Ok},
            {25, "UnregisterSocketDescriptor", K::Ok},
            {26, "GetNetworkAccessStatus", K::U32, 0},
        });
        cmd(s, "nifm", 2, "GetSystemEventReadableHandles", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->a.new_handle());   // signals when RequestState changes: never does here
            rsp.out_copy.push_back(ev->b.new_handle());
            return 0;
        });
        cmd(s, "nifm", 21, "GetAppletInfo", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            // The error applet (0xE) with no data, as Ryujinx answers it when the request has failed.
            rsp.push(static_cast<uint32_t>(0xE));
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_general() {
    const uint32_t client_id = alloc_client_id();
    auto ev = std::make_shared<ServiceEvent>();
    return make_iface("nn::nifm::detail::IGeneralService", [client_id, ev](ipc::Service& s) {
        cmd(s, "nifm", 1, "GetClientId", [client_id](ipc::Request& req, ipc::Response&) -> uint32_t {
            buf_write(req.out_buf(0), &client_id, sizeof client_id);
            return 0;
        });
        cmd(s, "nifm", 2, "CreateScanRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_scan_request());
            return 0;
        });
        cmd(s, "nifm", 4, "CreateRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_request());
            return 0;
        });
        cmd(s, "nifm", 31, "GetTelemetorySystemEventReadableHandle", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
        rows(s, "nifm", {
            {5, "GetCurrentNetworkProfile", K::Fail, kNifmNoConnection},
            {6, "EnumerateNetworkInterfaces", K::S32, 0},
            {7, "EnumerateNetworkProfiles", K::S32, 0},
            {8, "GetNetworkProfile", K::Fail, kNifmNoConnection},
            {9, "SetNetworkProfile", K::Zeros, 0, 16},          // returns a Uuid (16 zero bytes); the profile is not kept
            {10, "RemoveNetworkProfile", K::Ok},
            {11, "GetScanDataOld", K::S32, 0},
            {12, "GetCurrentIpAddress", K::Fail, kNifmNoConnection},
            {13, "GetCurrentAccessPointOld", K::Fail, kNifmNoConnection},
            {14, "CreateTemporaryNetworkProfile", K::Fail, kNifmNoConnection},
            {15, "GetCurrentIpConfigInfo", K::Fail, kNifmNoConnection},
            {16, "SetWirelessCommunicationEnabled", K::Ok},
            {17, "IsWirelessCommunicationEnabled", K::U8, 1},   // radio on, no access point
            {18, "GetInternetConnectionStatus", K::Fail, kNifmNoConnection},
            {19, "SetEthernetCommunicationEnabled", K::Ok},
            {20, "IsEthernetCommunicationEnabled", K::U8, 0},   // no adapter
            {21, "IsAnyInternetRequestAccepted", K::U8, 0},
            {22, "IsAnyForegroundRequestAccepted", K::U8, 0},
            {23, "PutToSleep", K::Ok},
            {24, "WakeUp", K::Ok},
            {25, "GetSsidListVersion", K::U32, 0},
            {26, "SetExclusiveClient", K::Ok},
            {27, "GetDefaultIpSetting", K::Zeros, 0, 0xD},
            {28, "SetDefaultIpSetting", K::Ok},
            {29, "SetWirelessCommunicationEnabledForTest", K::Ok},
            {30, "SetEthernetCommunicationEnabledForTest", K::Ok},
            {32, "GetTelemetryInfo", K::Ok},
            {33, "ConfirmSystemAvailability", K::Fail, kNifmNoConnection},
            {34, "SetBackgroundRequestEnabled", K::Ok},
            {35, "GetScanData", K::S32, 0},
            {36, "GetCurrentAccessPoint", K::Fail, kNifmNoConnection},
            {37, "Shutdown", K::Ok},
            {38, "GetAllowedChannels", K::Ok},
            {39, "NotifyApplicationSuspended", K::Ok},
            {40, "SetAcceptableNetworkTypeFlag", K::Ok},
            {41, "GetAcceptableNetworkTypeFlag", K::U32, 0},
            {42, "NotifyConnectionStateChanged", K::Ok},
            {43, "SetWowlDelayedWakeTime", K::Ok},
            {44, "IsWiredConnectionAvailable", K::U8, 0},
            {45, "IsNetworkEmulationFeatureEnabled", K::U8, 0},
            {46, "SelectActiveNetworkEmulationProfileIdForDebug", K::Ok},
            {47, "GetScanData", K::S32, 0},
            {48, "ResetActiveNetworkEmulationProfileId", K::Ok},
            {49, "GetActiveNetworkEmulationProfileId", K::U32, 0},
            {50, "IsRewriteFeatureEnabled", K::U8, 0},
            {51, "CreateRewriteRule", K::Ok},
            {52, "DestroyRewriteRule", K::Ok},
            {53, "IsActiveNetworkEmulationProfileIdSelected", K::U8, 0},
            {54, "SelectDefaultNetworkEmulationProfileId", K::Ok},
            {55, "GetDefaultNetworkEmulationProfileId", K::U32, 0},
            {56, "GetNetworkEmulationProfile", K::Fail, kNifmNoConnection},
            {57, "SetWowlTcpKeepAliveTimeout", K::Ok},
            // SDK 22 (Switch 2) versions with larger structures, read from the sdk proxy's vtable (thunk sizes in brackets):
            // 100 = 7 [entries 0xac], 101/102/103/104 = 5/8/9/14 [profile 0x1b4], 127/128 = 27/28 [0xf8], 132 = 32 [0x68c].
            {100, "EnumerateNetworkProfiles", K::S32, 0},
            {101, "GetCurrentNetworkProfile", K::Fail, kNifmNoConnection},
            {102, "GetNetworkProfile", K::Fail, kNifmNoConnection},
            {103, "SetNetworkProfile", K::Zeros, 0, 16},
            {104, "CreateTemporaryNetworkProfile", K::Fail, kNifmNoConnection},
            {127, "GetDefaultIpSetting", K::BufZero, 0, 0xf8},
            {128, "SetDefaultIpSetting", K::Ok},
            {132, "GetTelemetryInfo", K::BufZero, 0, 0x68c},
        });
    });
}

std::shared_ptr<ipc::Service> make_nifm_static() {
    return make_iface("nn::nifm::detail::IStaticService", [](ipc::Service& s) {
        cmd(s, "nifm", 4, "CreateGeneralServiceOld", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_general());
            return 0;
        });
        cmd(s, "nifm", 5, "CreateGeneralService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_general());
            return 0;
        });
    });
}

// =============================================================================================================================================
// bsd: bsd:u, bsd:s, bsd:a (IClient). Switchbrew "Sockets services". Every socket-level command returns -1 with errno ENETDOWN, or the
// success pair where the command is a control call. bsdcfg has an empty command table (ScarletNX bsd.swipc).
// =============================================================================================================================================

// The bsd descriptor table (the Socket / EventFd / Poll commands below). One per process, like the console's bsd service.
struct BsdFds {
    std::mutex mu;
    std::condition_variable cv;                                          // a Write to an eventfd wakes the Polls
    std::unordered_map<int32_t, std::pair<uint64_t, uint32_t>> eventfds;  // fd -> (counter, EventFd flags)
    std::unordered_set<int32_t> sockets;
    int32_t next = 3;
};
BsdFds& bsd_fds() {
    static BsdFds t;
    return t;
}
void bsd_ret(ipc::Response& rsp, int32_t ret, uint32_t err) {   // the (i32 ret, u32 errno) pair every bsd command answers with
    rsp.push(ret);
    rsp.push(err);
}
constexpr uint32_t kBsdEbadf = 9, kBsdEagain = 11, kBsdEinval = 22;   // Linux errno numbers, as kBsdNetDown
constexpr int16_t kPollIn = 0x1, kPollOut = 0x4, kPollNval = 0x20;
constexpr uint32_t kEfdSemaphore = 1;

std::shared_ptr<ipc::Service> make_bsd() {
    return make_iface("nn::socket::sf::IClient", [](ipc::Service& s) {
        cmd(s, "bsd", 0, "RegisterClient", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            close_copied(req);                  // the transfer memory is not used
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        rows(s, "bsd", {
            {1, "StartMonitoring", K::Ok},
            {3, "SocketExempt", K::Errno},
            {4, "Open", K::Errno},
            {5, "Select", K::Errno},
            {7, "Sysctl", K::Errno, 0, 1},
            {8, "Recv", K::Errno},
            {9, "RecvFrom", K::Errno, 0, 1},
            {10, "Send", K::Errno},
            {11, "SendTo", K::Errno},
            {12, "Accept", K::Errno, 0, 1},
            {13, "Bind", K::Errno},
            {14, "Connect", K::Errno},
            {15, "GetPeerName", K::Errno, 0, 1},
            {16, "GetSockName", K::Errno, 0, 1},
            {17, "GetSockOpt", K::Errno, 0, 1},
            {18, "Listen", K::Errno},
            {19, "Ioctl", K::Errno},
            {21, "SetSockOpt", K::Errno},
            {22, "Shutdown", K::Errno},
            {23, "ShutdownAllSockets", K::Errno},
            {27, "DuplicateSocket", K::Errno},
            {28, "GetResourceStatistics", K::Zeros, 0, 8},
            {29, "RecvMMsg", K::Errno},
            {30, "SendMMsg", K::Errno},
            {32, "RegisterResourceStatisticsName", K::U32, 0},
            {33, "RegisterClientShared", K::U32, 0},
            {34, "GetSocketStatistics", K::Errno},
            {35, "NifIoctl", K::Errno},
            {36, "Cmd36", K::U32, 0},            // Switchbrew: 4 bytes out, stubbed to 0 on NX
            {37, "SetSocketBufferAttribute", K::Errno},
            {38, "Cmd38", K::Errno},            // no name in Switchbrew's table
            {39, "NoSfEventFd", K::Errno},
            {40, "ShutdownAllSockets", K::Errno},
            {41, "Cmd41", K::Errno},
            {42, "SendMMsgNoMem", K::Errno},
            {43, "RecvMMsgNoMem", K::Errno},
            {44, "Cmd44", K::Errno},
            {200, "SetThreadCoreMask", K::Ok},
            {201, "GetThreadCoreMask", K::U32, 0},
        });
        // Descriptors (Ocarina of Time: gRPC probes IPv6 with a UDP socket, then makes its poll engine's wakeup an EventFd, and aborts when
        // it cannot). Sockets exist and do nothing (no network: their other commands keep failing with ENETDOWN, Poll never reports them
        // ready). An eventfd is Linux's: Write adds the 8-byte value, Read returns the counter and clears it (EFD_SEMAPHORE: 1, minus 1),
        // and an empty one answers EAGAIN (gRPC reads only after Poll said POLLIN). Poll waits up to its timeout for a Write.
        cmd(s, "bsd", 2, "Socket", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            BsdFds& t = bsd_fds();
            std::lock_guard lk(t.mu);
            const int32_t fd = t.next++;
            t.sockets.insert(fd);
            bsd_ret(rsp, fd, 0);
            return 0;
        });
        cmd(s, "bsd", 31, "EventFd", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            BsdFds& t = bsd_fds();
            std::lock_guard lk(t.mu);
            const int32_t fd = t.next++;
            t.eventfds[fd] = {req.arg<uint64_t>(0), req.arg<uint32_t>(8)};
            bsd_ret(rsp, fd, 0);
            return 0;
        });
        cmd(s, "bsd", 26, "Close", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            BsdFds& t = bsd_fds();
            std::lock_guard lk(t.mu);
            const int32_t fd = req.arg<int32_t>(0);
            const bool known = t.eventfds.erase(fd) + t.sockets.erase(fd) > 0;
            bsd_ret(rsp, known ? 0 : -1, known ? 0 : kBsdEbadf);
            return 0;
        });
        cmd(s, "bsd", 25, "Read", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            BsdFds& t = bsd_fds();
            std::lock_guard lk(t.mu);
            auto it = t.eventfds.find(req.arg<int32_t>(0));
            const ipc::Buffer out = req.out_buf(0);
            if (it == t.eventfds.end()) { bsd_ret(rsp, -1, kBsdNetDown); return 0; }
            if (it->second.first == 0) { bsd_ret(rsp, -1, kBsdEagain); return 0; }
            if (out.size < 8) { bsd_ret(rsp, -1, kBsdEinval); return 0; }
            const uint64_t v = (it->second.second & kEfdSemaphore) ? 1 : it->second.first;
            it->second.first -= v;
            std::memcpy(reinterpret_cast<void*>(out.addr), &v, 8);
            bsd_ret(rsp, 8, 0);
            return 0;
        });
        cmd(s, "bsd", 24, "Write", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            BsdFds& t = bsd_fds();
            std::lock_guard lk(t.mu);
            auto it = t.eventfds.find(req.arg<int32_t>(0));
            const ipc::Buffer in = req.in_buf(0);
            if (it == t.eventfds.end()) { bsd_ret(rsp, -1, kBsdNetDown); return 0; }
            if (in.size < 8) { bsd_ret(rsp, -1, kBsdEinval); return 0; }
            uint64_t v;
            std::memcpy(&v, reinterpret_cast<const void*>(in.addr), 8);
            it->second.first += v;
            t.cv.notify_all();
            bsd_ret(rsp, 8, 0);
            return 0;
        });
        cmd(s, "bsd", 20, "Fcntl", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            BsdFds& t = bsd_fds();
            std::lock_guard lk(t.mu);
            const int32_t fd = req.arg<int32_t>(0);
            const bool known = t.eventfds.count(fd) || t.sockets.count(fd);
            // F_GETFL (3) reports O_NONBLOCK (4): nothing here ever blocks except Poll; F_SETFL and the rest are accepted
            bsd_ret(rsp, known ? (req.arg<int32_t>(4) == 3 ? 4 : 0) : -1, known ? 0 : kBsdEbadf);
            return 0;
        });
        cmd(s, "bsd", 6, "Poll", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t n = req.arg<uint32_t>(0);
            const int32_t timeout_ms = req.arg<int32_t>(4);
            const ipc::Buffer in = req.in_buf(0), out = req.out_buf(0);
            if (n > 4096 || in.size < n * 8ull) { bsd_ret(rsp, -1, kBsdEinval); return 0; }
            std::vector<uint8_t> fds(n * 8);
            if (n) std::memcpy(fds.data(), reinterpret_cast<const void*>(in.addr), fds.size());
            BsdFds& t = bsd_fds();
            std::unique_lock lk(t.mu);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
            int32_t ready = 0;
            for (;;) {
                ready = 0;
                for (uint32_t i = 0; i < n; i++) {
                    int32_t fd;
                    int16_t events, revents = 0;
                    std::memcpy(&fd, &fds[i * 8], 4);
                    std::memcpy(&events, &fds[i * 8 + 4], 2);
                    auto it = t.eventfds.find(fd);
                    if (it != t.eventfds.end()) revents = static_cast<int16_t>(((events & kPollIn) && it->second.first ? kPollIn : 0) | (events & kPollOut));
                    else if (fd >= 0 && !t.sockets.count(fd)) revents = kPollNval;
                    std::memcpy(&fds[i * 8 + 6], &revents, 2);
                    ready += revents != 0;
                }
                if (ready || timeout_ms == 0) break;
                if (timeout_ms < 0) t.cv.wait(lk);
                else if (t.cv.wait_until(lk, deadline) == std::cv_status::timeout) break;
            }
            if (out.addr && out.size >= fds.size() && n) std::memcpy(reinterpret_cast<void*>(out.addr), fds.data(), fds.size());
            bsd_ret(rsp, ready, 0);
            return 0;
        });
    });
}

// =============================================================================================================================================
// ssl: ssl (ISslService). Switchbrew "SSL services". CreateContext answers. Connection calls fail with ssl InvalidSocket: there is no
// socket to run TLS over. Local settings (options, session cache mode, ALPN, verify option) are accepted.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_ssl_connection(const std::shared_ptr<int>& conns);

std::shared_ptr<ipc::Service> make_ssl_context() {
    {
        NetState& s = st();
        std::lock_guard<std::mutex> lk(s.mu);
        s.ssl_contexts++;
    }
    auto conns = std::make_shared<int>(0);     // live connections of this context
    return tracked(
        "nn::ssl::sf::ISslContext",
        [] {
            NetState& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (s.ssl_contexts > 0) s.ssl_contexts--;
        },
        [conns](ipc::Service& svc) {
            rows(svc, "ssl", {
                {0, "SetOption", K::Ok},
                {1, "GetOption", K::S32, 0},
                {6, "RemoveServerPki", K::Ok},
                {7, "RemoveClientPki", K::Ok},
                {9, "AddPolicyOid", K::Ok},
                {11, "RemoveCrl", K::Ok},
                {13, "GeneratePrivateKeyAndCert", K::Fail, kSslInvalidOption},
            });
            cmd(svc, "ssl", 2, "CreateConnection", [conns](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push_object(make_ssl_connection(conns));
                return 0;
            });
            cmd(svc, "ssl", 3, "GetConnectionCount", [conns](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push(static_cast<uint32_t>(*conns));
                return 0;
            });
            for (uint32_t id : {4u, 5u, 8u, 10u, 12u}) {
                const char* name = id == 4 ? "ImportServerPki" : id == 5 ? "ImportClientPki" : id == 8 ? "RegisterInternalPki"
                                 : id == 10 ? "ImportCrl" : "ImportClientCertKeyPki";
                cmd(svc, "ssl", id, name, [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                    rsp.push(alloc_pki_id());
                    return 0;
                });
            }
        });
}

std::shared_ptr<ipc::Service> make_ssl_connection(const std::shared_ptr<int>& conns) {
    ++(*conns);
    std::shared_ptr<int> c = conns;
    return tracked(
        "nn::ssl::sf::ISslConnection",
        [c] { --(*c); },
        [](ipc::Service& svc) {
            rows(svc, "ssl", {
                {0, "SetSocketDescriptor", K::Fail, kSslInvalidSocket},
                {1, "SetHostName", K::Ok},
                {2, "SetVerifyOption", K::Ok},
                {3, "SetIoMode", K::Ok},
                {4, "GetSocketDescriptor", K::S32, 0xFFFFFFFFu},   // -1: none was set
                {5, "GetHostName", K::U32, 0},
                {6, "GetVerifyOption", K::U32, 0},
                {7, "GetIoMode", K::U32, 0},
                {8, "DoHandshake", K::Fail, kSslInvalidSocket},
                {9, "DoHandshakeGetServerCert", K::Fail, kSslInvalidSocket},
                {10, "Read", K::Fail, kSslInvalidSocket},
                {11, "Write", K::Fail, kSslInvalidSocket},
                {12, "Pending", K::S32, 0},
                {13, "Peek", K::Fail, kSslInvalidSocket},
                {14, "Poll", K::U32, 0},
                {15, "GetVerifyCertError", K::U32, 0},
                {16, "GetNeededServerCertBufferSize", K::U32, 0},
                {17, "SetSessionCacheMode", K::Ok},
                {18, "GetSessionCacheMode", K::U32, 0},
                {19, "FlushSessionCache", K::Ok},
                {20, "SetRenegotiationMode", K::Ok},
                {21, "GetRenegotiationMode", K::U32, 0},
                {22, "SetOption", K::Ok},
                {23, "GetOption", K::U8, 0},
                {24, "GetVerifyCertErrors", K::Ok},
                {25, "GetCipherInfo", K::Fail, kSslInvalidSocket},
                {26, "SetNextAlpnProto", K::Ok},
                {27, "GetNextAlpnProto", K::Zeros, 0, 8},          // (u32 state, u32 size) = (0, 0)
                {28, "SetDtlsSocketDescriptor", K::Fail, kSslInvalidSocket},
                {29, "GetDtlsHandshakeTimeout", K::U64, 0},
                {30, "SetPrivateOption", K::Ok},
                {31, "SetSrtpCiphers", K::Ok},
                {32, "GetSrtpCipher", K::U32, 0},
                {33, "ExportKeyingMaterial", K::Fail, kSslInvalidSocket},
                {34, "SetIoTimeout", K::Ok},
                {35, "GetIoTimeout", K::U64, 0},
                {36, "GetSessionTicket", K::U32, 0},
                {37, "SetSessionTicket", K::Ok},
            });
        });
}

std::shared_ptr<ipc::Service> make_ssl_service() {
    return make_iface("nn::ssl::sf::ISslService", [](ipc::Service& s) {
        cmd(s, "ssl", 0, "CreateContext", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_ssl_context());
            return 0;
        });
        cmd(s, "ssl", 1, "GetContextCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            NetState& n = st();
            std::lock_guard<std::mutex> lk(n.mu);
            rsp.push(static_cast<uint32_t>(n.ssl_contexts));
            return 0;
        });
        rows(s, "ssl", {
            {2, "GetCertificates", K::U32, 0},
            {3, "GetCertificateBufSize", K::U32, 0},
            {4, "DebugIoctl", K::Fail, kSslInvalidOption},
            {5, "SetInterfaceVersion", K::Ok},
            {6, "FlushSessionCache", K::U32, 0},
            {7, "SetDebugOption", K::Ok},
            {8, "GetDebugOption", K::BufZero, 0, 1},
            {9, "ClearTls12FallbackFlag", K::Ok},
            {10, "GetCertificateByIndex", K::Ok},
            {11, "GetTrustedCertificateCount", K::U32, 0},
        });
    });
}

// =============================================================================================================================================
// nim:eca: the shop (ECA) interface. Creation succeeds; the shop objects answer their creation commands, nothing needs the network.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_shop_async() {
    return make_iface("nn::ec::IShopServiceAsync", [](ipc::Service&) {});   // no command table is read yet: any command is unhandled
}

std::shared_ptr<ipc::Service> make_shop_accessor() {
    return make_iface("nn::ec::IShopServiceAccessor", [](ipc::Service& s) {
        cmd(s, "nim:eca", 0, "CreateAsyncInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_shop_async());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_shop_server() {
    return make_iface("nn::ec::IShopServiceAccessServer", [](ipc::Service& s) {
        cmd(s, "nim:eca", 0, "CreateAccessorInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_shop_accessor());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_nim_eca() {
    return make_iface("nn::nim::detail::IShopServiceAccessServerInterface", [](ipc::Service& s) {
        auto create = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            close_copied(req);
            rsp.push_object(make_shop_server());
            return 0;
        };
        cmd(s, "nim:eca", 0, "CreateServerInterface", create);
        rows(s, "nim:eca", {
            {4, "IsLargeResourceAvailable", K::U8, 0},
        });
        cmd(s, "nim:eca", 5, "CreateServerInterface2", create);
    });
}

// =============================================================================================================================================
// friend: friend:u, friend:v, friend:m, friend:s, friend:a (IServiceCreator). Switchbrew "Friend services". No friends, no network: the
// list commands answer count 0, the network-sending commands are accepted and do nothing (see the report).
// =============================================================================================================================================

struct FriendEvents {
    ServiceEvent completion{true};   // nothing is pending, so the completion is at once
};

std::shared_ptr<ipc::Service> make_friend_service() {
    auto ev = std::make_shared<FriendEvents>();
    return make_iface("nn::friends::detail::ipc::IFriendService", [ev](ipc::Service& s) {
        cmd(s, "friend", 0, "GetCompletionEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->completion.new_handle());
            return 0;
        });
        rows(s, "friend", {
            {1, "Cancel", K::Ok},
            {10100, "GetFriendListIds", K::S32, 0},
            {10101, "GetFriendList", K::S32, 0},
            {10102, "UpdateFriendInfo", K::Ok},
            {10110, "GetFriendProfileImage", K::Ok},
            {10111, "GetFriendProfileImageWithImageSize", K::Ok},
            {10120, "CheckFriendListAvailability", K::U8, 1},
            {10121, "EnsureFriendListAvailable", K::Ok},
            {10200, "SendFriendRequestForApplication", K::Ok},
            {10211, "AddFacedFriendRequestForApplication", K::Ok},
            {10400, "GetBlockedUserListIds", K::S32, 0},
            {10420, "CheckBlockedUserListAvailability", K::U8, 1},
            {10421, "EnsureBlockedUserListAvailable", K::Ok},
            {10500, "GetProfileListLegacyV1", K::S32, 0},
            {10501, "GetProfileListLegacyV2", K::S32, 0},
            {10502, "GetProfileListV1", K::S32, 0},
            {10503, "GetProfileListV2", K::S32, 0},
            {10600, "DeclareOpenOnlinePlaySession", K::Ok},
            {10601, "DeclareCloseOnlinePlaySession", K::Ok},
            {10610, "UpdateUserPresence", K::Ok},
            {10700, "GetPlayHistoryRegistrationKey", K::Ok},
            {10701, "GetPlayHistoryRegistrationKeyWithNetworkServiceAccountId", K::Ok},
            {10702, "AddPlayHistory", K::Ok},
            {11000, "GetProfileImageUrl", K::Ok},
            {11001, "GetProfileImageUrlV2", K::Ok},
            {20100, "GetFriendCount", K::S32, 0},
            {20101, "GetNewlyFriendCount", K::S32, 0},
            {20102, "GetFriendDetailedInfo", K::Ok},
            {20103, "SyncFriendList", K::Ok},
            {20104, "RequestSyncFriendList", K::Ok},
            {20105, "GetFriendListForViewerV1", K::S32, 0},
            {20106, "UpdateFriendInfoForViewerV1", K::Ok},
            {20107, "GetFriendDetailedInfoV2", K::Ok},
            {20108, "GetFriendListForViewerV2", K::S32, 0},
            {20109, "UpdateFriendInfoForViewerV2", K::Ok},
            {20110, "LoadFriendSettingV1", K::Ok},
            {20111, "LoadFriendSettingV2", K::Ok},
            {20200, "GetReceivedFriendRequestCount", K::S32, 0},
            {20201, "GetFriendRequestListV1", K::S32, 0},
            {20202, "GetFriendRequestListV2", K::S32, 0},
            {20203, "GetFriendRequestReceivedNotificationCount", K::S32, 0},
            {20300, "GetFriendCandidateList", K::S32, 0},
            {20301, "GetNintendoNetworkIdInfo", K::Ok},
            {20400, "GetBlockedUserListV1", K::S32, 0},
            {20401, "SyncBlockedUserList", K::Ok},
            {20402, "GetBlockedUserListV2", K::S32, 0},
            {20500, "GetProfileExtraListV1", K::S32, 0},
            {20501, "GetRelationship", K::Ok},
            {20502, "GetProfileExtraListV2", K::S32, 0},
            {20600, "GetUserPresenceViewV1", K::Ok},
            {20601, "GetUserPresenceViewV2", K::Ok},
            {20700, "GetPlayHistoryListV1", K::S32, 0},
            {20701, "GetPlayHistoryStatistics", K::Ok},
            {20702, "GetPlayHistoryListV2", K::S32, 0},
            {20800, "LoadUserSettingV1", K::Ok},
            {20801, "SyncUserSetting", K::Ok},
            {20802, "LoadUserSettingV2", K::Ok},
            {20900, "RequestListSummaryOverlayNotification", K::Ok},
            {21000, "GetExternalApplicationCatalog", K::Ok},
            {22000, "GetReceivedFriendInvitationListV1", K::S32, 0},
            {22001, "GetReceivedFriendInvitationDetailedInfoV1", K::Ok},
            {22002, "GetReceivedFriendInvitationListV2", K::S32, 0},
            {22003, "GetReceivedFriendInvitationDetailedInfoV2", K::Ok},
            {22010, "GetReceivedFriendInvitationCountCache", K::S32, 0},
            {30100, "DropFriendNewlyFlags", K::Ok},
            {30101, "DeleteFriend", K::Ok},
            {30110, "DropFriendNewlyFlag", K::Ok},
            {30120, "ChangeFriendFavoriteFlag", K::Ok},
            {30121, "ChangeFriendOnlineNotificationFlag", K::Ok},
            {30130, "SetFriendNote", K::Ok},
            {30131, "RequestUploadPendingNote", K::Ok},
            {30190, "RequestSyncLocalUpdates", K::Ok},
            {30200, "SendFriendRequest", K::Ok},
            {30201, "SendFriendRequestWithApplicationInfoV1", K::Ok},
            {30202, "CancelFriendRequest", K::Ok},
            {30203, "AcceptFriendRequest", K::Ok},
            {30204, "RejectFriendRequest", K::Ok},
            {30205, "ReadFriendRequest", K::Ok},
            {30210, "GetFacedFriendRequestRegistrationKey", K::Ok},
            {30211, "AddFacedFriendRequest", K::Ok},
            {30212, "CancelFacedFriendRequest", K::Ok},
            {30213, "GetFacedFriendRequestProfileImage", K::Ok},
            {30214, "GetFacedFriendRequestProfileImageFromPath", K::Ok},
            {30215, "SendFriendRequestWithExternalApplicationCatalogId", K::Ok},
            {30216, "ResendFacedFriendRequest", K::Ok},
            {30217, "SendFriendRequestWithNintendoNetworkIdInfo", K::Ok},
            {30218, "SendFriendRequestWithApplicationInfoV2", K::Ok},
            {30300, "GetSnsAccountLinkPageUrl", K::Ok},
            {30301, "UnlinkSnsAccount", K::Ok},
            {30400, "BlockUser", K::Ok},
            {30401, "BlockUserWithApplicationInfoV1", K::Ok},
            {30402, "UnblockUser", K::Ok},
            {30403, "BlockUserWithApplicationInfoV2", K::Ok},
            {30500, "GetProfileExtraFromFriendCodeV1", K::Ok},
            {30501, "GetProfileExtraFromFriendCodeV2", K::Ok},
            {30700, "DeletePlayHistory", K::Ok},
            {30701, "AddPlayHistoryWithApplication", K::Ok},
            {30810, "ChangePresencePermission", K::Ok},
            {30811, "ChangeFriendRequestReception", K::Ok},
            {30812, "ChangePlayLogPermission", K::Ok},
            {30820, "IssueFriendCode", K::Ok},
            {30830, "ClearPlayLog", K::Ok},
            {30900, "SendFriendInvitationV1", K::Ok},
            {30901, "SendFriendInvitationV2", K::Ok},
            {30910, "ReadFriendInvitation", K::Ok},
            {30911, "ReadAllFriendInvitations", K::Ok},
            {31000, "OpenUser", K::Ok},
            {31001, "OpenUserWithPlayLogPolicy", K::Ok},
            {40100, "DeleteFriendListCache", K::Ok},
            {40400, "DeleteBlockedUserListCache", K::Ok},
            {49900, "DeleteNetworkServiceAccountCache", K::Ok},
        });
    });
}

std::shared_ptr<ipc::Service> make_friend_notification() {
    auto ev = std::make_shared<ServiceEvent>();   // never signalled: no notifications
    return make_iface("nn::friends::detail::ipc::INotificationService", [ev](ipc::Service& s) {
        cmd(s, "friend", 0, "GetEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
        rows(s, "friend", {
            {1, "Clear", K::Ok},
            {2, "Pop", K::Zeros, 0, 16},          // SizedNotificationInfo, all zero: nothing is queued
        });
    });
}

std::shared_ptr<ipc::Service> make_friend_creator() {
    return make_iface("nn::friends::detail::ipc::IServiceCreator", [](ipc::Service& s) {
        cmd(s, "friend", 0, "CreateFriendService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_friend_service());
            return 0;
        });
        cmd(s, "friend", 1, "CreateNotificationService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_friend_notification());
            return 0;
        });
        cmd(s, "friend", 2, "CreateDaemonSuspendSessionService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("nn::friends::detail::ipc::IDaemonSuspendSessionService", [](ipc::Service&) {}));
            return 0;
        });
    });
}

// =============================================================================================================================================
// prepo: prepo:u, prepo:a, prepo:a2 (IPrepoService). Play reports are accepted and dropped. ScarletNX misc.swipc lists 10105, 10200, 10400.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_prepo() {
    return make_iface("nn::prepo::detail::ipc::IPrepoService", [](ipc::Service& s) {
        rows(s, "prepo", {
            {10105, "SaveReportWithUser", K::Ok},
            {10200, "RequestImmediateTransmission", K::Ok},
            {10400, "GetSystemSessionId", K::U64, 0},
        });
    });
}

// =============================================================================================================================================
// pctl: pctl, pctl:s, pctl:r, pctl:a (IParentalControlServiceFactory). No restriction is set: everything answers "allowed".
// =============================================================================================================================================

struct PctlEvents {
    ServiceEvent pin_changed;
    ServiceEvent sync;
    ServiceEvent play_timer;
    ServiceEvent unlinked;
};

std::shared_ptr<ipc::Service> make_pctl_service() {
    auto ev = std::make_shared<PctlEvents>();
    return make_iface("nn::pctl::detail::ipc::IParentalControlService", [ev](ipc::Service& s) {
        rows(s, "pctl", {
            {1, "Initialize", K::Ok},
            {1001, "CheckFreeCommunicationPermission", K::Ok},
            {1002, "ConfirmLaunchApplicationPermission", K::Ok},
            {1003, "ConfirmResumeApplicationPermission", K::Ok},
            {1004, "ConfirmSnsPostPermission", K::Ok},
            {1005, "ConfirmSystemSettingsPermission", K::Ok},
            {1006, "IsRestrictionTemporaryUnlocked", K::U8, 0},
            {1007, "RevertRestrictionTemporaryUnlocked", K::Ok},
            {1008, "EnterRestrictedSystemSettings", K::Ok},
            {1009, "LeaveRestrictedSystemSettings", K::Ok},
            {1010, "IsRestrictedSystemSettingsEntered", K::U8, 0},
            {1011, "RevertRestrictedSystemSettingsEntered", K::Ok},
            {1012, "GetRestrictedFeatures", K::U32, 0},
            {1013, "ConfirmStereoVisionPermission", K::Ok},
            {1014, "ConfirmPlayableApplicationVideoOld", K::Ok},
            {1015, "ConfirmPlayableApplicationVideo", K::Ok},
            {1016, "ConfirmShowNewsPermission", K::Ok},
            {1017, "EndFreeCommunication", K::Ok},
            {1018, "IsFreeCommunicationAvailable", K::U8, 1},
            {1019, "ConfirmLaunchApplicationPermission", K::Ok},
            {1020, "ConfirmLaunchSharedApplicationPermission", K::Ok},
            {1021, "TryBeginFreeCommunicationForStreamPlay", K::Ok},
            {1022, "EndFreeCommunicationForStreamPlay", K::Ok},
            {1023, "GetRestrictedFeatureFlagSet", K::U32, 0},
            {1031, "IsRestrictionEnabled", K::U8, 0},
            {1032, "GetSafetyLevel", K::S32, 0},
            {1033, "SetSafetyLevel", K::Ok},
            {1034, "GetSafetyLevelSettings", K::Ok},
            {1035, "GetCurrentSettings", K::Ok},
            {1036, "SetCustomSafetyLevelSettings", K::Ok},
            {1037, "GetDefaultRatingOrganization", K::U32, 0},
            {1038, "SetDefaultRatingOrganization", K::Ok},
            {1039, "GetFreeCommunicationApplicationListCount", K::S32, 0},
            {1042, "AddToFreeCommunicationApplicationList", K::Ok},
            {1043, "DeleteSettings", K::Ok},
            {1044, "GetFreeCommunicationApplicationList", K::S32, 0},
            {1045, "UpdateFreeCommunicationApplicationList", K::Ok},
            {1046, "DisableFeaturesForReset", K::Ok},
            {1047, "NotifyApplicationDownloadStartedOld", K::Ok},
            {1048, "NotifyNetworkProfileCreated", K::Ok},
            {1049, "ResetFreeCommunicationApplicationList", K::Ok},
            {1050, "AddToFreeCommunicationApplicationList", K::Ok},
            {1051, "NotifyApplicationDownloadStarted", K::Ok},
            {1061, "ConfirmStereoVisionRestrictionConfigurable", K::Ok},
            {1062, "GetStereoVisionRestriction", K::U8, 0},
            {1063, "SetStereoVisionRestriction", K::Ok},
            {1064, "ResetConfirmedStereoVisionPermission", K::Ok},
            {1065, "IsStereoVisionPermitted", K::U8, 1},
            {1201, "UnlockRestrictionTemporarily", K::Ok},
            {1202, "UnlockSystemSettingsRestriction", K::Ok},
            {1203, "SetPinCode", K::Ok},
            {1204, "GenerateInquiryCode", K::Ok},
            {1205, "CheckMasterKey", K::Ok},
            {1206, "GetPinCodeLength", K::Ok},
            {1403, "IsPairingActive", K::U8, 0},
            {1453, "IsPlayTimerEnabled", K::U8, 0},
        });
        cmd(s, "pctl", 1207, "GetPinCodeChangedEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->pin_changed.new_handle());
            return 0;
        });
        cmd(s, "pctl", 1432, "GetSynchronizationEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->sync.new_handle());
            return 0;
        });
        cmd(s, "pctl", 1457, "GetPlayTimerEventToRequestSuspension", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->play_timer.new_handle());
            return 0;
        });
        cmd(s, "pctl", 1473, "GetUnlinkedEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->unlinked.new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_pctl_factory() {
    return make_iface("nn::pctl::detail::ipc::IParentalControlServiceFactory", [](ipc::Service& s) {
        auto create = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_pctl_service());
            return 0;
        };
        cmd(s, "pctl", 0, "CreateService", create);
        cmd(s, "pctl", 1, "CreateServiceWithoutInitialize", create);
    });
}

// =============================================================================================================================================
// bcat: bcat:u, bcat:a (IServiceCreator). No BCAT data for this title and no network: the sync requests finish at once with an empty cache.
// =============================================================================================================================================

struct ProgressEvents {
    ServiceEvent done{true};   // the sync is over
};

std::shared_ptr<ipc::Service> make_bcat_progress() {
    auto ev = std::make_shared<ProgressEvents>();
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheProgressService", [ev](ipc::Service& s) {
        cmd(s, "bcat", 0, "GetEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->done.new_handle());
            return 0;
        });
        rows(s, "bcat", {
            {1, "GetImpl", K::BufZero, 0, 0x200},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_notifier() {
    auto ev = std::make_shared<ServiceEvent>();   // the storage is never updated
    return make_iface("nn::bcat::detail::ipc::INotifierService", [ev](ipc::Service& s) {
        cmd(s, "bcat", 0, "GetEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_file() {
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheFileService", [](ipc::Service& s) {
        rows(s, "bcat", {
            {0, "Open", K::Fail, kFsPathNotFound},
            {1, "Read", K::U64, 0},
            {2, "GetSize", K::U64, 0},
            {3, "GetDigest", K::Zeros, 0, 0x10},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_directory() {
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheDirectoryService", [](ipc::Service& s) {
        rows(s, "bcat", {
            {0, "Open", K::Fail, kFsPathNotFound},
            {1, "Read", K::S32, 0},
            {2, "GetCount", K::S32, 0},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_storage() {
    return make_iface("nn::bcat::detail::ipc::IDeliveryCacheStorageService", [](ipc::Service& s) {
        cmd(s, "bcat", 0, "CreateFileService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_file());
            return 0;
        });
        cmd(s, "bcat", 1, "CreateDirectoryService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_directory());
            return 0;
        });
        rows(s, "bcat", {
            {10, "EnumerateDeliveryCacheDirectory", K::S32, 0},
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_service() {
    auto progress = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_bcat_progress());
        return 0;
    };
    return make_iface("nn::bcat::detail::ipc::IBcatService", [progress](ipc::Service& s) {
        cmd(s, "bcat", 10100, "RequestSyncDeliveryCache", progress);
        cmd(s, "bcat", 10101, "RequestSyncDeliveryCacheWithDirectoryName", progress);
        cmd(s, "bcat", 20100, "RequestSyncDeliveryCacheWithApplicationId", progress);
        cmd(s, "bcat", 20101, "RequestSyncDeliveryCacheWithApplicationIdAndDirectoryName", progress);
        cmd(s, "bcat", 20300, "GetDeliveryCacheStorageUpdateNotifier", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_notifier());
            return 0;
        });
        cmd(s, "bcat", 20301, "RequestSuspendDeliveryTask", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("nn::bcat::detail::ipc::IDeliveryTaskSuspensionService", [](ipc::Service&) {}));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_bcat_creator() {
    return make_iface("nn::bcat::detail::ipc::IServiceCreator", [](ipc::Service& s) {
        auto bcat = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_service());
            return 0;
        };
        auto storage = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_bcat_storage());
            return 0;
        };
        cmd(s, "bcat", 0, "CreateBcatService", bcat);
        cmd(s, "bcat", 1, "CreateDeliveryCacheStorageService", storage);
        cmd(s, "bcat", 2, "CreateDeliveryCacheStorageServiceWithApplicationId", storage);
    });
}

// =============================================================================================================================================
// ldn: ldn:u (IUserServiceCreator). Local wireless: the state machine starts at None, Initialize moves it to Initialized. The scans find
// nothing; anything that needs a network fails with the ldn result a console without a radio gives.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_ldn_local() {
    auto ev = std::make_shared<ServiceEvent>();   // the state never changes on its own
    return make_iface("nn::ldn::detail::IUserLocalCommunicationService", [ev](ipc::Service& s) {
        cmd(s, "ldn", 0, "GetState", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            NetState& n = st();
            std::lock_guard<std::mutex> lk(n.mu);
            rsp.push(static_cast<uint32_t>(n.ldn_state));
            return 0;
        });
        cmd(s, "ldn", 100, "AttachStateChangeEvent", [ev](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(ev->new_handle());
            return 0;
        });
        auto create = [](ipc::Request&, ipc::Response&) -> uint32_t {
            NetState& n = st();
            std::lock_guard<std::mutex> lk(n.mu);
            return n.ldn_state == kLdnStateNone ? kLdnInvalidState : kLdnDeviceNotAvailable;
        };
        cmd(s, "ldn", 202, "CreateNetwork", create);
        cmd(s, "ldn", 203, "CreateNetworkPrivate", create);
        auto init = [](ipc::Request&, ipc::Response&) -> uint32_t {
            NetState& n = st();
            std::lock_guard<std::mutex> lk(n.mu);
            n.ldn_state = kLdnStateInitialized;
            return 0;
        };
        cmd(s, "ldn", 400, "Initialize", init);
        cmd(s, "ldn", 402, "InitializeWithVersion", init);
        cmd(s, "ldn", 401, "Finalize", [](ipc::Request&, ipc::Response&) -> uint32_t {
            NetState& n = st();
            std::lock_guard<std::mutex> lk(n.mu);
            n.ldn_state = kLdnStateNone;
            return 0;
        });
        rows(s, "ldn", {
            {1, "GetNetworkInfo", K::Fail, kLdnInvalidState},
            {2, "GetIpv4Address", K::Fail, kLdnInvalidState},
            {3, "GetDisconnectReason", K::Zeros, 0, 2},
            {4, "GetSecurityParameter", K::Zeros, 0, 0x20},
            {5, "GetNetworkConfig", K::Zeros, 0, 0x20},
            {101, "GetNetworkInfoLatestUpdate", K::Fail, kLdnInvalidState},
            {102, "Scan", K::Zeros, 0, 2},          // count 0
            {103, "ScanPrivate", K::Zeros, 0, 2},   // count 0
            {104, "SetWirelessControllerRestriction", K::Ok},
            {106, "SetProtocol", K::Ok},
            {200, "OpenAccessPoint", K::Ok},
            {201, "CloseAccessPoint", K::Ok},
            {204, "DestroyNetwork", K::Fail, kLdnInvalidState},
            {205, "Reject", K::Fail, kLdnInvalidState},
            {206, "SetAdvertiseData", K::Ok},
            {207, "SetStationAcceptPolicy", K::Ok},
            {208, "AddAcceptFilterEntry", K::Ok},
            {209, "ClearAcceptFilter", K::Ok},
            {300, "OpenStation", K::Ok},
            {301, "CloseStation", K::Ok},
            {302, "Connect", K::Fail, kLdnConnectNotFound},
            {303, "ConnectPrivate", K::Fail, kLdnConnectNotFound},
            {304, "Disconnect", K::Ok},
        });
    });
}

std::shared_ptr<ipc::Service> make_ldn_user_creator() {
    return make_iface("nn::ldn::detail::IUserServiceCreator", [](ipc::Service& s) {
        cmd(s, "ldn", 0, "CreateUserLocalCommunicationService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_ldn_local());
            return 0;
        });
        cmd(s, "ldn", 1, "CreateClientProcessMonitor", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("nn::ldn::detail::IClientProcessMonitor", [](ipc::Service& m) {
                rows(m, "ldn", {{0, "RegisterClient", K::Ok}});
            }));
            return 0;
        });
    });
}

// =============================================================================================================================================
// nsd: nsd:u, nsd:a (IManager). The environment is the retail one (lp1); Resolve returns the name as it was given.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_nsd() {
    return make_iface("nn::nsd::detail::IManager", [](ipc::Service& s) {
        cmd(s, "nsd", 11, "GetEnvironmentIdentifier", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            static const uint8_t kEnv[8] = {'l', 'p', '1', 0, 0, 0, 0, 0};
            buf_write(req.out_buf(0), kEnv, sizeof kEnv);
            return 0;
        });
        rows(s, "nsd", {
            {15, "SetChangeEnvironmentIdentifierDisabled", K::Ok},
            {100, "GetApplicationServerEnvironmentType", K::U8, 0},
        });
        // Resolve(fqdn_in) -> fqdn_out is the same bytes; ResolveEx answers the same with inner_result 0.
        auto copy_name = [](ipc::Request& req) {
            const ipc::Buffer in = req.in_buf(0);
            const ipc::Buffer out = req.out_buf(0);
            std::vector<uint8_t> tmp(static_cast<size_t>(in.size));
            const uint64_t n = buf_read(in, tmp.data(), in.size);
            buf_write(out, tmp.data(), n);
        };
        cmd(s, "nsd", 20, "Resolve", [copy_name](ipc::Request& req, ipc::Response&) -> uint32_t {
            copy_name(req);
            return 0;
        });
        cmd(s, "nsd", 21, "ResolveEx", [copy_name](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            copy_name(req);
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
    });
}

// =============================================================================================================================================
// sfdnsres: sfdnsres (IResolver). Every lookup fails: host not found (netdb 1, EAI_NONAME). Error text commands answer the glibc strings.
// =============================================================================================================================================

std::string netdb_text(int32_t e) {
    switch (e) {
        case 1: return "Unknown host";
        case 2: return "Temporary failure in name resolution";
        case 3: return "Non-recoverable failure in name resolution";
        case 4: return "No address associated with name";
        default: return "Unknown error";
    }
}

std::string gai_text(int32_t e) {
    switch (e) {
        case -1: return "Invalid value for ai_flags";
        case -2: return "Name or service not known";
        case -3: return "Temporary failure in name resolution";
        case -4: return "Non-recoverable failure in name resolution";
        case -5: return "No address associated with hostname";
        case -6: return "ai_family not supported";
        case -7: return "ai_socktype not supported";
        case -8: return "Servicename not supported for ai_socktype";
        case -9: return "Address family for hostname not supported";
        case -10: return "Memory allocation failure";
        case -11: return "System error";
        default: return "Unknown error";
    }
}

void write_text(const ipc::Buffer& out, const std::string& text) {
    std::vector<uint8_t> z(text.size() + 1, 0);
    std::memcpy(z.data(), text.data(), text.size());
    buf_write(out, z.data(), z.size());
}

std::shared_ptr<ipc::Service> make_resolver() {
    return make_iface("nn::socket::resolver::IResolver", [](ipc::Service& s) {
        // GetHostByNameRequest / GetHostByAddrRequest: (netdb_error, bsd_errno, size)
        cmd(s, "sfdnsres", 2, "GetHostByNameRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        cmd(s, "sfdnsres", 3, "GetHostByAddrRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        cmd(s, "sfdnsres", 4, "GetHostStringErrorRequest", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            int32_t e = req.arg<int32_t>(0);
            write_text(req.out_buf(0), netdb_text(e));
            return 0;
        });
        cmd(s, "sfdnsres", 5, "GetGaiStringErrorRequest", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            int32_t e = req.arg<int32_t>(0);
            write_text(req.out_buf(0), gai_text(e));
            return 0;
        });
        // GetAddrInfoRequest: (bsd_errno, gai_error, size)
        cmd(s, "sfdnsres", 6, "GetAddrInfoRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<int32_t>(kEaiNoName));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        cmd(s, "sfdnsres", 8, "GetCancelHandleRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(alloc_cancel_handle());
            return 0;
        });
        rows(s, "sfdnsres", {
            {9, "CancelRequest", K::Ok},
        });
        // GetHostByNameRequestWithOptions / GetHostByAddrRequestWithOptions: (size, netdb_error, bsd_errno)
        cmd(s, "sfdnsres", 10, "GetHostByNameRequestWithOptions", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            return 0;
        });
        cmd(s, "sfdnsres", 11, "GetHostByAddrRequestWithOptions", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(kNetdbHostNotFound));
            rsp.push(static_cast<uint32_t>(kBsdNetDown));
            return 0;
        });
        // GetAddrInfoRequestWithOptions: (size, gai_error, netdb_error, bsd_errno)
        cmd(s, "sfdnsres", 12, "GetAddrInfoRequestWithOptions", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<int32_t>(kEaiNoName));
            rsp.push(static_cast<uint32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
        // ResolverSetOptionRequest: (i32 ret, u32 bsd_errno), accepted
        cmd(s, "sfdnsres", 14, "ResolverSetOptionRequest", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push(static_cast<int32_t>(0));
            rsp.push(static_cast<uint32_t>(0));
            return 0;
        });
    });
}

// =============================================================================================================================================
// ntc: ntc (IStaticService) and IEnsureNetworkClockAvailabilityService. The network clock task fails at once with "network not available"
// (time 1000); Cancel makes it "task canceled" (time 1003). The finish event is signalled when the task ends.
// =============================================================================================================================================

struct NtcTask {
    ServiceEvent finish;
    uint32_t result = 0;
};

std::shared_ptr<ipc::Service> make_ntc_task() {
    auto t = std::make_shared<NtcTask>();
    return make_iface("nn::ntc::detail::IEnsureNetworkClockAvailabilityService", [t](ipc::Service& s) {
        cmd(s, "ntc", 0, "StartTask", [t](ipc::Request&, ipc::Response&) -> uint32_t {
            t->result = kTimeNetworkNotAvailable;
            t->finish.signal();
            return t->result;
        });
        cmd(s, "ntc", 1, "GetFinishNotificationEvent", [t](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(t->finish.new_handle());
            return 0;
        });
        cmd(s, "ntc", 2, "GetResult", [t](ipc::Request&, ipc::Response&) -> uint32_t { return t->result; });
        cmd(s, "ntc", 3, "Cancel", [t](ipc::Request&, ipc::Response&) -> uint32_t {
            t->result = kTimeTaskCanceled;
            t->finish.signal();
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_ntc_static() {
    return make_iface("nn::ntc::detail::IStaticService", [](ipc::Service& s) {
        cmd(s, "ntc", 0, "CreateEnsureNetworkClockAvailabilityService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_ntc_task());
            return 0;
        });
    });
}

// =============================================================================================================================================
// ngct: ngct:u (IService). No word list: Match finds nothing, Filter returns the text as it was sent.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_ngct() {
    return make_iface("nn::ngct::detail::IService", [](ipc::Service& s) {
        rows(s, "ngct", {
            {0, "Match", K::U8, 0},               // b8 matched: false
        });
        cmd(s, "ngct", 1, "Filter", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const ipc::Buffer in = req.in_buf(0);
            const ipc::Buffer out = req.out_buf(0);
            std::vector<uint8_t> tmp(static_cast<size_t>(in.size));
            const uint64_t n = buf_read(in, tmp.data(), in.size);
            buf_write(out, tmp.data(), n);
            return 0;
        });
    });
}

// =============================================================================================================================================
// notif: notif:a (INotificationServicesForApplication). No alarms; Initialize accepted.
// =============================================================================================================================================

std::shared_ptr<ipc::Service> make_notif() {
    return make_iface("nn::notification::INotificationServicesForApplication", [](ipc::Service& s) {
        rows(s, "notif", {
            {520, "ListAlarmSettings", K::S32, 0},
            {1000, "Initialize", K::Ok},
        });
    });
}

}  // namespace

// =============================================================================================================================================
// Registration and reset.
// =============================================================================================================================================

void register_network() {
    const bool offline = config_get_bool("net", "offline", true);
    NXLOG_I("net", "network services: [net] offline=%s (no connection, no friends, no BCAT data in either mode)", offline ? "true" : "false");

    for (const char* n : {"nifm:u", "nifm:a", "nifm:s"}) add(n, [] { return make_nifm_static(); });
    for (const char* n : {"bsd:u", "bsd:a", "bsd:s"}) add(n, [] { return make_bsd(); });
    add("bsdcfg", [] { return make_iface("nn::bsdsocket::cfg::ServerInterface", [](ipc::Service&) {}); });
    // ssl:s (Switch 2; the game opens it, not ssl) is the same ISslService: CreateContext must hand out a real context object.
    for (const char* n : {"ssl", "ssl:s"}) add(n, [] { return make_ssl_service(); });
    add("nim:eca", [] { return make_nim_eca(); });
    for (const char* n : {"friend:u", "friend:v", "friend:m", "friend:s", "friend:a"}) add(n, [] { return make_friend_creator(); });
    for (const char* n : {"prepo:u", "prepo:a", "prepo:a2"}) add(n, [] { return make_prepo(); });
    for (const char* n : {"pctl", "pctl:s", "pctl:r", "pctl:a"}) add(n, [] { return make_pctl_factory(); });
    for (const char* n : {"bcat:u", "bcat:a"}) add(n, [] { return make_bcat_creator(); });
    add("ldn:u", [] { return make_ldn_user_creator(); });
    for (const char* n : {"nsd:u", "nsd:a"}) add(n, [] { return make_nsd(); });
    add("sfdnsres", [] { return make_resolver(); });
    add("ntc", [] { return make_ntc_static(); });
    add("ngct:u", [] { return make_ngct(); });
    add("notif:a", [] { return make_notif(); });
}

void network_reset() {
    NetState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.seen.clear();
    s.next_client_id = 1;
    s.next_cancel_handle = 1;
    s.next_pki_id = 1;
    s.ldn_state = kLdnStateNone;
    s.ssl_contexts = 0;
}

}  // namespace nxrt::services
