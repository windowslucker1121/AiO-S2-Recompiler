// SPDX-License-Identifier: GPL-3.0-or-later
// Small Horizon services of the title's Service Access Control list (S3c): csrng, ldr:ro, mm:u, hwopus, aoc:u, caps:u, caps:su,
// mii:u, irs, nfc:user, nfc:mf:u, nfp:user, ectx:aw, pcm, banana, htc, htc:tenv, htcs, olsc:u.
//
// Adapted from ScarletNX src/nx/hle/services/misc.swipc, ldr.swipc and misc.c (GPL-3.0-or-later): the command ids and argument
// layouts of csrng, mm:u, aoc:u, irs, mii:u, nfc/nfp, ectx:aw and ldr:ro, and the no-device result constants.
// Cross-checked against Switchbrew (https://switchbrew.org/wiki/Services_API and the per-service pages: RO services, SPL services,
// Audio services (hwopus), Capture services, NFC services, TMA services, Error_codes), and Ryubing (MIT) src/Ryujinx.HLE/HOS/Services
// (Ro/IRoInterface.cs, Spl/IRandomInterface.cs, Caps/*, Ectx/*, Ptm/Pcm/IManager.cs, Ns/Aoc/IAddOnContentManager.cs, Hid/Irs/
// IIrSensorServer.cs, Mii/StaticService/IDatabaseService.cs, Nfc/*).
//
// Every command in a table has a handler: accepted-and-ignored (logged at Debug), an error result, or a real answer. No table command
// falls through to kStubService. Two services have no table in the sources read, so their commands do fall through: pcm, and the IUser
// that nfc:mf:u hands out. "banana" is a StubService by design.
#include <atomic>
#include <map>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/tt.h"
#include "services_internal.h"

#include <windows.h>
#include <bcrypt.h>

#if NXRT_HAVE_OPUS
#include <opus/opus.h>
#endif

#pragma comment(lib, "bcrypt")

namespace nxrt::services {

namespace {

using ipc::Handler;
using ipc::Request;
using ipc::Response;
using ipc::Service;

// ---- result codes ---------------------------------------------------------------------------------------------------------------
// ro NotLoaded (module 22, desc 1028 = 0x80816): Switchbrew Error_codes.
constexpr uint32_t kRoNotLoaded = res(22, 1028);
// Sf_Hipc NotSupported (module 11, desc 1 = 0x20B): Switchbrew Error_codes. Used where the real console's code is unknown.
constexpr uint32_t kNotSupported = res(11, 1);
// No device: ScarletNX misc.c NFP_DEVICE_NOT_FOUND / NFC_DEVICE_NOT_FOUND (modules 115 Nfp and 127 Nfc, names from Switchbrew). Not verified
// against Switchbrew's listing, which stops before those modules.
constexpr uint32_t kNfpNoDevice = res(115, 64);
constexpr uint32_t kNfcNoDevice = res(127, 64);
constexpr uint32_t kOlscNullArgument = res(179, 100);   // Ryubing Olsc/ResultCode.cs
constexpr uint32_t kOlscNotInitialized = res(179, 101);
// Mii: ScarletNX misc.c MII_NOT_FOUND / MII_INVALID_ARGUMENT (module 126 Mii). Not verified against Switchbrew.
constexpr uint32_t kMiiNotFound = res(126, 4);
constexpr uint32_t kMiiInvalidArgument = res(126, 1);
// aoc: ScarletNX misc.c AOC_NO_PURCHASED_PRODUCT (module 164 = Ec, per Switchbrew) and AOC_NOT_CONNECTED (module 110 = Nifm).
constexpr uint32_t kAocNoPurchase = res(164, 400);
constexpr uint32_t kAocNotConnected = res(110, 300);

// Sizes
constexpr uint64_t kCharInfoSize = 0x58;          // nn::mii::CharInfo (ScarletNX misc.swipc)
constexpr uint64_t kAocBaseIdOffset = 0x1000;     // add-on content base id = application id + 0x1000
constexpr uint32_t kOpusWorkBufferSize = 0x8000;  // fixed size reported by GetWorkBufferSize* (page aligned, as Switchbrew says)
constexpr uint64_t kIrsSharedMemorySize = 0x1000; // size of the IR sensor shared memory handed out by GetIrsensorSharedMemoryHandle
constexpr size_t kMmSlots = 16;                    // clock requests kept at once (ScarletNX misc.c keeps a fixed table too)

// ---- helpers ---------------------------------------------------------------------------------------------------------------------
void zero_buffer(const ipc::Buffer& b) {
    if (b.addr && b.size) std::memset(buf_mut(b), 0, static_cast<size_t>(b.size));
}

// A command that is accepted and ignored (logged at Debug).
Handler accept(const char* tag, const char* what) {
    return [tag, what](Request& req, Response&) -> uint32_t {
        NXLOG_D(tag, "%s (cmd %u) accepted and ignored", what, req.cmd);
        return 0;
    };
}

// A command that answers with an error result (logged at Warn).
Handler refuse(uint32_t rc, const char* tag, const char* what) {
    return [rc, tag, what](Request& req, Response&) -> uint32_t {
        NXLOG_W(tag, "%s (cmd %u) answered with 0x%X", what, req.cmd, rc);
        return rc;
    };
}

// Events held for the process (signalled never: none of these services has an event to raise).
struct MiscEvents {
    ServiceEvent aoc_list_changed;
    ServiceEvent aoc_purchased;
    ServiceEvent nfc_available, nfc_activate, nfc_deactivate;
    ServiceEvent nfp_available, nfp_activate, nfp_deactivate;
};
MiscEvents& events() {
    static MiscEvents e;
    return e;
}

// ---- csrng ----------------------------------------------------------------------------------------------------------------------
// GenerateRandomBytes(buffer<u8, 6> data): fills the output buffer from the Windows CSPRNG.
std::shared_ptr<Service> make_csrng() {
    return make_iface("csrng", [](Service& s) {
        s.add_cmd(0, "GenerateRandomBytes", [](Request& req, Response&) -> uint32_t {
            const ipc::Buffer b = req.out_buf(0);
            uint64_t off = 0;
            if (tt::deterministic()) {                       // test tooling: a seeded stream, so replays see the same random numbers
                tt::rng_fill(buf_mut(b), static_cast<size_t>(b.size));
                return 0;
            }
            while (off < b.size) {
                const uint64_t n = std::min<uint64_t>(b.size - off, 0x10000000ull);
                const NTSTATUS st = BCryptGenRandom(nullptr, buf_mut(b) + off, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
                if (!BCRYPT_SUCCESS(st)) {
                    NXLOG_E("csrng", "BCryptGenRandom failed: 0x%lX", static_cast<unsigned long>(st));
                    return kNotSupported;
                }
                off += n;
            }
            NXLOG_D("csrng", "GenerateRandomBytes: %llu bytes", static_cast<unsigned long long>(b.size));
            return 0;
        });
    });
}

// ---- ldr:ro -----------------------------------------------------------------------------------------------------------------------
// The title loads no NRO. RegisterProcessHandle (cmd 4) is what the SDK sends first (the "Initialize" of nn::ro); the module-loading
// commands answer NotLoaded.
std::shared_ptr<Service> make_ro() {
    return make_iface("IRoInterface", [](Service& s) {
        s.add_cmd(0, "MapManualLoadModuleMemory", refuse(kRoNotLoaded, "ro", "MapManualLoadModuleMemory"));
        s.add_cmd(1, "UnmapManualLoadModuleMemory", refuse(kRoNotLoaded, "ro", "UnmapManualLoadModuleMemory"));
        s.add_cmd(2, "RegisterModuleInfo", refuse(kRoNotLoaded, "ro", "RegisterModuleInfo"));
        s.add_cmd(3, "UnregisterModuleInfo", refuse(kRoNotLoaded, "ro", "UnregisterModuleInfo"));
        s.add_cmd(4, "RegisterProcessHandle", accept("ro", "RegisterProcessHandle"));
        s.add_cmd(10, "RegisterModuleInfoWithUserProcessHandle", refuse(kRoNotLoaded, "ro", "RegisterModuleInfoWithUserProcessHandle"));
    });
}

// ---- mm:u -------------------------------------------------------------------------------------------------------------------------
// Multimedia clock requests. The old forms (0 to 3) name the module; the new ones (4 to 7) return a request id. The rate is kept per
// request and read back. Nothing is clocked here.
struct MmSlot {
    bool used = false;
    bool by_module = false;   // the old forms key the slot by module, the new ones by request id
    uint32_t key = 0;
    uint32_t module = 0;
    uint32_t current = 0;
};
struct MmState {
    std::mutex mu;
    uint32_t next_id = 0;
    MmSlot slots[kMmSlots];
};
MmState& mm_state() {
    static MmState s;
    return s;
}
MmSlot* mm_find(MmState& s, bool by_module, uint32_t key) {
    for (auto& sl : s.slots)
        if (sl.used && sl.by_module == by_module && sl.key == key) return &sl;
    return nullptr;
}
// Keeps a request. The client gets its id even when no slot is free (it then reads back 0), as ScarletNX does.
uint32_t mm_open(uint32_t module, bool by_module, uint32_t module_for_id) {
    MmState& s = mm_state();
    std::lock_guard<std::mutex> g(s.mu);
    const uint32_t id = ++s.next_id;
    const uint32_t key = by_module ? module : id;
    bool kept = false;
    for (auto& sl : s.slots) {
        if (sl.used) continue;
        sl.used = true;
        sl.by_module = by_module;
        sl.key = key;
        sl.module = by_module ? module : module_for_id;
        sl.current = 0;
        kept = true;
        break;
    }
    if (!kept) NXLOG_W("mm", "more than %zu clock requests, request %u is not kept", kMmSlots, id);
    return by_module ? 0 : id;
}
void mm_close(bool by_module, uint32_t key) {
    MmState& s = mm_state();
    std::lock_guard<std::mutex> g(s.mu);
    if (MmSlot* sl = mm_find(s, by_module, key)) sl->used = false;
}
void mm_set(bool by_module, uint32_t key, uint32_t value) {
    MmState& s = mm_state();
    std::lock_guard<std::mutex> g(s.mu);
    if (MmSlot* sl = mm_find(s, by_module, key)) sl->current = value;
}
uint32_t mm_get(bool by_module, uint32_t key) {
    MmState& s = mm_state();
    std::lock_guard<std::mutex> g(s.mu);
    const MmSlot* sl = mm_find(s, by_module, key);
    return sl ? sl->current : 0;
}

std::shared_ptr<Service> make_mm() {
    return make_iface("mmnv::IRequest", [](Service& s) {
        // Old forms: module at +0, then priority / min / max at +4 / +8.
        s.add_cmd(0, "InitializeOld", [](Request& req, Response&) -> uint32_t {
            mm_open(req.arg<uint32_t>(0), true, 0);
            NXLOG_D("mm", "InitializeOld module %u", req.arg<uint32_t>(0));
            return 0;
        });
        s.add_cmd(1, "FinalizeOld", [](Request& req, Response&) -> uint32_t {
            mm_close(true, req.arg<uint32_t>(0));
            return 0;
        });
        s.add_cmd(2, "SetAndWaitOld", [](Request& req, Response&) -> uint32_t {
            mm_set(true, req.arg<uint32_t>(0), req.arg<uint32_t>(4));
            return 0;
        });
        s.add_cmd(3, "GetOld", [](Request& req, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(mm_get(true, req.arg<uint32_t>(0)));
            return 0;
        });
        // New forms: request id returned by Initialize, used by the rest.
        s.add_cmd(4, "Initialize", [](Request& req, Response& rsp) -> uint32_t {
            const uint32_t id = mm_open(req.arg<uint32_t>(0), false, req.arg<uint32_t>(0));
            NXLOG_D("mm", "Initialize module %u -> request %u", req.arg<uint32_t>(0), id);
            rsp.push<uint32_t>(id);
            return 0;
        });
        s.add_cmd(5, "Finalize", [](Request& req, Response&) -> uint32_t {
            mm_close(false, req.arg<uint32_t>(0));
            return 0;
        });
        s.add_cmd(6, "SetAndWait", [](Request& req, Response&) -> uint32_t {
            mm_set(false, req.arg<uint32_t>(0), req.arg<uint32_t>(4));
            return 0;
        });
        s.add_cmd(7, "Get", [](Request& req, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(mm_get(false, req.arg<uint32_t>(0)));
            return 0;
        });
    });
}

// ---- hwopus -----------------------------------------------------------------------------------------------------------------------
// Decoded with libopus when the runtime has it (NXRT_HAVE_OPUS, runtime/cmake/media_deps.cmake). Without it the decoders open and
// "decode" to silence of the right length, so titles that need one (title C opens one at boot and aborts on a refusal) run
// with their Opus voices muted; the length comes from the packet's TOC byte (RFC 6716 section 3.1). Input: the platform's 8-byte header
// (u32 big-endian payload size, u32 final range) then the Opus packet. Output: s16 interleaved.

// Samples per channel at `rate` of one Opus packet (0 when the header or TOC is unusable).
uint32_t opus_packet_samples(const uint8_t* p, size_t n, uint32_t rate) {
    if (n < 1) return 0;
    const uint32_t config = p[0] >> 3;
    // frame duration in units of 1/400 s (2.5 ms)
    static const uint32_t kSilk[4] = {4, 8, 16, 24}, kHybrid[2] = {4, 8}, kCelt[4] = {1, 2, 4, 8};
    const uint32_t q = config < 12 ? kSilk[config & 3] : config < 16 ? kHybrid[config & 1] : kCelt[config & 3];
    uint32_t frames = 1;
    switch (p[0] & 3) {
        case 0: frames = 1; break;
        case 1: case 2: frames = 2; break;
        default: if (n < 2) return 0; frames = p[1] & 0x3f; break;
    }
    return frames * q * rate / 400;
}

// One libopus decoder per session (a session is used by one guest thread at a time; the lock is for safety only).
struct HostOpus {
    std::mutex mu;
#if NXRT_HAVE_OPUS
    OpusDecoder* dec = nullptr;
    ~HostOpus() { if (dec) opus_decoder_destroy(dec); }
#endif
};

std::shared_ptr<Service> make_hwopus_decoder(uint32_t rate, uint32_t channels) {
    if (rate == 0) rate = 48000;
    if (channels == 0 || channels > 8) channels = 2;
    auto host = std::make_shared<HostOpus>();
#if NXRT_HAVE_OPUS
    int err = 0;
    if (channels <= 2) host->dec = opus_decoder_create(static_cast<opus_int32>(rate), static_cast<int>(channels), &err);
    if (!host->dec) NXLOG_W("hwopus", "opus_decoder_create(%u Hz, %u channels) failed (%d): this decoder decodes silence", rate, channels, err);
#endif
    return make_iface("IHardwareOpusDecoder", [rate, channels, host](Service& s) {
        // Decode*: consumed bytes, samples per channel [, performance time]. The forms "with reset" (6..9) take a bool reset first.
        auto decode = [rate, channels, host](bool perf, bool has_reset) -> Handler {
            return [rate, channels, host, perf, has_reset](Request& req, Response& rsp) -> uint32_t {
                const ipc::Buffer in = req.in_buf(0), out = req.out_buf(0);
                const uint8_t* src = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(in.addr));
                uint32_t consumed = 0, samples = 0;
                bool decoded = false;
                if (src && in.size >= 8) {
                    const uint32_t size = (uint32_t(src[0]) << 24) | (uint32_t(src[1]) << 16) | (uint32_t(src[2]) << 8) | src[3];
                    if (size <= in.size - 8) {
                        consumed = 8 + size;
                        samples = opus_packet_samples(src + 8, size, rate);
#if NXRT_HAVE_OPUS
                        std::lock_guard<std::mutex> lk(host->mu);
                        if (host->dec && out.addr && out.size >= channels * 2) {
                            if (has_reset && req.arg<uint8_t>(0)) opus_decoder_ctl(host->dec, OPUS_RESET_STATE);
                            const int frames = opus_decode(host->dec, src + 8, static_cast<opus_int32>(size),
                                                           reinterpret_cast<opus_int16*>(static_cast<uintptr_t>(out.addr)),
                                                           static_cast<int>(out.size / (channels * 2)), 0);
                            if (frames >= 0) {
                                samples = static_cast<uint32_t>(frames);
                                decoded = true;
                            } else {
                                static std::atomic<int> logs{0};
                                if (logs.fetch_add(1) < 4) NXLOG_W("hwopus", "opus_decode of a %u-byte packet failed (%d): silence", size, frames);
                            }
                        }
#endif
                    }
                }
                if (!decoded && out.addr && out.size) {
                    const uint64_t bytes = std::min<uint64_t>(out.size, uint64_t(samples) * channels * 2);
                    samples = static_cast<uint32_t>(bytes / (channels * 2));
                    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(out.addr)), 0, static_cast<size_t>(out.size));
                }
                rsp.push<uint32_t>(consumed);
                rsp.push<uint32_t>(samples);
                if (perf) rsp.push<uint64_t>(0);
                return 0;
            };
        };
        s.add_cmd(0, "DecodeInterleavedOld", decode(false, false));
        s.add_cmd(1, "SetContext", [](Request&, Response&) -> uint32_t { return 0; });
        s.add_cmd(2, "DecodeInterleavedForMultiStreamOld", decode(false, false));
        s.add_cmd(3, "SetContextForMultiStream", [](Request&, Response&) -> uint32_t { return 0; });
        s.add_cmd(4, "DecodeInterleavedWithPerfOld", decode(true, false));
        s.add_cmd(5, "DecodeInterleavedForMultiStreamWithPerfOld", decode(true, false));
        s.add_cmd(6, "DecodeInterleavedWithPerfAndResetOld", decode(true, true));
        s.add_cmd(7, "DecodeInterleavedForMultiStreamWithPerfAndResetOld", decode(true, true));
        s.add_cmd(8, "DecodeInterleaved", decode(true, true));
        s.add_cmd(9, "DecodeInterleavedForMultiStream", decode(true, true));
    });
}

std::shared_ptr<Service> make_hwopus() {
    return make_iface("IHardwareOpusDecoderManager", [](Service& s) {
        // Open*(params..., work buffer size, transfer memory). Raw arguments start with {s32 sample rate, s32 channels}: the old forms'
        // own fields and the Ex forms' OpusParametersEx alike (Ryujinx reads it from the request data; the game's cmd 4 has no buffer).
        // A buffer is looked at only when the raw data has no rate.
        auto open = [](bool ex) -> Handler {
            return [ex](Request& req, Response& rsp) -> uint32_t {
                uint32_t rate = req.arg<uint32_t>(0), ch = req.arg<uint32_t>(4);
                if (ex && rate == 0) {
                    const ipc::Buffer b = req.in_buf(0);
                    if (b.addr && b.size >= 8) {
                        std::memcpy(&rate, reinterpret_cast<const void*>(static_cast<uintptr_t>(b.addr)), 4);
                        std::memcpy(&ch, reinterpret_cast<const void*>(static_cast<uintptr_t>(b.addr + 4)), 4);
                    }
                }
                static std::atomic<int> logs{0};
                if (logs.fetch_add(1) < 4)
                    NXLOG_I("hwopus", "Opus decoder opened (cmd %u, %u Hz, %u channels)%s", req.cmd, rate, ch,
                            NXRT_HAVE_OPUS ? " on libopus" : ": no Opus decoder here, it decodes silence");
                rsp.push_object(make_hwopus_decoder(rate, ch));
                return 0;
            };
        };
        s.add_cmd(0, "OpenHardwareOpusDecoder", open(false));
        s.add_cmd(2, "OpenHardwareOpusDecoderForMultiStream", refuse(kNotSupported, "hwopus", "OpenHardwareOpusDecoderForMultiStream"));
        s.add_cmd(4, "OpenHardwareOpusDecoderEx", open(true));
        s.add_cmd(6, "OpenHardwareOpusDecoderForMultiStreamEx", refuse(kNotSupported, "hwopus", "OpenHardwareOpusDecoderForMultiStreamEx"));
        const Handler size = [](Request& req, Response& rsp) -> uint32_t {
            NXLOG_I("hwopus", "game asked for the Opus work buffer size (cmd %u); reporting %u", req.cmd, kOpusWorkBufferSize);
            rsp.push<uint32_t>(kOpusWorkBufferSize);
            return 0;
        };
        s.add_cmd(1, "GetWorkBufferSize", size);
        s.add_cmd(3, "GetWorkBufferSizeForMultiStream", size);
        s.add_cmd(5, "GetWorkBufferSizeEx", size);
        s.add_cmd(7, "GetWorkBufferSizeForMultiStreamEx", size);
        s.add_cmd(8, "GetWorkBufferSizeExEx", size);
        s.add_cmd(9, "GetWorkBufferSizeForMultiStreamExEx", size);
    });
}

// ---- aoc:u ------------------------------------------------------------------------------------------------------------------------
// No add-on content is installed. The base id of the title's add-on content is its program id + 0x1000.
std::shared_ptr<Service> make_purchase_manager() {
    return make_iface("IPurchaseEventManager", [](Service& s) {
        s.add_cmd(2, "GetPurchasedEventReadableHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().aoc_purchased.new_handle());
            return 0;
        });
        s.add_cmd(3, "PopPurchasedProductInfo", refuse(kAocNoPurchase, "aoc", "PopPurchasedProductInfo"));
        s.add_cmd(4, "PopPurchasedProductInfoWithUid", refuse(kAocNoPurchase, "aoc", "PopPurchasedProductInfoWithUid"));
    });
}
std::shared_ptr<Service> make_contents_manager() {
    return make_iface("IContentsServiceManager", [](Service& s) {
        // The authorization token comes from the network, which this console does not have.
        s.add_cmd(0, "RequestContentsAuthorizationToken", refuse(kAocNotConnected, "aoc", "RequestContentsAuthorizationToken"));
    });
}
std::shared_ptr<Service> make_aoc() {
    return make_iface("IAddOnContentManager", [](Service& s) {
        // Count and list answer zero entries. The list commands also zero their index buffer.
        s.add_cmd(0, "CountAddOnContentByApplicationId", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(0);
            return 0;
        });
        s.add_cmd(1, "ListAddOnContentByApplicationId", [](Request& req, Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        });
        s.add_cmd(2, "CountAddOnContent", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(0);
            return 0;
        });
        s.add_cmd(3, "ListAddOnContent", [](Request& req, Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        });
        s.add_cmd(4, "GetAddOnContentBaseIdByApplicationId", [](Request& req, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(req.arg<uint64_t>(0) + kAocBaseIdOffset);
            return 0;
        });
        s.add_cmd(5, "GetAddOnContentBaseId", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(profile().program_id + kAocBaseIdOffset);
            NXLOG_I("aoc", "GetAddOnContentBaseId: 0x%016llX", static_cast<unsigned long long>(profile().program_id + kAocBaseIdOffset));
            return 0;
        });
        s.add_cmd(6, "PrepareAddOnContentByApplicationId", accept("aoc", "PrepareAddOnContentByApplicationId"));
        s.add_cmd(7, "PrepareAddOnContent", accept("aoc", "PrepareAddOnContent"));
        const Handler list_event = [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().aoc_list_changed.new_handle());
            return 0;
        };
        s.add_cmd(8, "GetAddOnContentListChangedEvent", list_event);
        s.add_cmd(9, "GetAddOnContentLostErrorCode", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(0);   // no add-on content was lost
            return 0;
        });
        s.add_cmd(10, "GetAddOnContentListChangedEventWithProcessId", list_event);
        s.add_cmd(11, "NotifyMountAddOnContent", accept("aoc", "NotifyMountAddOnContent"));
        s.add_cmd(12, "NotifyUnmountAddOnContent", accept("aoc", "NotifyUnmountAddOnContent"));
        s.add_cmd(13, "IsAddOnContentMountedForDebug", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(50, "CheckAddOnContentMountStatus", accept("aoc", "CheckAddOnContentMountStatus"));
        const Handler purchase = [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_purchase_manager());
            return 0;
        };
        s.add_cmd(100, "CreateEcPurchasedEventManager", purchase);
        s.add_cmd(101, "CreatePermanentEcPurchasedEventManager", purchase);
        s.add_cmd(110, "CreateContentsServiceManager", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_contents_manager());
            return 0;
        });
        s.add_cmd(200, "SetRequiredAddOnContentsOnContentsAvailabilityTransition",
                  accept("aoc", "SetRequiredAddOnContentsOnContentsAvailabilityTransition"));
    });
}

// ---- caps:u (caps:su is in album.cpp) --------------------------------------------------------------------------------------------
// Album reading: lists are empty (title A main imports no album list or load function, only nn::album::SaveScreenshot).
Handler album_list() {
    return [](Request& req, Response& rsp) -> uint32_t {
        zero_buffer(req.out_buf(0));
        rsp.push<uint64_t>(0);
        return 0;
    };
}
std::shared_ptr<Service> make_album_app() {
    return make_iface("IAlbumApplicationService", [](Service& s) {
        s.add_cmd(32, "SetShimLibraryVersion", accept("caps", "SetShimLibraryVersion"));
        s.add_cmd(102, "GetAlbumFileList0AafeAruidDeprecated", album_list());
        s.add_cmd(103, "DeleteAlbumFileByAruid", accept("caps", "DeleteAlbumFileByAruid"));
        s.add_cmd(104, "GetAlbumFileSizeByAruid", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(0);
            return 0;
        });
        s.add_cmd(105, "DeleteAlbumFileByAruidForDebug", accept("caps", "DeleteAlbumFileByAruidForDebug"));
        s.add_cmd(110, "LoadAlbumScreenShotImageByAruid", accept("caps", "LoadAlbumScreenShotImageByAruid"));
        s.add_cmd(120, "LoadAlbumScreenShotThumbnailImageByAruid", accept("caps", "LoadAlbumScreenShotThumbnailImageByAruid"));
        s.add_cmd(130, "PrecheckToCreateContentsByAruid", accept("caps", "PrecheckToCreateContentsByAruid"));
        s.add_cmd(140, "GetAlbumFileList1AafeAruidDeprecated", album_list());
        s.add_cmd(141, "GetAlbumFileList2AafeUidAruidDeprecated", album_list());
        s.add_cmd(142, "GetAlbumFileList3AaeAruidDeprecated", album_list());
    });
}
// ---- mii:u ------------------------------------------------------------------------------------------------------------------------
// The shared Mii database is empty. Count and get answer zero; the default and random builders return an all-zero CharInfo (the
// real default Mii data is not reproduced here).
std::shared_ptr<Service> make_mii_database() {
    return make_iface("IDatabaseService", [](Service& s) {
        s.add_cmd(0, "IsUpdated", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(1, "IsFullDatabase", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        const Handler count = [](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(0);
            return 0;
        };
        s.add_cmd(2, "GetCount", count);
        const Handler get = [](Request& req, Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        };
        s.add_cmd(3, "Get", get);
        s.add_cmd(4, "Get1", get);
        s.add_cmd(8, "Get2", get);
        s.add_cmd(9, "Get3", get);
        // UpdateLatest / UpdateLatest1: nothing is stored, so the Mii that was given comes back unchanged.
        const Handler latest = [](Request& req, Response& rsp) -> uint32_t {
            if (req.in_size >= kCharInfoSize) {
                rsp.out.insert(rsp.out.end(), req.in, req.in + kCharInfoSize);
            } else {
                rsp.out.resize(rsp.out.size() + kCharInfoSize, 0);
            }
            return 0;
        };
        s.add_cmd(5, "UpdateLatest", latest);
        s.add_cmd(10, "UpdateLatest1", latest);
        s.add_cmd(6, "BuildRandom", [](Request&, Response& rsp) -> uint32_t {
            rsp.out.resize(rsp.out.size() + kCharInfoSize, 0);
            return 0;
        });
        s.add_cmd(7, "BuildDefault", [](Request& req, Response& rsp) -> uint32_t {
            if (req.arg<uint32_t>(0) >= 6) return kMiiInvalidArgument;   // six defaults
            rsp.out.resize(rsp.out.size() + kCharInfoSize, 0);
            return 0;
        });
        s.add_cmd(11, "FindIndex", refuse(kMiiNotFound, "mii", "FindIndex"));
        s.add_cmd(21, "GetIndex", refuse(kMiiNotFound, "mii", "GetIndex"));
        s.add_cmd(12, "Move", accept("mii", "Move"));
        s.add_cmd(13, "AddOrReplace", accept("mii", "AddOrReplace"));
        s.add_cmd(14, "Delete", accept("mii", "Delete"));
        s.add_cmd(15, "DestroyFile", accept("mii", "DestroyFile"));
        s.add_cmd(16, "DeleteFile", accept("mii", "DeleteFile"));
        s.add_cmd(17, "Format", accept("mii", "Format"));
        s.add_cmd(18, "Import", accept("mii", "Import"));
        s.add_cmd(19, "Export", accept("mii", "Export"));
        s.add_cmd(20, "IsBrokenDatabaseWithClearFlag", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(22, "SetInterfaceVersion", accept("mii", "SetInterfaceVersion"));
        s.add_cmd(23, "Convert", accept("mii", "Convert"));
        s.add_cmd(24, "ConvertCoreDataToCharInfo", accept("mii", "ConvertCoreDataToCharInfo"));
        s.add_cmd(25, "ConvertCharInfoToCoreData", accept("mii", "ConvertCharInfoToCoreData"));
        s.add_cmd(26, "Append", accept("mii", "Append"));
    });
}
std::shared_ptr<Service> make_mii_static() {
    return make_iface("IStaticService", [](Service& s) {
        s.add_cmd(0, "GetDatabaseService", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_mii_database());
            return 0;
        });
    });
}

// ---- irs --------------------------------------------------------------------------------------------------------------------------
// No IR camera is connected: sessions are accepted, processors accept and do nothing, and the camera handle is 0.
std::shared_ptr<Service> make_irs() {
    return make_iface("IIrSensorServer", [](Service& s) {
        s.add_cmd(302, "ActivateIrsensor", accept("irs", "ActivateIrsensor"));
        s.add_cmd(303, "DeactivateIrsensor", accept("irs", "DeactivateIrsensor"));
        s.add_cmd(304, "GetIrsensorSharedMemoryHandle", [](Request&, Response& rsp) -> uint32_t {
            services::ServiceSharedMemory sm(kIrsSharedMemorySize, "irs");
            if (!sm.ok()) return kNotSupported;
            rsp.out_copy.push_back(sm.new_handle());
            return 0;
        });
        s.add_cmd(305, "StopImageProcessor", accept("irs", "StopImageProcessor"));
        s.add_cmd(306, "RunMomentProcessor", accept("irs", "RunMomentProcessor"));
        s.add_cmd(307, "RunClusteringProcessor", accept("irs", "RunClusteringProcessor"));
        s.add_cmd(308, "RunImageTransferProcessor", accept("irs", "RunImageTransferProcessor"));
        s.add_cmd(309, "GetImageTransferProcessorState", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
        s.add_cmd(310, "RunTeraPluginProcessor", accept("irs", "RunTeraPluginProcessor"));
        s.add_cmd(311, "GetNpadIrCameraHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);   // no camera: handle 0
            return 0;
        });
        s.add_cmd(314, "CheckFirmwareVersion", accept("irs", "CheckFirmwareVersion"));
        s.add_cmd(318, "StopImageProcessorAsync", accept("irs", "StopImageProcessorAsync"));
        s.add_cmd(319, "ActivateIrsensorWithFunctionLevel", accept("irs", "ActivateIrsensorWithFunctionLevel"));
    });
}

// ---- nfc:user, nfc:mf:u, nfp:user -------------------------------------------------------------------------------------------------
// NFC is switched off: the state is NonInitialized, nothing is enabled, and there are no devices. Commands that need a device answer
// the no-device result.
Handler nfc_no_device(const char* tag, const char* what) { return refuse(kNfcNoDevice, tag, what); }
Handler nfp_no_device(const char* tag, const char* what) { return refuse(kNfpNoDevice, tag, what); }

std::shared_ptr<Service> make_nfc_user() {
    return make_iface("nn::nfc::detail::IUser", [](Service& s) {
        s.add_cmd(0, "InitializeOld", accept("nfc", "InitializeOld"));
        s.add_cmd(1, "FinalizeOld", accept("nfc", "FinalizeOld"));
        s.add_cmd(2, "GetStateOld", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);   // NonInitialized
            return 0;
        });
        s.add_cmd(3, "IsNfcEnabledOld", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(400, "Initialize", accept("nfc", "Initialize"));
        s.add_cmd(401, "Finalize", accept("nfc", "Finalize"));
        s.add_cmd(402, "GetState", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
        s.add_cmd(403, "IsNfcEnabled", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(404, "ListDevices", [](Request& req, Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        });
        s.add_cmd(405, "GetDeviceState", nfc_no_device("nfc", "GetDeviceState"));
        s.add_cmd(406, "GetNpadId", nfc_no_device("nfc", "GetNpadId"));
        s.add_cmd(407, "GetAvailabilityChangeEventHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfc_available.new_handle());
            return 0;
        });
        s.add_cmd(408, "StartDetection", nfc_no_device("nfc", "StartDetection"));
        s.add_cmd(409, "StopDetection", nfc_no_device("nfc", "StopDetection"));
        s.add_cmd(410, "GetTagInfo", nfc_no_device("nfc", "GetTagInfo"));
        s.add_cmd(411, "GetActivateEventHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfc_activate.new_handle());
            return 0;
        });
        s.add_cmd(412, "GetDeactivateEventHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfc_deactivate.new_handle());
            return 0;
        });
        s.add_cmd(1000, "ReadMifare", nfc_no_device("nfc", "ReadMifare"));
        s.add_cmd(1001, "WriteMifare", nfc_no_device("nfc", "WriteMifare"));
        s.add_cmd(1300, "SendCommandByPassThrough", nfc_no_device("nfc", "SendCommandByPassThrough"));
        s.add_cmd(1301, "KeepPassThroughSession", nfc_no_device("nfc", "KeepPassThroughSession"));
        s.add_cmd(1302, "ReleasePassThroughSession", nfc_no_device("nfc", "ReleasePassThroughSession"));
    });
}

std::shared_ptr<Service> make_nfp_user() {
    return make_iface("nn::nfp::detail::IUser", [](Service& s) {
        // The object's State (Ryujinx IUser: NonInitialized 0, Initialized 1): GetState reports it, so a caller that checks it
        // before nn::nfp::Initialize does not initialise twice (the SDK aborts on a second Initialize).
        auto state = std::make_shared<std::atomic<uint32_t>>(0);
        s.add_cmd(0, "Initialize", [state](Request&, Response&) -> uint32_t {
            state->store(1);
            NXLOG_D("nfp", "Initialize: state Initialized");
            return 0;
        });
        s.add_cmd(1, "Finalize", [state](Request&, Response&) -> uint32_t {
            state->store(0);
            NXLOG_D("nfp", "Finalize: state NonInitialized");
            return 0;
        });
        s.add_cmd(2, "ListDevices", [](Request& req, Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        });
        s.add_cmd(3, "StartDetection", nfp_no_device("nfp", "StartDetection"));
        s.add_cmd(4, "StopDetection", nfp_no_device("nfp", "StopDetection"));
        s.add_cmd(5, "Mount", nfp_no_device("nfp", "Mount"));
        s.add_cmd(6, "Unmount", nfp_no_device("nfp", "Unmount"));
        s.add_cmd(7, "OpenApplicationArea", nfp_no_device("nfp", "OpenApplicationArea"));
        s.add_cmd(8, "GetApplicationArea", nfp_no_device("nfp", "GetApplicationArea"));
        s.add_cmd(9, "SetApplicationArea", nfp_no_device("nfp", "SetApplicationArea"));
        s.add_cmd(10, "Flush", nfp_no_device("nfp", "Flush"));
        s.add_cmd(11, "Restore", nfp_no_device("nfp", "Restore"));
        s.add_cmd(12, "CreateApplicationArea", nfp_no_device("nfp", "CreateApplicationArea"));
        s.add_cmd(13, "GetTagInfo", nfp_no_device("nfp", "GetTagInfo"));
        s.add_cmd(14, "GetRegisterInfo", nfp_no_device("nfp", "GetRegisterInfo"));
        s.add_cmd(15, "GetCommonInfo", nfp_no_device("nfp", "GetCommonInfo"));
        s.add_cmd(16, "GetModelInfo", nfp_no_device("nfp", "GetModelInfo"));
        s.add_cmd(17, "AttachActivateEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfp_activate.new_handle());
            return 0;
        });
        s.add_cmd(18, "AttachDeactivateEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfp_deactivate.new_handle());
            return 0;
        });
        s.add_cmd(19, "GetState", [state](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(state->load());
            return 0;
        });
        s.add_cmd(20, "GetDeviceState", nfp_no_device("nfp", "GetDeviceState"));
        s.add_cmd(21, "GetNpadId", nfp_no_device("nfp", "GetNpadId"));
        s.add_cmd(22, "GetApplicationAreaSize", nfp_no_device("nfp", "GetApplicationAreaSize"));
        s.add_cmd(23, "GetAvailabilityChangeEventHandle", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(events().nfp_available.new_handle());
            return 0;
        });
        s.add_cmd(24, "RecreateApplicationArea", nfp_no_device("nfp", "RecreateApplicationArea"));
        s.add_cmd(25, "StartDetectionWithFilter", nfp_no_device("nfp", "StartDetectionWithFilter"));
    });
}

// IUserManager of nfc:user, nfc:mf:u and nfp:user: CreateUserInterface returns the user object for that service. The nfc:mf:u
// IUser has no command table here (its ids are not in the sources read), so its commands answer kStubService.
std::shared_ptr<Service> make_user_manager(const char* name, std::shared_ptr<Service> (*make_user)()) {
    return make_iface(name, [make_user](Service& s) {
        s.add_cmd(0, "CreateUserInterface", [make_user](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_user());
            return 0;
        });
    });
}
std::shared_ptr<Service> make_mifare_user() { return make_iface("nn::nfc::mifare::detail::IUser", [](Service&) {}); }

// ---- olsc:u -----------------------------------------------------------------------------------------------------------------------
// IOlscServiceForApplication (10.0.0+), online save data backup, which is off: Ryubing Olsc/IOlscServiceForApplication.cs (MIT)
// for the commands, results and the setting values (1 enabled, 2 not; Ryubing marks the values as not determined).
std::shared_ptr<Service> make_olsc() {
    struct Olsc {
        std::mutex mu;
        bool initialized = false;
        std::map<std::pair<uint64_t, uint64_t>, bool> backup;
    };
    auto o = std::make_shared<Olsc>();
    return make_iface("olsc:u", [o](Service& s) {
        s.add_cmd(0, "Initialize", [o](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> g(o->mu);
            o->initialized = true;
            NXLOG_D("olsc", "Initialize: online save data backup is not available");
            return 0;
        });
        s.add_cmd(13, "GetSaveDataBackupSetting", [o](Request& req, Response& rsp) -> uint32_t {
            const auto uid = std::make_pair(req.arg<uint64_t>(0), req.arg<uint64_t>(8));
            std::lock_guard<std::mutex> g(o->mu);
            if (!o->initialized) return kOlscNotInitialized;
            if (uid.first == 0 && uid.second == 0) return kOlscNullArgument;
            const auto it = o->backup.find(uid);
            rsp.push<uint8_t>(it != o->backup.end() && it->second ? 1 : 2);
            return 0;
        });
        s.add_cmd(14, "SetSaveDataBackupSettingEnabled", [o](Request& req, Response&) -> uint32_t {
            const bool enabled = req.arg<uint64_t>(0) != 0;
            const auto uid = std::make_pair(req.arg<uint64_t>(8), req.arg<uint64_t>(16));
            std::lock_guard<std::mutex> g(o->mu);
            if (!o->initialized) return kOlscNotInitialized;
            if (uid.first == 0 && uid.second == 0) return kOlscNullArgument;
            o->backup[uid] = enabled;
            return 0;
        });
    });
}

// ---- ectx:aw ----------------------------------------------------------------------------------------------------------------------
// Error-context writer: the registrar takes the context and drops it.
std::shared_ptr<Service> make_ectx_registrar() {
    return make_iface("nn::err::context::IContextRegistrar", [](Service& s) {
        s.add_cmd(0, "Complete", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(0);   // context_descriptor
            return 0;
        });
    });
}
std::shared_ptr<Service> make_ectx() {
    return make_iface("nn::err::context::IWriterForApplication", [](Service& s) {
        s.add_cmd(0, "CreateContextRegistrar", [](Request&, Response& rsp) -> uint32_t {
            rsp.push_object(make_ectx_registrar());
            return 0;
        });
    });
}

// ---- pcm and banana ---------------------------------------------------------------------------------------------------------------
// pcm: the interface has no command table in the sources read (Ryubing's IManager is empty; Switchbrew lists pcm as not present on
// retail), so every command reaches the framework's default and answers kStubService.
// banana: the profiler service, not on retail. Every command is logged at Warn and answered with kStubService.
class BananaService : public ipc::StubService {
public:
    BananaService() : ipc::StubService("banana") {}
    uint32_t handle(Request& req, Response& rsp) override {
        NXLOG_W("banana", "command %u (msg type %u) asked of banana; answering StubService", req.cmd, req.msg_type);
        return ipc::StubService::handle(req, rsp);
    }
};

// ---- htc, htc:tenv, htcs ----------------------------------------------------------------------------------------------------------
// TMA services: absent on retail (Switchbrew). Every command answers NotSupported and is logged.
using CmdNames = std::vector<std::pair<uint32_t, const char*>>;
std::shared_ptr<Service> make_tma(const char* name, const CmdNames& cmds) {
    return make_iface(name, [cmds](Service& s) {
        for (const auto& c : cmds) s.add_cmd(c.first, c.second, refuse(kNotSupported, "tma", c.second));
    });
}
CmdNames htc_cmds() {
    return {{0, "GetEnvironmentVariable"},        {1, "GetEnvironmentVariableLength"}, {2, "BindHostConnectionEvent"},
            {3, "BindHostDisconnectionEvent"},    {4, "BindHostConnectionEventForSystem"}, {5, "BindHostDisconnectionEventForSystem"},
            {6, "GetBridgeIpAddress"},            {7, "GetBridgePort"},                {8, "SetCradleAttached"},
            {9, "GetBridgeSubnetMask"},           {10, "GetBridgeMacAddress"},         {11, "GetWorkingDirectoryPath"},
            {12, "GetWorkingDirectoryPathSize"},  {13, "RunOnHostStart"},              {14, "RunOnHostResults"},
            {15, "SetBridgeIpAddress"},           {16, "SetBridgeSubnetMask"},         {17, "SetBridgePort"},
            {18, "GetBridgeSerialNumber"},        {19, "GetBridgeFwVersion"},          {20, "ResetBridgeSettings"},
            {21, "BeginUpdateBridge"},            {22, "ContinueUpdateBridge"},        {23, "EndUpdateBridge"},
            {24, "GetBridgeType"},                {25, "GetBridgeDefaultGateway"},     {26, "SetBridgeDefaultGateway"},
            {27, "GetBridgeDhcp"},                {28, "SetBridgeDhcp"},               {29, "GetBridgeProgramVersion"}};
}
CmdNames htcs_cmds() {
    return {{0, "Socket"},  {1, "Close"},   {2, "Connect"},   {3, "Bind"},     {4, "Listen"},   {5, "Accept"},   {6, "Recv"},
            {7, "Send"},    {8, "Shutdown"}, {9, "Fcntl"},     {10, "GetPeerNameAny"}, {11, "GetDefaultHostName"}, {12, "CreateSocketOld"},
            {13, "CreateSocket"}, {100, "RegisterProcessId"}, {101, "MonitorManager"}, {130, "Cmd130"}, {131, "Cmd131"}};
}
CmdNames htc_tenv_cmds() { return {{0, "GetServiceInterface"}}; }

}  // namespace

// ---- registration ------------------------------------------------------------------------------------------------------------------
// stpl:u (platform 2 stream play / GameShare): nn::streamplay::detail::GetUserService opens it and calls cmd 0 for the user service object.
// GameShare is not available here, but the two IUserService commands the SDK sends at start-up abort the process on any failure
// (sdk 0x75006b9744 and 0x75006ad634: AbortImpl), so they answer "nothing to share" instead of failing:
//   300 GetHidSharedMemory (sdk proxy 0x75006b78f4, caller GetUserService 0x75006b95f0): out {u64 size, u64 offset} + a copy handle.
//       HidApplicationResourceHolder (0x75006b08b8) attaches `size` bytes, maps them and reads the guest-input format at `offset`.
//   1   GetRequiredMemorySize(u8 type, u8 mode, u32 guests) -> u32 (sdk proxy 0x75006b76f0): the work memory sizes below (it was 0, but
//       nn::streamplay::Initialize divides the buffer size by the per-guest size, CalculateGuestCountFromMemorySize
//       (sdk 0x75006b9850), and aborts when the quotient is not at least 1: 0 made hosting a GameShare session abort).
// The format, from HidSharedMemoryAccessor's readers (sdk 0x75006b0980..0x75006b24a0), for up to three guests (IsGuestIdValid: id < 3):
// pads at 0, 0x24000 per guest pad id (0, 1, 32 = handheld) and 0xC000 per guest inside it (u32 NpadStyleSet at +0, pad ring at +0x28
// of 33 x 0x38 entries, six-axis rings from +0x2490 every 0xE80, controller colours at +0x7B90); touch screens at 0x6C000, 0x6000 per
// guest; Jcms rings at 0x7E000, 0x1800 per (pad 0/1, controller type 0/1) and 0x800 per guest. The readers reach 0x84000. "No GameShare
// session" is all zero: every style set 0 (GetGuestPadStyleSet: no pad) and every ring count 0 (the state readers return 0 entries).
//
// Hosting a GameShare session (title A: main 0x7101132f60 state 1 nn::streamplay::Initialize, state 2 OpenSessionWithMatchmakeSupport).
// A console with no guests around shows the matchmaking applet and the player backs out of it; am.cpp ends that applet (0x1000024 /
// 0x1030024) as cancelled, the SDK returns 0x3D1F and the game (main 0x7101133704) goes back quietly, without the error applet, and
// tears the session down (nn::streamplay::Finalize). Before that applet every IUserService command on the way must succeed: a failure
// in cmd 502 / 503 / 505 / 506 aborts the SDK, and a failure in Initialize makes the game show it through the error applet. Command
// ids are the IUserService proxy's (vtable 0x7500dd8d20, thunks 0x75006b7708..0x75006b79a8; slot +32 is cmd 1, +40 cmd 2 ...), the
// layouts come from the CMIF writers they branch to:
//   2   Initialize(u8 StreamPlayType, u8 StreamingMode, u32 guest count, pid) (0x75006b7ae0): kept for 502 / 503. No output.
//   3   Finalize, 5 CloseSession: accepted.
//   7   GetConnectionStateChangedEvent -> copy handle (Initialize attaches it): an event that never fires (no guest ever connects).
//   8   GetConnectionState -> 8 bytes (u8 flags, bit 0 connected; u32 result at 4): all zero, not connected.
//   25  SetSupportedGuestNpadStyleSet(u32), 27 SetGuestNpadJoyHoldType(u32): stored nowhere (no guest pads), accepted.
//   502 -> u8, 503 -> u8 (0x75001cb5e8): OpenSessionWithMatchmakeSupport passes them on as the StreamPlayType and StreamingMode
//       (CreateWorkBufferInProcessGlobal(type, mode), the same pair GetRequiredMemorySize takes; 502 also picks the applet id), so
//       they answer what Initialize was given. Type 1 skips the in-process work buffers (505 / 506).
//   504 (32-byte MatchmakeConfig) -> 0x80 bytes (0x75006b8990): the applet's argument, opaque to the SDK: zeros.
//   505 (copy handle: the work buffer's transfer memory, u64 size), 506 (u32 guest, 3 copy handles, 3 u64 sizes): accepted; the
//       handles stay the client's (it closes them).
//   507, 509 (no data), 510 (16 bytes: the audio output volumes Initialize reads): accepted.
//   9   GetWorkBufferSizes(u8 type, u8 mode) -> 4 x u64 {global, three per-guest parts} (0x75006b7d0c): type 0 (in-process) carves
//       its transfer memories out of the game's buffer with these sizes (CreateWorkBufferInProcessGlobal / PerGuest). Real sizes are
//       not known; one page each keeps everything page aligned and the game's buffer small (nothing is ever streamed). Command 1
//       answers to match: type 0 = global + guests x 3 pages, type 1 = guests x 1 page (CalculateRequiredWorkBufferSizePerInterface
//       asks cmd 1 for one guest of type 1).
// Every other command (StartStreaming's 4 / 500 / 501, the guest queries) is not implemented: it fails and is logged, so a GameShare
// path that gets past the matchmaking applet shows up in the log instead of running on zeros.
constexpr uint64_t kStreamplayHidSharedMemorySize = 0x84000;   // the extent of the format above (page aligned)

constexpr uint64_t kStreamplayPage = 0x1000;   // every work buffer part (see cmd 9 above)

uint32_t streamplay_required_memory(uint8_t type, uint32_t guests) {
    if (guests > 3) guests = 3;   // the SDK never asks for more (CalculateGuestCountFromMemorySize caps at 3)
    return static_cast<uint32_t>(type == 1 ? guests * kStreamplayPage : kStreamplayPage + guests * 3 * kStreamplayPage);
}

// What Initialize was given (one GameShare session per process).
struct StreamplayState {
    std::mutex mu;
    uint8_t type = 0;
    uint8_t mode = 0;
    ServiceEvent connection_changed{false};
};
StreamplayState& streamplay_state() {
    static StreamplayState s;
    return s;
}

Handler streamplay_ok(const char* name) {
    return [name](Request&, Response&) -> uint32_t {
        NXLOG_I("streamplay", "%s: accepted (no GameShare session here)", name);
        return 0;
    };
}

std::shared_ptr<ipc::Service> make_streamplay_user_service() {
    return make_iface("nn::streamplay::detail::IUserService", [](Service& s) {
        s.add_cmd(1, "GetRequiredMemorySize", [](Request& req, Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(streamplay_required_memory(req.arg<uint8_t>(0), req.arg<uint32_t>(4)));
            return 0;
        });
        s.add_cmd(9, "GetWorkBufferSizes", [](Request&, Response& rsp) -> uint32_t {
            for (int i = 0; i < 4; ++i) rsp.push<uint64_t>(kStreamplayPage);
            return 0;
        });
        s.add_cmd(300, "GetHidSharedMemory", [](Request&, Response& rsp) -> uint32_t {
            services::ServiceSharedMemory sm(kStreamplayHidSharedMemorySize, "streamplay.hid");
            if (!sm.ok()) return kNotSupported;
            const uint32_t h = sm.new_handle();
            if (!h) return kNotSupported;
            rsp.push<uint64_t>(kStreamplayHidSharedMemorySize);
            rsp.push<uint64_t>(0);   // the format starts at the beginning of the block
            rsp.out_copy.push_back(h);
            return 0;
        });
        s.add_cmd(2, "Initialize", [](Request& req, Response&) -> uint32_t {
            StreamplayState& st = streamplay_state();
            std::lock_guard<std::mutex> lk(st.mu);
            st.type = req.arg<uint8_t>(0);
            st.mode = req.arg<uint8_t>(1);
            NXLOG_I("streamplay", "Initialize(type %u, mode %u, %u guests): accepted, no guest will connect", st.type, st.mode,
                    req.arg<uint32_t>(4));
            return 0;
        });
        s.add_cmd(3, "Finalize", streamplay_ok("Finalize"));
        s.add_cmd(5, "CloseSession", streamplay_ok("CloseSession"));
        s.add_cmd(7, "GetConnectionStateChangedEvent", [](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(streamplay_state().connection_changed.new_handle());
            return 0;
        });
        s.add_cmd(8, "GetConnectionState", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(0);   // not connected, result 0
            return 0;
        });
        s.add_cmd(25, "SetSupportedGuestNpadStyleSet", streamplay_ok("SetSupportedGuestNpadStyleSet"));
        s.add_cmd(27, "SetGuestNpadJoyHoldType", streamplay_ok("SetGuestNpadJoyHoldType"));
        s.add_cmd(502, "GetStreamPlayType", [](Request&, Response& rsp) -> uint32_t {
            StreamplayState& st = streamplay_state();
            std::lock_guard<std::mutex> lk(st.mu);
            rsp.push<uint8_t>(st.type);
            return 0;
        });
        s.add_cmd(503, "GetStreamingMode", [](Request&, Response& rsp) -> uint32_t {
            StreamplayState& st = streamplay_state();
            std::lock_guard<std::mutex> lk(st.mu);
            rsp.push<uint8_t>(st.mode);
            return 0;
        });
        s.add_cmd(504, "PrepareMatchmake", [](Request&, Response& rsp) -> uint32_t {
            NXLOG_I("streamplay", "matchmaking requested: the matchmaking applet comes next (it ends as cancelled)");
            rsp.out.insert(rsp.out.end(), 0x80, uint8_t{0});
            return 0;
        });
        s.add_cmd(505, "SetWorkBuffer", streamplay_ok("SetWorkBuffer (cmd 505)"));
        s.add_cmd(506, "SetGuestWorkBuffer", streamplay_ok("SetGuestWorkBuffer (cmd 506)"));
        s.add_cmd(507, "Cmd507", streamplay_ok("cmd 507"));
        s.add_cmd(509, "Cmd509", streamplay_ok("cmd 509"));
        s.add_cmd(510, "SetAudioOutputVolumes", streamplay_ok("SetAudioOutputVolumes (cmd 510)"));
    });
}

std::shared_ptr<ipc::Service> make_streamplay_user() {
    auto s = std::make_shared<ipc::Service>("stpl:u");
    s->add_cmd(0, "CreateUserService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_streamplay_user_service());
        return 0;
    });
    return s;
}

// ---- chat:u (platform 2 GameChat) -----------------------------------------------------------------------------------------------------
// nn::chat::Initialize (the SDK of newer platform 2 titles runs it at boot, e.g. title C 1.8.0) opens "chat:u" and waits in sm until
// the service exists; Eglantine NX2_NOTES rows 123-124. Cmd 0 opens a session the SDK keeps using, so it has to be an object; the session's
// cmd 1 (u32 2 in) returns a copy handle the SDK waits on: an event that never fires (nobody is chatting). Every other command is not
// implemented and is logged by the stub. The rest of GameChat's start-up (nn::chat::gsv::Initialize: GraphicsSupervisor 1201 / 1202,
// then camera 601 / 602) is not answered yet: docs/platform2_services_xcheck.md rows 124-127.
std::shared_ptr<ipc::Service> make_chat_session() {
    auto ev = std::make_shared<ServiceEvent>(false);
    return make_iface("chat:u session", [ev](Service& s) {
        s.add_cmd(1, "GetEvent (platform 2, name ours)", [ev](Request& req, Response& rsp) -> uint32_t {
            NXLOG_I("chat", "chat:u session cmd 1 (arg %u): an event that never fires", req.arg<uint32_t>(0));
            const uint32_t h = ev->new_handle();
            if (!h) return kNotSupported;
            rsp.out_copy.push_back(h);
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_chat_user() {
    auto s = std::make_shared<ipc::Service>("chat:u");
    s->add_cmd(0, "OpenSession (platform 2, name ours)", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_chat_session());
        return 0;
    });
    return s;
}

void register_misc() {
    add("stpl:u", [] { return make_streamplay_user(); });
    add("chat:u", [] { return make_chat_user(); });
    add("csrng", [] { return make_csrng(); });
    add("ldr:ro", [] { return make_ro(); });
    add("mm:u", [] { return make_mm(); });
    add("hwopus", [] { return make_hwopus(); });
    add("aoc:u", [] { return make_aoc(); });
    add("caps:u", [] { return make_album_app(); });
    add("caps:su", [] { return make_screenshot_app(); });
    add("mii:u", [] { return make_mii_static(); });
    add("irs", [] { return make_irs(); });
    add("nfc:user", [] { return make_user_manager("nn::nfc::detail::IUserManager", &make_nfc_user); });
    add("nfc:mf:u", [] { return make_user_manager("nn::nfc::mifare::detail::IUserManager", &make_mifare_user); });
    add("nfp:user", [] { return make_user_manager("nn::nfp::detail::IUserManager", &make_nfp_user); });
    add("ectx:aw", [] { return make_ectx(); });
    add("pcm", [] { return make_iface("pcm", [](Service&) {}); });
    add("banana", [] { return std::make_shared<BananaService>(); });
    add("htc", [] { return make_tma("nn::tma::IHtcManager", htc_cmds()); });
    add("htc:tenv", [] { return make_tma("nn::htc::tenv::IServiceManager", htc_tenv_cmds()); });
    add("htcs", [] { return make_tma("nn::tma::IHtcsManager", htcs_cmds()); });
    add("olsc:u", [] { return make_olsc(); });
}

// Process-wide state back to its start-up value: the clock requests, and the events (cleared).
void misc_reset() {
    {
        MmState& s = mm_state();
        std::lock_guard<std::mutex> g(s.mu);
        s.next_id = 0;
        for (auto& sl : s.slots) sl = MmSlot{};
    }
    MiscEvents& e = events();
    e.aoc_list_changed.clear();
    e.aoc_purchased.clear();
    e.nfc_available.clear();
    e.nfc_activate.clear();
    e.nfc_deactivate.clear();
    e.nfp_available.clear();
    e.nfp_activate.clear();
    e.nfp_deactivate.clear();
}

}  // namespace nxrt::services
