// SPDX-License-Identifier: GPL-3.0-or-later
// appletOE and appletAE (S3c): the applet manager as the one application sees it. The application is in focus, its message
// queue holds what the console queues for it, the notification flags it sets are real state. Library applets: the software keyboard
// answers a text (a prompt on the PC, or a configured text); the player select answers the one user; the others end at once.
// Command ids and layouts follow the Switchbrew "Applet Manager services" page and ScarletNX's am.swipc.
// Adapted from ScarletNX src/nx/hle/services/am.c and am.swipc (GPL-3.0-or-later): the command table, the message queue, the lock
// accessor, the audio volumes, the save data rules and the launch parameter storage.
// Adapted from Ryujinx src/Ryujinx.HLE/HOS/Services/Am (MIT): the result codes, the LaunchParameterKind values, the preselected user
// layout (AppletAE/Storage/StorageHelper.cs MakeLaunchParams) and the GamePlay recording command id.
// Adapted from SuperMarioWonderRecomp runtime/src/svc_am.cpp (MIT): the window controller's applet resource user id.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

using Bytes = std::vector<uint8_t>;
using ipc::Handler;
using ipc::Request;
using ipc::Response;
using ipc::Service;
using ServicePtr = std::shared_ptr<Service>;

// nn::am::AppletMessage values the queue carries (Ryujinx AppletAE/AllSystemAppletProxiesService/.../AppletMessage.cs).
constexpr uint32_t kMsgFocusStateChanged = 15;
constexpr uint32_t kMsgOperationModeChanged = 30;
constexpr uint32_t kMsgPerformanceModeChanged = 31;
constexpr uint32_t kMsgStartupLogoDisappeared = 95;

// nn::am::service::FocusState: InFocus 1, OutOfFocus 2, Background 3.
constexpr uint8_t kFocusInFocus = 1;
constexpr uint8_t kFocusOutOfFocus = 2;
constexpr uint64_t kAppletResourceUserId = 0x51;

// nn::am::service::LaunchParameterKind (Ryujinx AppletOE/.../Types/LaunchParameterKind.cs): UserChannel 1, PreselectedUser 2.
constexpr uint32_t kLaunchParamUserChannel = 1;
constexpr uint32_t kLaunchParamPreselectedUser = 2;
constexpr uint32_t kLaunchParamsMagic = 0xC79497CAu;   // LaunchParameterAccountPreselectedUser
constexpr uint64_t kLaunchParamsSize = 0x88;
constexpr uint8_t kSaveDataTypeAccount = 1;            // nn::fs::SaveDataType::Account

// Largest storage a guest can ask for (CreateStorage and the transfer / handle storages).
constexpr int64_t kMaxStorageSize = 0x10000000;

// Am results (module 128): result = module | description << 9 (services.h res()).
const uint32_t kNoData = res(128, 2);                // ResultNoDataInChannel: no launch parameter / nothing to pop
const uint32_t kNoMessages = res(128, 3);            // ResultNoMessages: 0x680
const uint32_t kInvalidOffset = res(128, 503);       // ResultInvalidOffset
const uint32_t kInvalidStorage = res(128, 511);      // ResultInvalidStorageType
const uint32_t kOutOfMemory = res(128, 600);         // MemoryAllocationFailed
// A refused library applet. The SDK (nn::applet::CreateLibraryApplet) fails without aborting only for am descriptions 701..749
// (anything else, e.g. 128-35 AppletLaunchFailed, ends in svcBreak: work/audit/crashes.md); 712 is Switchbrew's "thread stack pool
// exhausted (out of memory)", a resource refusal the caller gets back as a plain failure.
const uint32_t kLibraryAppletFailed = res(128, 712);
const uint32_t kAppletCanceled = res(128, 22);       // LibraryAppletCanceled: 0x2C80, what JoinLibraryApplet maps to exit reason 1

// Everything the applet manager keeps for the process. Replaced by fresh_state() on start-up and by am_reset().
struct AmState {
    std::deque<uint32_t> msgs;                         // the AppletMessage queue, oldest first
    ServiceEvent msg_event{false};                     // signalled while msgs is not empty
    ServiceEvent sleep_lock_event{false};
    ServiceEvent launchable_event{true};               // a library applet can always be launched
    ServiceEvent tick_event{true};                     // accumulated suspended tick changed
    ServiceEvent never_event{false};                   // the events that never fire
    bool docked = true;
    uint8_t focus = kFocusInFocus;
    bool operation_notify = false;                     // SetOperationModeChangedNotification
    bool performance_notify = false;                   // SetPerformanceModeChangedNotification
    bool vr_mode = false;
    bool vr_mode_3d = false;
    bool auto_sleep_disabled = false;
    uint32_t idle_extension = 0;
    uint64_t next_layer = 1;
    bool preselected_popped = false;
    std::vector<std::shared_ptr<Bytes>> user_channel;  // UnpopToUserChannel pushes; PopLaunchParameter(UserChannel) pops the last
    bool save_extended = false;                        // ExtendSaveData was called for the account save data
    int64_t save_normal = 0;
    int64_t save_journal = 0;
};

// One mutex for the whole applet manager: the host threads (amctl) and the guest's sessions both go through it.
std::mutex& mu() {
    static std::mutex m;
    return m;
}

// The state a fresh application starts in: in focus, docked as the profile says, and FocusStateChanged queued (Ryujinx
// AppletStateMgr queues exactly that; the operation and performance mode messages come only when the mode changes).
AmState fresh_state() {
    AmState s;
    s.docked = profile().docked;
    s.msgs.push_back(kMsgFocusStateChanged);
    s.msg_event.signal();
    return s;
}

AmState& st() {
    static AmState s = fresh_state();
    return s;
}

// Queues one AppletMessage and signals the message event. The caller holds mu().
void queue_message(uint32_t msg) {
    st().msgs.push_back(msg);
    st().msg_event.signal();
}

// A command the console answers without doing anything (am.swipc marks it @stub): logged at Debug, with `zero_bytes` of zero output
// so the reply has the layout the SDK reads.
Handler ignored(const char* name, size_t zero_bytes = 0) {
    return [name, zero_bytes](Request&, Response& rsp) -> uint32_t {
        NXLOG_D("am", "%s: accepted, nothing done", name);
        rsp.out.insert(rsp.out.end(), zero_bytes, uint8_t{0});
        return 0;
    };
}

// Copy handles belong to the client (HIPC copies them, the guest closes its own afterwards): nothing to close here (debug note 122).
void close_copies(const Request&) {}

// Byte range [off, off + n) lies inside a storage of `size` bytes.
bool span_ok(uint64_t size, int64_t off, uint64_t n) {
    return off >= 0 && static_cast<uint64_t>(off) <= size && n <= size - static_cast<uint64_t>(off);
}

bool storage_size_ok(int64_t size) { return size >= 0 && size <= kMaxStorageSize; }

// Appends `n` bytes of `src` (padded with zeros) to a reply: fixed-size outputs such as the display version.
void push_bytes(Response& rsp, const void* src, size_t src_len, size_t n) {
    const size_t at = rsp.out.size();
    rsp.out.resize(at + n, 0);
    if (src && src_len) std::memcpy(rsp.out.data() + at, src, src_len < n ? src_len : n);
}

ServicePtr make_accessor(std::shared_ptr<Bytes> data);

// IStorage over a host byte vector (the launch parameters, CreateStorage and the transfer / handle storages). Open is the one
// command here (IStorage's OpenTransferStorage has no accessor in this model).
class AmStorage : public Service {
public:
    explicit AmStorage(std::shared_ptr<Bytes> bytes) : Service("IStorage"), data(std::move(bytes)) {
        add_cmd(0, "Open", [d = data](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_accessor(d));
            return 0;
        });
    }
    std::shared_ptr<Bytes> data;
};

ServicePtr new_storage(std::shared_ptr<Bytes> data) { return std::make_shared<AmStorage>(std::move(data)); }

// IStorageAccessor: GetSize, Write (guest -> storage), Read (storage -> guest). Ranges past the end are refused, not cut.
ServicePtr make_accessor(std::shared_ptr<Bytes> data) {
    return make_iface("IStorageAccessor", [data](Service& s) {
        s.add_cmd(0, "GetSize", [data](Request&, Response& rsp) -> uint32_t {
            rsp.push<int64_t>(static_cast<int64_t>(data->size()));
            return 0;
        });
        s.add_cmd(10, "Write", [data](Request& req, Response&) -> uint32_t {
            const int64_t off = req.arg<int64_t>(0);
            const ipc::Buffer b = req.in_buf(0);
            if (!span_ok(data->size(), off, b.size)) return kInvalidOffset;
            buf_read(b, data->data() + off, b.size);
            return 0;
        });
        s.add_cmd(11, "Read", [data](Request& req, Response&) -> uint32_t {
            const int64_t off = req.arg<int64_t>(0);
            const ipc::Buffer b = req.out_buf(0);
            if (!span_ok(data->size(), off, b.size)) return kInvalidOffset;
            buf_write(b, data->data() + off, b.size);
            return 0;
        });
    });
}

// ILockAccessor: granted whenever it is free. The event is signalled while it is free (libnx waits on it before TryLock) and
// again when it is released.
struct LockState {
    bool locked = false;
    ServiceEvent event{true};
};

ServicePtr make_lock_accessor() {
    auto l = std::make_shared<LockState>();
    return make_iface("ILockAccessor", [l](Service& s) {
        s.add_cmd(1, "TryLock", [l](Request& req, Response& rsp) -> uint32_t {
            const bool return_handle = req.arg<uint8_t>(0) != 0;
            bool acquired;
            {
                std::lock_guard<std::mutex> lk(mu());
                acquired = !l->locked;
                l->locked = true;
            }
            rsp.push<uint8_t>(acquired ? 1 : 0);
            if (return_handle) rsp.out_copy.push_back(l->event.new_handle());
            return 0;
        });
        s.add_cmd(2, "Unlock", [l](Request&, Response&) -> uint32_t {
            {
                std::lock_guard<std::mutex> lk(mu());
                l->locked = false;
            }
            l->event.signal();
            return 0;
        });
        s.add_cmd(3, "GetEvent", [l](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(l->event.new_handle());
            return 0;
        });
        s.add_cmd(4, "IsLocked", [l](Request&, Response& rsp) -> uint32_t {
            bool locked;
            {
                std::lock_guard<std::mutex> lk(mu());
                locked = l->locked;
            }
            rsp.push<uint8_t>(locked ? 1 : 0);
            return 0;
        });
    });
}

// IAudioController: the expected volumes, kept per controller as ScarletNX keeps them. The fade of ChangeMainAppletMasterVolume is
// not played.
struct Volumes {
    float main = 1.0f;
    float library = 1.0f;
};

float clamp_volume(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

ServicePtr make_audio_controller() {
    auto v = std::make_shared<Volumes>();
    return make_iface("IAudioController", [v](Service& s) {
        s.add_cmd(0, "SetExpectedMasterVolume", [v](Request& req, Response&) -> uint32_t {
            v->main = clamp_volume(req.arg<float>(0));
            v->library = clamp_volume(req.arg<float>(4));
            return 0;
        });
        s.add_cmd(1, "GetMainAppletExpectedMasterVolume", [v](Request&, Response& rsp) -> uint32_t {
            rsp.push<float>(v->main);
            return 0;
        });
        s.add_cmd(2, "GetLibraryAppletExpectedMasterVolume", [v](Request&, Response& rsp) -> uint32_t {
            rsp.push<float>(v->library);
            return 0;
        });
        s.add_cmd(3, "ChangeMainAppletMasterVolume", [v](Request& req, Response&) -> uint32_t {
            v->main = clamp_volume(req.arg<float>(0));
            return 0;
        });
        s.add_cmd(4, "SetTransparentVolumeRate", ignored("SetTransparentVolumeRate"));
    });
}

// [am] resolution = WxH overrides the default resolution of the mode (ICommonStateGetter::GetDefaultDisplayResolution).
bool parse_resolution(const std::string& text, uint32_t* w, uint32_t* h) {
    if (text.empty()) return false;
    char* end = nullptr;
    const unsigned long a = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != 'x') return false;
    const char* p = end + 1;
    const unsigned long b = std::strtoul(p, &end, 10);
    if (end == p || *end != '\0' || a == 0 || b == 0 || a > 0xFFFF || b > 0xFFFF) return false;
    *w = static_cast<uint32_t>(a);
    *h = static_cast<uint32_t>(b);
    return true;
}

void default_resolution(bool docked, uint32_t* w, uint32_t* h) {
    *w = docked ? 1920 : 1280;
    *h = docked ? 1080 : 720;
    uint32_t cw = 0, ch = 0;
    if (parse_resolution(nxrt::config_get_str("am", "resolution", ""), &cw, &ch)) {
        *w = cw;
        *h = ch;
    }
}

// The events that never fire (GetDefaultDisplayResolutionChangeEvent and the rest): each call hands out a new handle onto this one.
ServiceEvent& never_event() { return st().never_event; }

// ICommonStateGetter: the message queue, the operation and performance modes, VR mode, the default resolution and the boost calls.
ServicePtr make_common_state_getter() {
    return make_iface("ICommonStateGetter", [](Service& s) {
        s.add_cmd(0, "GetEventHandle", [](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.out_copy.push_back(st().msg_event.new_handle());
            return 0;
        });
        s.add_cmd(1, "ReceiveMessage", [](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            AmState& a = st();
            if (a.msgs.empty()) {
                a.msg_event.clear();
                return kNoMessages;
            }
            rsp.push<uint32_t>(a.msgs.front());
            a.msgs.pop_front();
            if (a.msgs.empty()) a.msg_event.clear();
            return 0;
        });
        s.add_cmd(2, "GetThisAppletKind", ignored("GetThisAppletKind", 8));
        s.add_cmd(5, "GetOperationMode", [](Request&, Response& rsp) -> uint32_t {   // nn::oe::OperationMode: Handheld 0, Console 1
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint8_t>(st().docked ? 1 : 0);
            return 0;
        });
        s.add_cmd(6, "GetPerformanceMode", [](Request&, Response& rsp) -> uint32_t {   // nn::oe::PerformanceMode: Handheld 0, Boost 1
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint32_t>(st().docked ? 1u : 0u);
            return 0;
        });
        s.add_cmd(8, "GetBootMode", ignored("GetBootMode", 1));
        s.add_cmd(9, "GetCurrentFocusState", [](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint8_t>(st().focus);
            return 0;
        });
        s.add_cmd(10, "RequestToAcquireSleepLock", [](Request&, Response&) -> uint32_t {
            st().sleep_lock_event.signal();
            return 0;
        });
        s.add_cmd(11, "ReleaseSleepLock", ignored("ReleaseSleepLock"));
        s.add_cmd(12, "ReleaseSleepLockTransiently", ignored("ReleaseSleepLockTransiently"));
        s.add_cmd(13, "GetAcquiredSleepLockEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(st().sleep_lock_event.new_handle());
            return 0;
        });
        s.add_cmd(30, "GetHomeButtonReaderLockAccessor", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_lock_accessor());
            return 0;
        });
        s.add_cmd(31, "GetReaderLockAccessorEx", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_lock_accessor());
            return 0;
        });
        s.add_cmd(32, "GetWriterLockAccessorEx", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_lock_accessor());
            return 0;
        });
        s.add_cmd(50, "IsVrModeEnabled", [](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint8_t>(st().vr_mode ? 1 : 0);
            return 0;
        });
        s.add_cmd(51, "SetVrModeEnabled", [](Request& req, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().vr_mode = req.arg<uint8_t>(0) != 0;
            return 0;
        });
        s.add_cmd(52, "SetLcdBacklighOffEnabled", ignored("SetLcdBacklighOffEnabled"));
        s.add_cmd(53, "BeginVrModeEx", [](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().vr_mode = true;
            return 0;
        });
        s.add_cmd(54, "EndVrModeEx", [](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().vr_mode = false;
            return 0;
        });
        s.add_cmd(55, "IsInControllerFirmwareUpdateSection", ignored("IsInControllerFirmwareUpdateSection", 1));
        s.add_cmd(59, "SetVrPositionForDebug", ignored("SetVrPositionForDebug"));
        s.add_cmd(60, "GetDefaultDisplayResolution", [](Request&, Response& rsp) -> uint32_t {
            bool docked;
            {
                std::lock_guard<std::mutex> lk(mu());
                docked = st().docked;
            }
            uint32_t w = 0, h = 0;
            default_resolution(docked, &w, &h);
            rsp.push<int32_t>(static_cast<int32_t>(w));
            rsp.push<int32_t>(static_cast<int32_t>(h));
            return 0;
        });
        s.add_cmd(61, "GetDefaultDisplayResolutionChangeEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(never_event().new_handle());
            return 0;
        });
        s.add_cmd(62, "GetHdcpAuthenticationState", ignored("GetHdcpAuthenticationState", 4));
        s.add_cmd(63, "GetHdcpAuthenticationStateChangeEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(never_event().new_handle());
            return 0;
        });
        s.add_cmd(66, "SetCpuBoostMode", ignored("SetCpuBoostMode"));
        s.add_cmd(67, "CancelCpuBoostMode", ignored("CancelCpuBoostMode"));
        s.add_cmd(68, "GetBuiltInDisplayType", ignored("GetBuiltInDisplayType", 4));
        // 21.0.0+: the start-up logo is gone at once (there is none here), so the message is queued when it is asked for.
        s.add_cmd(130, "EnableStartupLogoDisappearedMessage", [](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            queue_message(kMsgStartupLogoDisappeared);
            return 0;
        });
        s.add_cmd(300, "GetSettingsPlatformRegion", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(1);   // Global, as ScarletNX answers
            return 0;
        });
        s.add_cmd(900, "SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled",
                  ignored("SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled"));
        s.add_cmd(1000, "BeginVrMode3d", [](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().vr_mode_3d = true;
            return 0;
        });
        s.add_cmd(1001, "EndVrMode3d", [](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().vr_mode_3d = false;
            return 0;
        });
        s.add_cmd(1002, "IsVrModeEnabled3d", [](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint8_t>(st().vr_mode_3d ? 1 : 0);
            return 0;
        });
    });
}

// ISelfController: exit, the notification flags (real state), the display layers, the idle and sleep settings.
ServicePtr make_self_controller() {
    return make_iface("ISelfController", [](Service& s) {
        // platform 2 ([S2], switchbrew): 2000 GetGraphicsSupervisor hands out a TIPC session (a move handle, also from a domain);
        // 2100-2402 switch recording / sharing / copyright layers and audio sharing on or off, which a PC run has nothing to do for.
        s.add_cmd(2000, "GetGraphicsSupervisor", [](Request&, Response& rsp) -> uint32_t {
            const uint32_t h = create_session_handle(std::make_shared<ipc::StubService>("GraphicsSupervisor"));
            if (!h) return kernel::kStubService;
            rsp.out_move.push_back(h);
            return 0;
        });
        for (uint32_t id : {2100u, 2200u, 2300u, 2400u, 2401u, 2402u})
            s.add_cmd(id, "SetLayerOrAudioSharingEnabled (S2)", [](Request&, Response&) -> uint32_t { return 0; });
        // Exit: the application asks to end. The run is ended by the graphics shell (src/gfx), which this library does not link, so
        // here the request is logged and answered with success only.
        s.add_cmd(0, "Exit", [](Request&, Response&) -> uint32_t {
            NXLOG_W("am", "Exit requested by the application: nothing ends the run here (the shell is not linked into the services)");
            return 0;
        });
        s.add_cmd(1, "LockExit", ignored("LockExit"));
        s.add_cmd(2, "UnlockExit", ignored("UnlockExit"));
        s.add_cmd(3, "EnterFatalSection", ignored("EnterFatalSection"));
        s.add_cmd(4, "LeaveFatalSection", ignored("LeaveFatalSection"));
        s.add_cmd(9, "GetLibraryAppletLaunchableEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(st().launchable_event.new_handle());
            return 0;
        });
        s.add_cmd(10, "SetScreenShotPermission", ignored("SetScreenShotPermission"));
        s.add_cmd(11, "SetOperationModeChangedNotification", [](Request& req, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().operation_notify = req.arg<uint8_t>(0) != 0;
            return 0;
        });
        s.add_cmd(12, "SetPerformanceModeChangedNotification", [](Request& req, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().performance_notify = req.arg<uint8_t>(0) != 0;
            return 0;
        });
        s.add_cmd(13, "SetFocusHandlingMode", ignored("SetFocusHandlingMode"));
        s.add_cmd(14, "SetRestartMessageEnabled", ignored("SetRestartMessageEnabled"));
        s.add_cmd(16, "SetOutOfFocusSuspendingEnabled", ignored("SetOutOfFocusSuspendingEnabled"));
        s.add_cmd(40, "CreateManagedDisplayLayer", [](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint64_t>(st().next_layer++);
            return 0;
        });
        s.add_cmd(41, "IsSystemBufferSharingEnabled", ignored("IsSystemBufferSharingEnabled"));
        s.add_cmd(44, "CreateManagedDisplaySeparableLayer", [](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint64_t>(st().next_layer++);
            rsp.push<uint64_t>(st().next_layer++);
            return 0;
        });
        s.add_cmd(62, "SetIdleTimeDetectionExtension", [](Request& req, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().idle_extension = req.arg<uint32_t>(0);
            return 0;
        });
        s.add_cmd(63, "GetIdleTimeDetectionExtension", [](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint32_t>(st().idle_extension);
            return 0;
        });
        s.add_cmd(68, "SetAutoSleepDisabled", [](Request& req, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().auto_sleep_disabled = req.arg<uint8_t>(0) != 0;
            return 0;
        });
        s.add_cmd(69, "IsAutoSleepDisabled", [](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint8_t>(st().auto_sleep_disabled ? 1 : 0);
            return 0;
        });
        s.add_cmd(90, "GetAccumulatedSuspendedTickValue", ignored("GetAccumulatedSuspendedTickValue", 8));
        s.add_cmd(91, "GetAccumulatedSuspendedTickChangedEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(st().tick_event.new_handle());
            return 0;
        });
        s.add_cmd(100, "SetAlbumImageTakenNotificationEnabled", ignored("SetAlbumImageTakenNotificationEnabled"));
        s.add_cmd(110, "SetApplicationAlbumUserData", ignored("SetApplicationAlbumUserData"));
        s.add_cmd(120, "SaveCurrentScreenshot", ignored("SaveCurrentScreenshot"));
    });
}

// IWindowController: the applet resource user id of the one application (the value Wonder answers).
ServicePtr make_window_controller() {
    return make_iface("IWindowController", [](Service& s) {
        s.add_cmd(1, "GetAppletResourceUserId", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(kAppletResourceUserId);
            return 0;
        });
        s.add_cmd(10, "AcquireForegroundRights", ignored("AcquireForegroundRights"));
        s.add_cmd(11, "ReleaseForegroundRights", ignored("ReleaseForegroundRights"));
        s.add_cmd(12, "RejectToChangeIntoBackground", ignored("RejectToChangeIntoBackground"));
    });
}

// ---- library applets -------------------------------------------------------------------------------------------------------------
// Every applet title A's main (and the SDK functions it imports) can create gets its own answer; the list and the reasons are in
// docs/IPC_ANSWERS.md. The software keyboard (LibraryAppletId 0x11) in its normal, full-screen form answers a text, the player select
// (0x10) answers the one user or, for its network-account modes, that the player backed out; the error applet (0x0E) logs the error it
// was given; the shop, friends, data management, network connection and GameShare matchmaking applets end as cancelled by the player
// (offline: nothing to buy, invite, free or connect to). An applet id nothing here knows ends at once with no output. Command ids
// follow the Switchbrew "Applet Manager services" page (IAppletAccessor / ILibraryAppletAccessor). The keyboard storages follow Ryujinx
// src/Ryujinx.HLE/HOS/Applets/SoftwareKeyboard (MIT: SoftwareKeyboardConfig, the 0x7D8-byte output) and Eden / yuzu
// applet_software_keyboard_types.h (GPL-3.0-or-later); the accessor's channel model follows the user's Afterpulse runtime (am.cpp, same
// licence). platform 2 games pass the applet id with more bits above it (title A: 0x1020011 for the keyboard), so the low 16 bits pick it.
constexpr uint32_t kAppletIdError = 0x0E;                     // LibraryAppletError (title A: 0x100000E, nn::err::ShowError)
constexpr uint32_t kAppletIdShop = 0x14;                      // LibraryAppletShop (0x1000014, nn::ec::detail::ShowShopPage)
// LibraryAppletMyPage (0x100001A): nn::friends::StartFriendInvitation / StartSendingFriendInvitation (sdk 0x75003e32a0, 0x75003e33b0)
// run it with a 0x10A8-byte argument (kind 8 / 9). CreateMyPageApplet aborts if creating it fails, so it is created and ends as
// cancelled at once (offline: no invitation can be sent); nn::la::StartLibraryApplet turns that into a failure and the friends call
// returns 0x3C79 (121-30) to the game, which ignores it (StartFriendInvitation) or takes its failure path (StartSending...).
constexpr uint32_t kAppletIdMyPage = 0x1A;
// LibraryAppletDataErase (0x100000D): nn::fs::EnsureSaveData / ExtendSaveData (sdk 0x75003e6a78, 0x75003e6d7c) run it through
// nn::la::StartLibraryAppletEasy when IApplicationFunctions reports a required size above 0 (the save does not fit). Ends cancelled:
// the fs call then returns 0x3E02 (not enough space), as when the player leaves data management without freeing anything.
constexpr uint32_t kAppletIdDataErase = 0x0D;
// LibraryAppletNetConnect (0x0F): nn::nifm::HandleNetworkRequestErrorResult. title A's two imports of it are answered before the SDK
// gets here (core/switch2.cpp); if it is reached anyway it ends cancelled (the network problem is not resolved).
constexpr uint32_t kAppletIdNetConnect = 0x0F;
// GameShare matchmaking (0x1000024, or 0x1030024 for the other StreamPlayType): nn::streamplay::detail::ShowMatchmakeSupportCore
// (sdk 0x75006b9420), from nn::streamplay::OpenSessionWithMatchmakeSupport (game: main 0x7101132f60, GameShare host). Ends cancelled:
// the SDK returns 0x3D1F, which the game (main 0x7101133704) treats as the player backing out, with no error shown.
constexpr uint32_t kAppletIdMatchmake = 0x24;
constexpr uint32_t kAppletIdPlayerSelect = 0x10;              // nn::applet::AppletId LibraryAppletPlayerSelect
// The player select's UiSettings (in data 1, 0xA0 bytes): u32 mode at 0 (nn::account::detail::StartPselApplet, sdk 0x75001844c0,
// pushes it and pops a 0x18-byte {u64 result, Uid} answer whose result it returns). Mode 0 is ShowUserSelector; 2 is
// EnsureNetworkServiceAccountAvailable, 10 / 11 ShowLicenseRequirementsForNetworkService (also from nn::err::ShowError for some
// network results). Offline there is no console account to link and no online-service licence, so every mode but the user selector
// answers ResultCancelledByUser (124-1, 0x27C). title A's callers (main 0x7100542f20, 0x7100583860 ...) read 0x27C as "the player
// backed out" and stop quietly; a success from mode 2 would make them retry the token request in a loop.
constexpr uint32_t kPselModeUserSelector = 0;
constexpr uint32_t kAccountCancelledByUser = 0x27C;
constexpr uint32_t kAppletIdSwkbd = 0x11;                     // LibraryAppletSwkbd
constexpr uint32_t kAppletModeAllForeground = 0;              // nn::applet::LibraryAppletMode: the normal keyboard
constexpr size_t kSwkbdStringSize = 0x7D4;                    // STRING_BUFFER_SIZE
constexpr size_t kSwkbdOutputSize = 4 + kSwkbdStringSize;     // SwkbdResult + string: 0x7D8
constexpr size_t kSwkbdTextCheckSize = 4 + kSwkbdStringSize;  // SwkbdTextCheck the application pushes back: 0x7D8
constexpr uint32_t kSwkbdDefaultMaxLength = 500;              // DEFAULT_MAX_TEXT_LENGTH
constexpr uint32_t kSwkbdResultOk = 0;
constexpr uint32_t kSwkbdResultCancel = 1;
constexpr uint32_t kTextCheckSuccess = 0;                     // SwkbdTextCheckResult: Success 0, Failure 1, Confirm 2, Silent 3
constexpr uint32_t kTextCheckConfirm = 2;
// SoftwareKeyboardConfig (Ryujinx, sequential layout) field offsets.
constexpr size_t kCfgOkText = 0x04;            // u16[9]
constexpr size_t kCfgHeaderText = 0x24;        // u16[65]
constexpr size_t kCfgSubText = 0xA6;           // u16[129]
constexpr size_t kCfgGuideText = 0x1A8;        // u16[257]
constexpr size_t kCfgMaxTextLength = 0x3AC;
constexpr size_t kCfgMinTextLength = 0x3B0;
constexpr size_t kCfgPasswordMode = 0x3B4;
constexpr size_t kCfgUseUtf8 = 0x3BD;
constexpr size_t kCfgInitialOffset = 0x3C0;
constexpr size_t kCfgInitialLength = 0x3C4;    // UTF-16 characters
constexpr size_t kCfgUseTextCheck = 0x3D0;
constexpr size_t kCfgCommonSize = 0x3D4;
constexpr size_t kCommonArgsLibraryVersion = 8;               // CommonArguments: version, size, library_version, theme ...

std::string utf16_to_utf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(s[i + 1]) - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::u16string utf8_to_utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 0x6) { n = 2; cp = c & 0x1Fu; }
        else if ((c >> 4) == 0xE) { n = 3; cp = c & 0x0Fu; }
        else if ((c >> 3) == 0x1E) { n = 4; cp = c & 0x07u; }
        if (n > 1) {
            if (i + n > s.size()) { cp = 0xFFFD; n = 1; }
            for (size_t k = 1; k < n; ++k) {
                const uint8_t cc = static_cast<uint8_t>(s[i + k]);
                if ((cc >> 6) != 0x2) { cp = 0xFFFD; n = k; break; }
                cp = (cp << 6) | (cc & 0x3Fu);
            }
        }
        i += n;
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp > 0xFFFF ? 0xFFFD : cp));
        }
    }
    return out;
}

template <typename T>
T read_at(const Bytes& b, size_t off, T fallback) {
    T v = fallback;
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

// A NUL-terminated UTF-16 string of at most `chars` units at `off`, as UTF-8 (control characters dropped, as Ryujinx strips them).
std::string read_u16_text(const Bytes& b, size_t off, size_t chars) {
    std::u16string t;
    for (size_t i = 0; i < chars && off + 2 * i + 2 <= b.size(); ++i) {
        char16_t c;
        std::memcpy(&c, b.data() + off + 2 * i, 2);
        if (c == 0) break;
        if (c >= 0x20 || c == 0x0A) t.push_back(c);
    }
    return utf16_to_utf8(t);
}

std::atomic<bool>& prompt_flag() {
    static std::atomic<bool> on{false};
    return on;
}

// One library applet's channels and state. Guarded by mu() (the prompt thread takes it to answer).
struct AppletState {
    uint32_t id = 0;            // the low 16 bits of what the game asked for
    uint32_t full_id = 0;
    uint32_t mode = 0;
    bool started = false;
    bool completed = false;
    std::deque<std::shared_ptr<Bytes>> in_data;            // PushInData, oldest first
    std::deque<std::shared_ptr<Bytes>> interactive_in;     // PushInteractiveInData
    std::deque<std::shared_ptr<Bytes>> out_data;           // PopOutData
    std::deque<std::shared_ptr<Bytes>> interactive_out;    // PopInteractiveOutData
    ServiceEvent state_event{false};                       // signalled once the applet has ended
    ServiceEvent out_event{false};                         // signalled while out_data is not empty
    ServiceEvent interactive_out_event{false};             // signalled while interactive_out is not empty
    bool utf8 = false;                                     // software keyboard: the config's use_utf8
    bool text_check = false;                               // software keyboard: the config's use_text_check
    std::u16string text;                                   // software keyboard: the text answered
    swkbdui::Request req;                                  // software keyboard: what the prompt shows
};

// Ends the applet: it has exited and its state-changed event fires. The caller holds mu().
void applet_finish(AppletState& a) {
    a.completed = true;
    a.state_event.signal();
}

void push_out(AppletState& a, std::shared_ptr<Bytes> d) {
    a.out_data.push_back(std::move(d));
    a.out_event.signal();
}

// Copies `text` into a zeroed buffer of `cap` bytes at `dst`, as UTF-8 or UTF-16LE, leaving room for the NUL.
void put_text(uint8_t* dst, size_t cap, const std::u16string& text, bool utf8) {
    if (utf8) {
        const std::string t = utf16_to_utf8(text);
        std::memcpy(dst, t.data(), t.size() < cap - 1 ? t.size() : cap - 1);
    } else {
        const size_t n = text.size() * 2;
        std::memcpy(dst, text.data(), n < cap - 2 ? n : cap - 2);
    }
}

// The 0x7D8-byte keyboard output: SwkbdResult, then the text, zero padded. The applet then ends. The caller holds mu().
void swkbd_submit(AppletState& a, uint32_t result) {
    auto out = std::make_shared<Bytes>(kSwkbdOutputSize, uint8_t{0});
    std::memcpy(out->data(), &result, 4);
    if (result == kSwkbdResultOk) put_text(out->data() + 4, kSwkbdStringSize, a.text, a.utf8);
    push_out(a, out);
    applet_finish(a);
}

// The text is known (or the player cancelled): submit it, or with text check on hand it to the application first (u64 byte size with
// the NUL, then the string: 8 + 0x7D4 bytes); its answer comes back through PushInteractiveInData. The caller holds mu().
void swkbd_answer(AppletState& a, bool ok) {
    if (a.completed) return;
    if (!ok) {
        NXLOG_I("am", "swkbd: cancelled");
        swkbd_submit(a, kSwkbdResultCancel);
        return;
    }
    NXLOG_I("am", "swkbd: answering \"%s\"%s", utf16_to_utf8(a.text).c_str(), a.text_check ? " (to the game's text check first)" : "");
    if (!a.text_check) {
        swkbd_submit(a, kSwkbdResultOk);
        return;
    }
    auto out = std::make_shared<Bytes>(8 + kSwkbdStringSize, uint8_t{0});
    const uint64_t size = a.utf8 ? utf16_to_utf8(a.text).size() + 1 : (a.text.size() + 1) * 2;
    std::memcpy(out->data(), &size, 8);
    put_text(out->data() + 8, kSwkbdStringSize, a.text, a.utf8);
    a.interactive_out.push_back(out);
    a.interactive_out_event.signal();
}

// Asks the PC prompt on its own thread; the answer arrives with mu() taken there.
void swkbd_prompt_async(const std::shared_ptr<AppletState>& ap) {
    swkbdui::Request r = ap->req;
    std::thread([ap, r] {
        std::string t;
        const bool ok = swkbdui::win32_prompt(r, &t);
        std::u16string u = utf8_to_utf16(t);
        std::lock_guard<std::mutex> lk(mu());
        if (ok) {
            const uint32_t max_len = r.max_len ? r.max_len : kSwkbdDefaultMaxLength;
            if (u.size() > max_len) u.resize(max_len);
            ap->text = u;
        }
        swkbd_answer(*ap, ok);
    }).detach();
}

// Start of the normal keyboard: in data 0 is CommonArguments, 1 the keyboard config, 2 the work buffer (initial text, dictionary).
// The caller holds mu().
void swkbd_start(const std::shared_ptr<AppletState>& ap) {
    AppletState& a = *ap;
    std::shared_ptr<Bytes> common = a.in_data.size() > 0 ? a.in_data[0] : nullptr;
    std::shared_ptr<Bytes> cfg = a.in_data.size() > 1 ? a.in_data[1] : nullptr;
    std::shared_ptr<Bytes> work = a.in_data.size() > 2 ? a.in_data[2] : nullptr;
    const uint32_t version = common ? read_at<uint32_t>(*common, kCommonArgsLibraryVersion, 0) : 0;
    uint32_t max_len = kSwkbdDefaultMaxLength;
    swkbdui::Request& r = a.req;
    r.title = profile().name;
    if (cfg && cfg->size() >= kCfgCommonSize) {
        const uint32_t m = read_at<uint32_t>(*cfg, kCfgMaxTextLength, 0);
        if (m > 0 && m <= kSwkbdDefaultMaxLength) max_len = m;
        r.min_len = read_at<uint32_t>(*cfg, kCfgMinTextLength, 0);
        r.password = read_at<uint32_t>(*cfg, kCfgPasswordMode, 0) != 0;
        a.utf8 = read_at<uint8_t>(*cfg, kCfgUseUtf8, 0) != 0;
        a.text_check = read_at<uint8_t>(*cfg, kCfgUseTextCheck, 0) != 0;
        r.ok_text = read_u16_text(*cfg, kCfgOkText, 9);
        r.header = read_u16_text(*cfg, kCfgHeaderText, 65);
        r.sub = read_u16_text(*cfg, kCfgSubText, 129);
        r.guide = read_u16_text(*cfg, kCfgGuideText, 257);
        const int32_t ioff = read_at<int32_t>(*cfg, kCfgInitialOffset, 0);
        const int32_t ilen = read_at<int32_t>(*cfg, kCfgInitialLength, 0);
        if (work && ioff >= 0 && ilen > 0) r.initial = read_u16_text(*work, static_cast<size_t>(ioff), static_cast<size_t>(ilen));
    } else {
        NXLOG_W("am", "swkbd: no keyboard config among the %zu storages pushed: defaults used", a.in_data.size());
    }
    r.max_len = max_len;
    NXLOG_I("am", "swkbd: version 0x%x, config %zu bytes, work %zu bytes, max length %u, utf8 %d, text check %d, header \"%s\", guide \"%s\", initial \"%s\"",
            version, cfg ? cfg->size() : size_t{0}, work ? work->size() : size_t{0}, max_len, a.utf8 ? 1 : 0, a.text_check ? 1 : 0,
            r.header.c_str(), r.guide.c_str(), r.initial.c_str());
    if (nxrt::config_has("applet", "swkbd_text")) {
        a.text = utf8_to_utf16(nxrt::config_get_str("applet", "swkbd_text", ""));
    } else if (prompt_flag().load()) {
        swkbd_prompt_async(ap);
        return;
    } else {
        a.text = utf8_to_utf16(r.initial.empty() ? std::string("Player") : r.initial);
    }
    if (a.text.size() > max_len) a.text.resize(max_len);
    swkbd_answer(a, true);
}

// The application's text check answer (SwkbdTextCheck). Success or Confirm submits the text. Failure or Silent: with the prompt the
// player is asked again (the game's message as the guide text); without it the same text would come back, so the keyboard cancels.
// The caller holds mu().
void swkbd_interactive(const std::shared_ptr<AppletState>& ap) {
    AppletState& a = *ap;
    if (a.interactive_in.empty() || !a.text_check || a.completed) return;
    std::shared_ptr<Bytes> d = a.interactive_in.front();
    a.interactive_in.pop_front();
    if (d->size() != kSwkbdTextCheckSize) NXLOG_W("am", "swkbd: text check storage is %zu bytes, 0x7D8 expected", d->size());
    const uint32_t check = read_at<uint32_t>(*d, 0, 1);
    if (check == kTextCheckSuccess || check == kTextCheckConfirm) {
        swkbd_submit(a, kSwkbdResultOk);
        return;
    }
    const std::string why = read_u16_text(*d, 4, kSwkbdStringSize / 2);
    NXLOG_W("am", "swkbd: the game rejected the text (text check result %u: \"%s\")", check, why.c_str());
    if (prompt_flag().load() && !nxrt::config_has("applet", "swkbd_text")) {
        if (!why.empty()) a.req.guide = why;
        a.req.initial = utf16_to_utf8(a.text);
        swkbd_prompt_async(ap);
    } else {
        swkbd_submit(a, kSwkbdResultCancel);
    }
}

// The player select (output: u64 result, then the Uid; 0x18 bytes). The user selector picks the one user at once; the other modes
// (network account and licence screens) answer that the player backed out, with no user.
void player_select_start(AppletState& a) {
    std::shared_ptr<Bytes> ui = a.in_data.size() > 1 ? a.in_data[1] : nullptr;
    const uint32_t mode = ui ? read_at<uint32_t>(*ui, 0, kPselModeUserSelector) : kPselModeUserSelector;
    auto out = std::make_shared<Bytes>(0x18, uint8_t{0});
    if (mode == kPselModeUserSelector) {
        uint64_t hi = 0, lo = 0;
        accapi::user_uid(&hi, &lo);
        std::memcpy(out->data() + 8, &hi, 8);
        std::memcpy(out->data() + 16, &lo, 8);
        NXLOG_I("am", "player select applet 0x%x (user selector): answering the one user", a.full_id);
    } else {
        const uint64_t r = kAccountCancelledByUser;
        std::memcpy(out->data(), &r, 8);
        NXLOG_I("am", "player select applet 0x%x (account mode %u: network account / licence screen): offline, answering cancelled by "
                "the player (0x27C)", a.full_id, mode);
    }
    push_out(a, out);
    applet_finish(a);
}

// The applets that end as cancelled by the player (GetResult LibraryAppletCanceled, exit reason 1), with what the log says about them.
const char* cancelled_applet(uint32_t id) {
    switch (id) {
    case kAppletIdShop: return "shop: no eShop here";
    case kAppletIdMyPage: return "friends (friend invitation): offline, no invitation sent";
    case kAppletIdDataErase: return "data management (save data does not fit): nothing freed";
    case kAppletIdNetConnect: return "network connection: offline, not resolved";
    case kAppletIdMatchmake: return "GameShare matchmaking: no guests here";
    default: return nullptr;
    }
}

// The error applet: in data 0 is CommonArguments, 1 the error argument; the first byte of that is its type (sdk nn::err, title A's sdk
// module: ShowError 0x38e580, ShowApplicationError 0x38ead0, ApplicationErrorArg 0x38fb9c..0x38fe60). Type 0 (ShowError(Result), 0x20
// bytes): the Result at 0x10. Type 4 (a pctl Result, 12 bytes): the Result at 8. Type 2 (ApplicationErrorArg, 0x1014 bytes): u32
// error code number at 8, language code char[8] at 0xC, dialog message char[0x800] at 0x14, full-screen message char[0x800] at 0x814.
// Nothing is shown: the error is logged and the applet ends as if the player closed it (no output, GetResult success).
std::string read_c_text(const Bytes& b, size_t off, size_t cap) {
    std::string t;
    for (size_t i = 0; i < cap && off + i < b.size(); ++i) {
        const char c = static_cast<char>(b[off + i]);
        if (c == 0) break;
        t.push_back(c == '\r' ? ' ' : c);
    }
    return t;
}

void error_applet_start(AppletState& a) {
    std::shared_ptr<Bytes> arg = a.in_data.size() > 1 ? a.in_data[1] : nullptr;
    if (!arg || arg->empty()) {
        NXLOG_I("am", "error applet: no error argument among the %zu storages pushed", a.in_data.size());
    } else {
        const uint8_t type = (*arg)[0];
        if (type == 0 || type == 4) {
            const uint32_t r = read_at<uint32_t>(*arg, type == 0 ? 0x10 : 8, 0);
            NXLOG_I("am", "error applet: the game shows error %04u-%04u (result 0x%x, argument type %u, %zu bytes)", 2000 + (r & 0x1FF),
                    (r >> 9) & 0x1FFF, r, type, arg->size());
        } else if (type == 2) {
            const uint32_t code = read_at<uint32_t>(*arg, 8, 0);
            const std::string lang = read_c_text(*arg, 0xC, 8);
            const std::string dialog = read_c_text(*arg, 0x14, 0x800);
            const std::string full = read_c_text(*arg, 0x814, 0x800);
            NXLOG_I("am", "error applet: the game shows application error %u (language \"%s\"): dialog \"%s\", full screen \"%s\"", code,
                    lang.c_str(), dialog.c_str(), full.c_str());
        } else {
            NXLOG_I("am", "error applet: argument type %u (%zu bytes), not decoded", type, arg->size());
        }
    }
    applet_finish(a);
}

// The bytes of the storage handed in (an IStorage this file made). Null when there is none or it is not one.
std::shared_ptr<Bytes> storage_bytes(const Request& req) {
    if (req.in_services.empty() || !req.in_services[0]) return nullptr;
    auto src = std::dynamic_pointer_cast<AmStorage>(req.in_services[0]);
    return src ? src->data : nullptr;
}

// Pops the oldest storage of `q` as a new IStorage; `ev` is cleared once `q` is empty.
uint32_t pop_storage(std::deque<std::shared_ptr<Bytes>>& q, ServiceEvent& ev, Response& rsp) {
    std::shared_ptr<Bytes> d;
    {
        std::lock_guard<std::mutex> lk(mu());
        if (!q.empty()) {
            d = q.front();
            q.pop_front();
        }
        if (q.empty()) ev.clear();
    }
    if (!d) return kNoData;
    rsp.push_object(new_storage(d));
    return 0;
}

// ILibraryAppletAccessor over one AppletState. Pushed storages are copied (the guest may reuse its storage afterwards).
ServicePtr make_library_applet_accessor(uint32_t full_id, uint32_t mode) {
    auto a = std::make_shared<AppletState>();
    a->full_id = full_id;
    a->id = full_id & 0xFFFF;
    a->mode = mode;
    return make_iface("ILibraryAppletAccessor", [a](Service& s) {
        s.add_cmd(0, "GetAppletStateChangedEvent", [a](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(a->state_event.new_handle());
            return 0;
        });
        s.add_cmd(1, "IsCompleted", [a](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint8_t>(a->completed ? 1 : 0);
            return 0;
        });
        s.add_cmd(10, "Start", [a](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            if (a->started) return 0;
            a->started = true;
            if (a->id == kAppletIdSwkbd) {
                swkbd_start(a);
            } else if (a->id == kAppletIdPlayerSelect) {
                player_select_start(*a);
            } else if (a->id == kAppletIdError) {
                error_applet_start(*a);
            } else if (const char* why = cancelled_applet(a->id)) {
                NXLOG_I("am", "library applet 0x%x started (%s): it ends as cancelled by the player", a->full_id, why);
                applet_finish(*a);
            } else {
                NXLOG_W("am", "library applet 0x%x started (no tested title was seen to launch it): it ends at once with no output", a->full_id);
                applet_finish(*a);
            }
            return 0;
        });
        // RequestExit / Terminate: the applet ends at once.
        s.add_cmd(20, "RequestExit", [a](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            if (!a->completed) applet_finish(*a);
            return 0;
        });
        s.add_cmd(25, "Terminate", [a](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            if (!a->completed) applet_finish(*a);
            return 0;
        });
        // The cancelled applets answer LibraryAppletCanceled (128-22: exit reason Canceled in nn::applet::JoinLibraryApplet, a failure
        // in nn::la::StartLibraryApplet), so nn::ec::detail::ShowShopPage does not read the 0x1010-byte return value it aborts without
        // and the others return their own "cancelled" result. Every other applet: success.
        s.add_cmd(30, "GetResult", [a](Request&, Response&) -> uint32_t {
            return cancelled_applet(a->id) ? kAppletCanceled : 0;
        });
        s.add_cmd(50, "SetOutOfFocusApplicationSuspendingEnabled", ignored("SetOutOfFocusApplicationSuspendingEnabled"));
        s.add_cmd(60, "PresetLibraryAppletGpuTimeSliceZero", ignored("PresetLibraryAppletGpuTimeSliceZero"));
        s.add_cmd(100, "PushInData", [a](Request& req, Response&) -> uint32_t {
            std::shared_ptr<Bytes> d = storage_bytes(req);
            if (!d) return kInvalidStorage;
            std::lock_guard<std::mutex> lk(mu());
            a->in_data.push_back(std::make_shared<Bytes>(*d));
            return 0;
        });
        s.add_cmd(101, "PopOutData", [a](Request&, Response& rsp) -> uint32_t {
            return pop_storage(a->out_data, a->out_event, rsp);
        });
        s.add_cmd(102, "PushExtraStorage", [](Request& req, Response&) -> uint32_t {
            return storage_bytes(req) ? 0 : kInvalidStorage;
        });
        s.add_cmd(103, "PushInteractiveInData", [a](Request& req, Response&) -> uint32_t {
            std::shared_ptr<Bytes> d = storage_bytes(req);
            if (!d) return kInvalidStorage;
            std::lock_guard<std::mutex> lk(mu());
            a->interactive_in.push_back(std::make_shared<Bytes>(*d));
            if (a->id == kAppletIdSwkbd && a->started) swkbd_interactive(a);
            return 0;
        });
        s.add_cmd(104, "PopInteractiveOutData", [a](Request&, Response& rsp) -> uint32_t {
            return pop_storage(a->interactive_out, a->interactive_out_event, rsp);
        });
        s.add_cmd(105, "GetPopOutDataEvent", [a](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(a->out_event.new_handle());
            return 0;
        });
        s.add_cmd(106, "GetPopInteractiveOutDataEvent", [a](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(a->interactive_out_event.new_handle());
            return 0;
        });
        s.add_cmd(110, "NeedsToExitProcess", ignored("NeedsToExitProcess", 1));
        s.add_cmd(120, "GetLibraryAppletInfo", [a](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(a->full_id);
            rsp.push<uint32_t>(a->mode);
            return 0;
        });
        s.add_cmd(150, "RequestForAppletToGetForeground", ignored("RequestForAppletToGetForeground"));
        // platform 2: one input bool, no output (sdk proxy at 0x75001bd560). Name and meaning unknown; accepted.
        s.add_cmd(90, "Cmd90", ignored("ILibraryAppletAccessor Cmd90"));
    });
}

// CreateLibraryApplet(u32 applet id, u32 library applet mode) and CreateLibraryAppletEx (the same two words first). The inline
// keyboard (a partial-foreground mode) is refused: it talks a different protocol the game would wait on forever.
uint32_t create_library_applet(const char* cmd, Request& req, Response& rsp) {
    const uint32_t id = req.arg<uint32_t>(0);
    const uint32_t mode = req.arg<uint32_t>(4);
    if ((id & 0xFFFF) == kAppletIdSwkbd && mode != kAppletModeAllForeground) {
        NXLOG_W("am", "%s(applet 0x%x, mode %u): the inline software keyboard is not supported", cmd, id, mode);
        return kLibraryAppletFailed;
    }
    NXLOG_I("am", "%s(applet 0x%x, mode %u)", cmd, id, mode);
    rsp.push_object(make_library_applet_accessor(id, mode));
    return 0;
}

// The guest bytes behind a transfer memory handle, copied (the keyboard's work buffer: the initial text). Zeros when unreadable.
std::shared_ptr<Bytes> transfer_memory_bytes(const Request& req, int64_t size) {
    auto d = std::make_shared<Bytes>(static_cast<size_t>(size), uint8_t{0});
    if (!req.in_copy.empty())
        if (auto tm = kernel::handle_as<kernel::KTransferMemory>(req.in_copy[0]))
            if (tm->addr && size > 0 && static_cast<uint64_t>(size) <= tm->size && mem::is_mapped(tm->addr, static_cast<uint64_t>(size)))
                std::memcpy(d->data(), reinterpret_cast<const void*>(tm->addr), static_cast<size_t>(size));
    return d;
}

// ILibraryAppletCreator: the library applets above, and the storages handed to them.
ServicePtr make_library_applet_creator() {
    return make_iface("ILibraryAppletCreator", [](Service& s) {
        s.add_cmd(0, "CreateLibraryApplet", [](Request& req, Response& rsp) -> uint32_t {
            return create_library_applet("CreateLibraryApplet", req, rsp);
        });
        s.add_cmd(3, "CreateLibraryAppletEx", [](Request& req, Response& rsp) -> uint32_t {
            return create_library_applet("CreateLibraryAppletEx", req, rsp);
        });
        s.add_cmd(10, "CreateStorage", [](Request& req, Response& rsp) -> uint32_t {
            const int64_t size = req.arg<int64_t>(0);
            if (!storage_size_ok(size)) return kOutOfMemory;
            rsp.push_object(new_storage(std::make_shared<Bytes>(static_cast<size_t>(size), uint8_t{0})));
            return 0;
        });
        // Transfer and handle storages: a host copy of the guest memory behind the handle (taken now: the keyboard reads its work buffer).
        s.add_cmd(11, "CreateTransferMemoryStorage", [](Request& req, Response& rsp) -> uint32_t {
            const int64_t size = req.arg<int64_t>(8);
            if (!storage_size_ok(size)) return kOutOfMemory;
            rsp.push_object(new_storage(transfer_memory_bytes(req, size)));
            close_copies(req);
            return 0;
        });
        s.add_cmd(12, "CreateHandleStorage", [](Request& req, Response& rsp) -> uint32_t {
            const int64_t size = req.arg<int64_t>(0);
            if (!storage_size_ok(size)) return kOutOfMemory;
            rsp.push_object(new_storage(transfer_memory_bytes(req, size)));
            close_copies(req);
            return 0;
        });
    });
}

// The preselected user launch parameter: the 0x88-byte LaunchParameterAccountPreselectedUser (magic, flag 1, the user id as the
// Uid's low word then high word). The user is the one the account service reports.
ServicePtr preselected_user_storage() {
    uint64_t hi = 0, lo = 0;
    accapi::user_uid(&hi, &lo);
    auto d = std::make_shared<Bytes>(static_cast<size_t>(kLaunchParamsSize), uint8_t{0});
    const uint32_t magic = kLaunchParamsMagic, flag = 1;
    std::memcpy(d->data() + 0, &magic, 4);
    std::memcpy(d->data() + 4, &flag, 4);
    std::memcpy(d->data() + 8, &hi, 8);    // the Uid in memory order, as acc:u0 carries it (first u64 = profile uid_hi)
    std::memcpy(d->data() + 16, &lo, 8);
    return new_storage(d);
}

// The language the game is shown in: the system's when the game supports it, else the first language the game supports.
Language desired_language() {
    const TitleProfile& p = profile();
    const uint32_t mask = p.supported_languages;
    const uint32_t lang = static_cast<uint32_t>(p.language);
    if (mask == 0 || ((mask >> lang) & 1u)) return p.language;
    for (uint32_t i = 0; i < static_cast<uint32_t>(Language::Count); ++i)
        if ((mask >> i) & 1u) return static_cast<Language>(i);
    return p.language;
}

// IApplicationFunctions: launch parameters, save data sizes, the language and display version, and the events that never fire.
ServicePtr make_application_functions() {
    return make_iface("IApplicationFunctions", [](Service& s) {
        s.add_cmd(1, "PopLaunchParameter", [](Request& req, Response& rsp) -> uint32_t {
            const uint32_t kind = req.arg<uint32_t>(0);
            if (kind == kLaunchParamUserChannel) {
                std::shared_ptr<Bytes> d;
                {
                    std::lock_guard<std::mutex> lk(mu());
                    if (!st().user_channel.empty()) {
                        d = st().user_channel.back();
                        st().user_channel.pop_back();
                    }
                }
                if (!d) return kNoData;
                rsp.push_object(new_storage(d));
                return 0;
            }
            if (kind == kLaunchParamPreselectedUser) {
                bool popped;
                {
                    std::lock_guard<std::mutex> lk(mu());
                    popped = st().preselected_popped;
                    st().preselected_popped = true;
                }
                if (popped) return kNoData;
                rsp.push_object(preselected_user_storage());
                return 0;
            }
            return kNoData;
        });
        // EnsureSaveData(Uid) -> u64 required size: the account save data of the title, created when it is missing.
        s.add_cmd(20, "EnsureSaveData", [](Request& req, Response& rsp) -> uint32_t {
            const uint64_t hi = req.arg<uint64_t>(0);   // Uid in memory order: first u64 = hi
            const uint64_t lo = req.arg<uint64_t>(8);
            bool created = false;
            const uint32_t rc = fsapi::ensure_account_save(hi, lo, &created);
            rsp.push<uint64_t>(0);   // nothing is missing: the space and the save are there
            return rc;
        });
        s.add_cmd(21, "GetDesiredLanguage", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(language_code_u64(desired_language()));
            return 0;
        });
        s.add_cmd(22, "SetTerminateResult", ignored("SetTerminateResult"));
        // platform 2 commands with no input (sdk proxies 0x75001bd9f8 / 0x75001bda04): 320 returns two bytes, 321 one byte. Names unknown;
        // zeros (false) is what boot ran on, and nothing reads them as an object.
        s.add_cmd(320, "Cmd320", ignored("IApplicationFunctions Cmd320", 2));
        s.add_cmd(321, "Cmd321", ignored("IApplicationFunctions Cmd321", 1));
        s.add_cmd(23, "GetDisplayVersion", [](Request&, Response& rsp) -> uint32_t {
            const std::string& v = profile().display_version;
            push_bytes(rsp, v.data(), v.size() < 15 ? v.size() : 15, 16);   // NUL-terminated 16-byte string
            return 0;
        });
        // ExtendSaveData(type, Uid, normal, journal) -> i64 required size. Only the account save data is kept (its size is stored).
        s.add_cmd(25, "ExtendSaveData", [](Request& req, Response& rsp) -> uint32_t {
            const uint8_t type = req.arg<uint8_t>(0);
            const int64_t normal = req.arg<int64_t>(24);
            const int64_t journal = req.arg<int64_t>(32);
            if (type == kSaveDataTypeAccount) {
                std::lock_guard<std::mutex> lk(mu());
                st().save_extended = true;
                st().save_normal = normal;
                st().save_journal = journal;
            }
            rsp.push<int64_t>(0);
            return 0;
        });
        // GetSaveDataSize(type, Uid) -> (i64 normal, i64 journal): the stored extension, else the fs account save data.
        s.add_cmd(26, "GetSaveDataSize", [](Request& req, Response& rsp) -> uint32_t {
            const uint8_t type = req.arg<uint8_t>(0);
            const uint64_t hi = req.arg<uint64_t>(8);
            const uint64_t lo = req.arg<uint64_t>(16);
            int64_t normal = 0, journal = 0;
            uint32_t rc = 0;
            if (type == kSaveDataTypeAccount) {
                bool extended;
                {
                    std::lock_guard<std::mutex> lk(mu());
                    extended = st().save_extended;
                    normal = st().save_normal;
                    journal = st().save_journal;
                }
                if (!extended) rc = fsapi::account_save_size(hi, lo, &normal, &journal);
            }
            rsp.push<int64_t>(normal);
            rsp.push<int64_t>(journal);
            return rc;
        });
        s.add_cmd(27, "CreateCacheStorage", ignored("CreateCacheStorage", 16));
        s.add_cmd(28, "GetSaveDataSizeMax", ignored("GetSaveDataSizeMax", 16));
        s.add_cmd(32, "BeginBlockingHomeButton", ignored("BeginBlockingHomeButton"));
        s.add_cmd(33, "EndBlockingHomeButton", ignored("EndBlockingHomeButton"));
        s.add_cmd(34, "SelectApplicationLicense", ignored("SelectApplicationLicense", 1));
        s.add_cmd(35, "GetDeviceSaveDataSizeMax", ignored("GetDeviceSaveDataSizeMax", 16));
        s.add_cmd(37, "GetLimitedApplicationLicenseUpgradableEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(never_event().new_handle());
            return 0;
        });
        s.add_cmd(40, "NotifyRunning", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(1);
            return 0;
        });
        // GetPseudoDeviceId -> the 16-byte nn::util::Uuid. A fixed value ("NXRT-DEVICE-0001") on every run.
        s.add_cmd(50, "GetPseudoDeviceId", [](Request&, Response& rsp) -> uint32_t {
            static const char kId[16] = {'N', 'X', 'R', 'T', '-', 'D', 'E', 'V', 'I', 'C', 'E', '-', '0', '0', '0', '1'};
            push_bytes(rsp, kId, sizeof kId, sizeof kId);
            return 0;
        });
        s.add_cmd(60, "SetMediaPlaybackStateForApplication", ignored("SetMediaPlaybackStateForApplication"));
        // GamePlay recording is not available: it reports unsupported and takes the calls that set it up.
        s.add_cmd(65, "IsGamePlayRecordingSupported", ignored("IsGamePlayRecordingSupported", 1));
        s.add_cmd(66, "InitializeGamePlayRecording", [](Request& req, Response&) -> uint32_t {
            close_copies(req);
            NXLOG_D("am", "InitializeGamePlayRecording: accepted, nothing recorded");
            return 0;
        });
        s.add_cmd(67, "SetGamePlayRecordingState", ignored("SetGamePlayRecordingState"));
        s.add_cmd(68, "RequestFlushGamePlayingMovieForDebug", ignored("RequestFlushGamePlayingMovieForDebug"));
        s.add_cmd(90, "EnableApplicationCrashReport", ignored("EnableApplicationCrashReport"));
        s.add_cmd(100, "InitializeApplicationCopyrightFrameBuffer", [](Request& req, Response&) -> uint32_t {
            close_copies(req);
            NXLOG_D("am", "InitializeApplicationCopyrightFrameBuffer: accepted, no overlay");
            return 0;
        });
        s.add_cmd(101, "SetApplicationCopyrightImage", ignored("SetApplicationCopyrightImage"));
        s.add_cmd(110, "QueryApplicationPlayStatistics", ignored("QueryApplicationPlayStatistics", 4));
        s.add_cmd(120, "ExecuteProgram", ignored("ExecuteProgram"));
        s.add_cmd(121, "ClearUserChannel", [](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            st().user_channel.clear();
            return 0;
        });
        // UnpopToUserChannel(IStorage): a copy of the storage goes on the user channel, to be popped by PopLaunchParameter(UserChannel).
        s.add_cmd(122, "UnpopToUserChannel", [](Request& req, Response&) -> uint32_t {
            if (req.in_services.empty() || !req.in_services[0]) return 0;
            auto src = std::dynamic_pointer_cast<AmStorage>(req.in_services[0]);
            if (!src) return kInvalidStorage;
            auto copy = std::make_shared<Bytes>(*src->data);
            std::lock_guard<std::mutex> lk(mu());
            st().user_channel.push_back(copy);
            return 0;
        });
        s.add_cmd(123, "GetPreviousProgramIndex", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(-1);   // launched by the system, not by another of the title's programs
            return 0;
        });
        s.add_cmd(124, "EnableApplicationAllThreadDumpOnCrash", ignored("EnableApplicationAllThreadDumpOnCrash"));
        s.add_cmd(130, "GetGpuErrorDetectedSystemEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(never_event().new_handle());
            return 0;
        });
        // Friend invitations: nn::friends::GetFriendInvitationNotificationEvent (sdk 0x75003d8968, vtable slot 0x1B0 = cmd 140; it aborts
        // if the command fails) and TryPopFriendInvitationNotificationInfo (0x75003d89f0, slot 0x1B8 = cmd 141; 128-2 means "none",
        // anything else but success aborts). Offline: an event that never fires and nothing to pop.
        s.add_cmd(140, "GetFriendInvitationStorageChannelEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(never_event().new_handle());
            return 0;
        });
        s.add_cmd(141, "TryPopFromFriendInvitationStorageChannel", [](Request&, Response&) -> uint32_t { return kNoData; });
        s.add_cmd(150, "GetNotificationStorageChannelEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(never_event().new_handle());
            return 0;
        });
        s.add_cmd(151, "TryPopFromNotificationStorageChannel", [](Request&, Response&) -> uint32_t { return kNoData; });
        s.add_cmd(160, "GetHealthWarningDisappearedSystemEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(never_event().new_handle());
            return 0;
        });
        s.add_cmd(170, "SetHdcpAuthenticationActivated", ignored("SetHdcpAuthenticationActivated"));
        s.add_cmd(210, "GetUnknownEvent210", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(never_event().new_handle());
            return 0;
        });
    });
}

// IApplicationProxy: the interfaces of one application. Display, process-winding and debug functions are empty here.
ServicePtr make_application_proxy() {
    return make_iface("IApplicationProxy", [](Service& s) {
        s.add_cmd(0, "GetCommonStateGetter", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_common_state_getter());
            return 0;
        });
        s.add_cmd(1, "GetSelfController", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_self_controller());
            return 0;
        });
        s.add_cmd(2, "GetWindowController", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_window_controller());
            return 0;
        });
        s.add_cmd(3, "GetAudioController", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_audio_controller());
            return 0;
        });
        s.add_cmd(4, "GetDisplayController", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("IDisplayController", [](Service&) {}));
            return 0;
        });
        // platform 2 (switchbrew [S2]): 40 IPerformanceFunctions, 60 IGraphicsScalingParametersFunctions. Commands unknown so far:
        // stub objects that log what the game asks of them.
        s.add_cmd(40, "GetPerformanceFunctions", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(std::make_shared<ipc::StubService>("IPerformanceFunctions"));
            return 0;
        });
        s.add_cmd(60, "GetGraphicsScalingParametersFunctions", [](Request&, Response& rsp) -> uint32_t {
            // nn::oe::GetCurrentGraphicsScale (sdk) maps the first word of cmd 10 to a scale level: 10 -> 0, 20 -> 1, 30 -> 2 (anything else
            // aborts). Level 0 is the console's full-quality one: title A's dynamic resolution (main 0x710022d338 / 0x710022cd20) tops out
            // at its level 30 (1920x1080) at scale 0 docked, 24 (1728x972) at scale 1 and 22 (1664x936) at scale 2. [am] graphics_scale
            // (10, 20 or 30) picks another answer.
            rsp.push_object(make_iface("IGraphicsScalingParametersFunctions", [](Service& g) {
                g.add_cmd(10, "GetGraphicsScalingParameters", [](Request&, Response& r) -> uint32_t {
                    static const uint32_t answer = [] {
                        const int64_t v = nxrt::config_get_int("am", "graphics_scale", 10);
                        return (v == 20 || v == 30) ? static_cast<uint32_t>(v) : 10u;
                    }();
                    r.push<uint32_t>(answer);
                    r.push<uint32_t>(0);
                    r.push<uint64_t>(0);
                    return 0;
                });
                g.add_cmd(1050, "SetGraphicsScalingNotification (S2)", [](Request&, Response&) -> uint32_t { return 0; });
            }));
            return 0;
        });
        s.add_cmd(10, "GetProcessWindingController", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("IProcessWindingController", [](Service&) {}));
            return 0;
        });
        s.add_cmd(11, "GetLibraryAppletCreator", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_library_applet_creator());
            return 0;
        });
        s.add_cmd(20, "GetApplicationFunctions", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_application_functions());
            return 0;
        });
        s.add_cmd(1000, "GetDebugFunctions", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("IDebugFunctions", [](Service&) {}));
            return 0;
        });
    });
}

// appletOE: IApplicationProxyService, OpenApplicationProxy(pid, process copy). appletAE: IAllSystemAppletProxiesService, whose
// OpenSystemApplicationProxy (350) answers the same proxy.
ServicePtr make_applet_service(const char* name, bool is_system) {
    return make_iface(name, [is_system](Service& s) {
        const uint32_t open_cmd = is_system ? 350u : 0u;
        s.add_cmd(open_cmd, is_system ? "OpenSystemApplicationProxy" : "OpenApplicationProxy",
                  [](Request& req, Response& rsp) -> uint32_t {
                      close_copies(req);   // the process handle is not kept
                      rsp.push_object(make_application_proxy());
                      return 0;
                  });
        if (!is_system)   // platform 2 SDKs open the proxy with OpenApplicationProxy2 (10); same answer
            s.add_cmd(10, "OpenApplicationProxy2", [](Request& req, Response& rsp) -> uint32_t {
                close_copies(req);
                rsp.push_object(make_application_proxy());
                return 0;
            });
    });
}

}  // namespace

void register_am() {
    add("appletOE", [] { return make_applet_service("appletOE", false); });
    add("appletAE", [] { return make_applet_service("appletAE", true); });
}

void am_reset() {
    std::lock_guard<std::mutex> lk(mu());
    st() = fresh_state();
}

namespace swkbdui {
void enable_prompt(bool on) { prompt_flag().store(on); }
bool prompt_enabled() { return prompt_flag().load(); }
}  // namespace swkbdui

namespace amctl {

void set_docked(bool docked) {
    std::lock_guard<std::mutex> lk(mu());
    st().docked = docked;
    if (st().operation_notify) queue_message(kMsgOperationModeChanged);
    if (st().performance_notify) queue_message(kMsgPerformanceModeChanged);
}

void set_focus(bool in_focus) {
    std::lock_guard<std::mutex> lk(mu());
    st().focus = in_focus ? kFocusInFocus : kFocusOutOfFocus;
    queue_message(kMsgFocusStateChanged);
}

bool docked() {
    std::lock_guard<std::mutex> lk(mu());
    return st().docked;
}

bool in_focus() {
    std::lock_guard<std::mutex> lk(mu());
    return st().focus == kFocusInFocus;
}

}  // namespace amctl

}  // namespace nxrt::services
