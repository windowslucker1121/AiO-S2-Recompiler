// SPDX-License-Identifier: GPL-3.0-or-later
// nn::pl:u and pl:s (S3c): the six shared system fonts in the 0x1100000-byte shared memory.
// Command ids and reply layouts: Switchbrew "Shared Database services" (the pl:s table; RequestLoad 0 .. GetSharedFontInOrderOfPriorityForSystem 6
// are the font commands of firmware 1.0.0 to 15.0.1, and pl:u has the same ones at that firmware).
// Adapted from ScarletNX src/nx/hle/services/pl.c (GPL-3.0-or-later): GetSharedFontInOrderOfPriority lists every font in type order
// whatever the language, and an unknown font type answers 0 from GetSize and GetSharedMemoryAddressOffset.
// Adapted from Ryujinx src/Ryujinx.HLE/HOS/Services/Sdb/Pl/SharedFontManager.cs and ISharedFontManager.cs (MIT): the slot order and offsets
// (8-byte header, then the font data; the next header follows directly), the magic 0x18029A7F little-endian, the size XOR 0x49621806 stored
// big-endian, the BFTTF file decryption (each little-endian word XOR 0x06186249 after a magic check) and the reply of a u8 flag and a u32 count.
//
// What is served: the font data in the shared memory is plain TrueType (Switchbrew: "Font data is TTF, located at the offset returned by
// GetSharedMemoryAddressOffset"; Ryujinx and ScarletNX also store it decrypted). The console's real fonts are not available: every slot takes
// the same freely licensed DejaVu Sans (assets/fonts/DejaVuSans.ttf, Bitstream Vera licence). DejaVu Sans has no CJK glyphs and lacks
// several symbols that the system fonts carry, so Chinese, Korean and the console's own symbols do not render with it.
//
// Lookup, per slot: a real dump file in [pl] font_dir (FontStandard.bfttf ... in BFTTF form, decrypted here) replaces its slot; otherwise the
// first existing DejaVuSans.ttf of: [pl] font_dir, fonts\ next to the exe, assets\fonts\ under the working directory, the exe's folder.
// When no TTF at all is found every slot still gets its 8-byte header with size 0 and a warning is logged once.
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/ipc.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

constexpr uint32_t kSharedSize = 0x1100000;   // map size the guest uses (Switchbrew: size=0x1100000, permissions R--)
constexpr uint32_t kFontCount = 6;
constexpr uint32_t kBfttfMagic = 0x18029A7F;
constexpr uint32_t kSizeKey = 0x49621806;      // XORed with the size in the header
constexpr uint32_t kFileKey = 0x06186249;      // XORed with every little-endian word of a BFTTF file (and its magic)

// SharedFontType (Switchbrew): 0 Standard (Japan, US, Europe), 1 Chinese Simplified, 2 Extended Chinese Simplified, 3 Chinese Traditional,
// 4 Korean, 5 the console's extended symbols. The dump file names are the ones of the console's font archive as they are named in it; ScarletNX and
// Ryujinx call slot 2 FontExtendedChineseSimplified.
constexpr const char* kDumpNames[kFontCount] = {
    "FontStandard.bfttf",          "FontChineseSimplified.bfttf", "FontChineseSimplifiedExtension.bfttf",
    "FontChineseTraditional.bfttf", "FontKorean.bfttf",           "FontNintendoExtended.bfttf",
};

struct Slot {
    uint32_t offset = 0;   // offset of the font data in the shared memory (the 8-byte header is just before it)
    uint32_t size = 0;     // font data size
};

struct State {
    services::ServiceSharedMemory shm{kSharedSize, "pl"};
    Slot slot[kFontCount];
    bool requested[kFontCount] = {};   // RequestLoad marks a font; loading is instant, so GetLoadState never waits
    std::mutex mu;
};

std::mutex& state_mu() {
    static std::mutex m;
    return m;
}
std::unique_ptr<State>& state_slot() {
    static std::unique_ptr<State> s;
    return s;
}

void put_le32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
uint32_t get_le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool read_file(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    out.clear();
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n <= 0) return false;
    out.resize(size_t(n));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return f.good() || f.gcount() == n;
}

// A BFTTF file: magic word (little-endian, XOR key), size word (skipped), then the TTF as little-endian words XOR key. Ryujinx's DecryptFont.
bool decode_bfttf(const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
    out.clear();
    if (raw.size() < 8 || (get_le32(raw.data()) ^ kFileKey) != kBfttfMagic) return false;
    const size_t words = (raw.size() - 8) / 4;
    out.resize(words * 4);
    for (size_t i = 0; i < words; ++i) put_le32(out.data() + 4 * i, get_le32(raw.data() + 8 + 4 * i) ^ kFileKey);
    return true;
}

std::filesystem::path exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
#else
    return {};
#endif
}

// The DejaVu Sans fallback: the first existing candidate wins.
std::vector<uint8_t> load_fallback(const std::filesystem::path& dir) {
    std::error_code ec;
    std::vector<std::filesystem::path> cands;
    if (!dir.empty()) cands.push_back(dir / "DejaVuSans.ttf");
    const std::filesystem::path ex = exe_dir();
    if (!ex.empty()) cands.push_back(ex / "fonts" / "DejaVuSans.ttf");
    cands.push_back(std::filesystem::current_path(ec) / "assets" / "fonts" / "DejaVuSans.ttf");
    if (!ex.empty()) cands.push_back(ex / "DejaVuSans.ttf");
    for (const auto& p : cands) {
        std::vector<uint8_t> v;
        if (read_file(p, v)) {
            NXLOG_I("pl", "fallback shared font: %s (%zu bytes)", p.string().c_str(), v.size());
            return v;
        }
    }
    return {};
}

// Fills the shared memory: header and font data for the six slots, back to back in SharedFontType order (Ryujinx SharedFontManager).
void build(State& st) {
    uint8_t* mem = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(st.shm.host()));
    if (!mem || st.shm.size() != kSharedSize) {
        NXLOG_E("pl", "shared font memory could not be created: pl:u answers with empty fonts");
        return;
    }
    std::memset(mem, 0, kSharedSize);
    static bool warned = false;   // process-wide: the warning is logged once
    const std::filesystem::path dir = config_get_str("pl", "font_dir", "");
    const std::vector<uint8_t> fallback = load_fallback(dir);
    uint64_t cursor = 0;
    uint32_t ttf_slots = 0;
    for (uint32_t i = 0; i < kFontCount; ++i) {
        std::vector<uint8_t> font;
        const char* from = "none";
        if (!dir.empty()) {
            std::vector<uint8_t> raw;
            if (read_file(dir / kDumpNames[i], raw)) {
                if (decode_bfttf(raw, font))
                    from = "dump";
                else
                    NXLOG_W("pl", "%s is not a BFTTF file: the DejaVu fallback takes its slot", (dir / kDumpNames[i]).string().c_str());
            }
        }
        if (font.empty() && !fallback.empty()) {
            font = fallback;
            from = "fallback";
        }
        if (cursor + 8 + font.size() > kSharedSize) {
            NXLOG_W("pl", "shared font %u (%zu bytes) does not fit in the shared memory: its slot is empty", i, font.size());
            font.clear();
            from = "none";
        }
        if (!font.empty()) ++ttf_slots;
        uint8_t* h = mem + cursor;
        put_le32(h, kBfttfMagic);
        put_be32(h + 4, uint32_t(font.size()) ^ kSizeKey);
        if (!font.empty()) std::memcpy(h + 8, font.data(), font.size());
        st.slot[i].offset = uint32_t(cursor + 8);
        st.slot[i].size = uint32_t(font.size());
        NXLOG_I("pl", "shared font %u: %s, %zu bytes at shared memory offset 0x%llx", i, from, font.size(),
                static_cast<unsigned long long>(cursor + 8));
        cursor += 8 + font.size();
    }
    if (ttf_slots == 0 && !warned) {
        warned = true;
        NXLOG_W("pl", "no TTF font found (no fonts/ or assets/fonts/DejaVuSans.ttf, no [pl] font_dir): every shared font is empty");
    }
}

State& state() {
    std::lock_guard<std::mutex> lk(state_mu());
    auto& p = state_slot();
    if (!p) {
        p = std::make_unique<State>();
        build(*p);
    }
    return *p;
}

// GetSharedFontInOrderOfPriority / ...ForSystem: u64 language code in (not used: every language gets the same order, as in ScarletNX and
// Ryujinx), three type-0x6 output buffers of u32 arrays (font type, offset, size), reply u8 loaded (1) and u32 count.
uint32_t in_priority_order(ipc::Request& req, ipc::Response& rsp) {
    const uint64_t lang = req.arg<uint64_t>(0);
    NXLOG_D("pl", "GetSharedFontInOrderOfPriority(language 0x%llx)", static_cast<unsigned long long>(lang));
    State& st = state();
    const ipc::Buffer types = req.out_buf(0), offsets = req.out_buf(1), sizes = req.out_buf(2);
    const uint64_t n = std::min<uint64_t>({kFontCount, types.size / 4, offsets.size / 4, sizes.size / 4});
    std::vector<uint32_t> t(n), o(n), z(n);
    for (uint64_t i = 0; i < n; ++i) {
        t[i] = uint32_t(i);
        o[i] = st.slot[i].offset;
        z[i] = st.slot[i].size;
    }
    services::buf_write(types, t.data(), n * 4);
    services::buf_write(offsets, o.data(), n * 4);
    services::buf_write(sizes, z.data(), n * 4);
    rsp.push<uint8_t>(1);   // loaded: the fonts are all there from the start
    rsp.push<uint8_t>(0);   // u8 padded to a word
    rsp.push<uint8_t>(0);
    rsp.push<uint8_t>(0);
    rsp.push<uint32_t>(uint32_t(n));
    return 0;
}

std::shared_ptr<ipc::Service> make_pl(const char* name) {
    return make_iface(name, [](ipc::Service& s) {
        // 0 RequestLoad(u32 SharedFontType). Loading is instant, so this only records the request.
        s.add_cmd(0, "RequestLoad", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint32_t type = req.arg<uint32_t>(0);
            NXLOG_D("pl", "RequestLoad(%u)", type);
            State& st = state();
            if (type >= kFontCount) {
                NXLOG_W("pl", "RequestLoad: font type %u is not a shared font type (accepted, nothing done)", type);
                return 0;
            }
            std::lock_guard<std::mutex> lk(st.mu);
            st.requested[type] = true;
            return 0;
        });
        // 1 GetLoadState(u32) -> u32 LoadState: 1 = Loaded, for every font.
        s.add_cmd(1, "GetLoadState", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            NXLOG_D("pl", "GetLoadState(%u)", req.arg<uint32_t>(0));
            rsp.push<uint32_t>(1);
            return 0;
        });
        // 2 GetSize(u32) -> u32 font data size.
        s.add_cmd(2, "GetSize", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t type = req.arg<uint32_t>(0);
            State& st = state();
            if (type >= kFontCount) {
                NXLOG_W("pl", "GetSize: font type %u is not a shared font type (answers 0)", type);
                rsp.push<uint32_t>(0);
                return 0;
            }
            rsp.push<uint32_t>(st.slot[type].size);
            return 0;
        });
        // 3 GetSharedMemoryAddressOffset(u32) -> u32 offset of the font data (TTF) in the shared memory.
        s.add_cmd(3, "GetSharedMemoryAddressOffset", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t type = req.arg<uint32_t>(0);
            State& st = state();
            if (type >= kFontCount) {
                NXLOG_W("pl", "GetSharedMemoryAddressOffset: font type %u is not a shared font type (answers 0)", type);
                rsp.push<uint32_t>(0);
                return 0;
            }
            rsp.push<uint32_t>(st.slot[type].offset);
            return 0;
        });
        // 4 GetSharedMemoryNativeHandle() -> copy handle of the shared memory (mapped R-- by the guest).
        s.add_cmd(4, "GetSharedMemoryNativeHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            State& st = state();
            rsp.out_copy.push_back(st.shm.new_handle());
            return 0;
        });
        // 5 GetSharedFontInOrderOfPriority(u64 language, 3 x buffer) -> (u8 loaded, u32 count).
        s.add_cmd(5, "GetSharedFontInOrderOfPriority", in_priority_order);
        // 6 GetSharedFontInOrderOfPriorityForSystem: the same answer ([4.0.0-15.0.1] in Switchbrew, ScarletNX and Ryujinx).
        s.add_cmd(6, "GetSharedFontInOrderOfPriorityForSystem", in_priority_order);
        // platform 2 SDKs (nn::pl::GetBuiltInFontData, title F; Eglantine NX2_NOTES row 36) read the fonts as plain TrueType through two
        // new commands instead of the BFTTF slots above. The shared memory already holds plain TTF, so both answer from it.
        // 2000 GetBuiltInFontSharedMemory() -> u32 size to map, copy handle of the shared memory (mapped read-only).
        s.add_cmd(2000, "GetBuiltInFontSharedMemory (platform 2)", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            State& st = state();
            uint32_t end = 0;
            for (uint32_t i = 0; i < kFontCount; ++i) end = std::max(end, st.slot[i].offset + st.slot[i].size);
            rsp.push<uint32_t>(std::min<uint32_t>(kSharedSize, std::max<uint32_t>(0x1000, (end + 0xFFF) & ~0xFFFu)));
            rsp.out_copy.push_back(st.shm.new_handle());
            return 0;
        });
        // 2010 GetBuiltInFontData(u8 SharedFontType, u8 flag (+2 padding bytes)) -> 24 bytes: u32 found at 0, then a descriptor at 8 of
        // {u8 flags, u8 flag bit, u16, u32 offset into the 2000 memory, u32 size, u32}. Types are the SharedFontType values; an empty slot
        // (no TTF found) or an unknown type answers "not found" with zeros.
        s.add_cmd(2010, "GetBuiltInFontData (platform 2)", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t args = req.arg<uint32_t>(0);
            const uint32_t type = args & 0xFF;
            State& st = state();
            const bool found = type < kFontCount && st.slot[type].size != 0;
            NXLOG_D("pl", "GetBuiltInFontData(type %u, flag %u) -> %s", type, (args >> 8) & 1, found ? "found" : "not found");
            rsp.push<uint32_t>(found ? 1 : 0);
            rsp.push<uint32_t>(0);
            rsp.push<uint32_t>(0);
            rsp.push<uint32_t>(found ? st.slot[type].offset : 0);
            rsp.push<uint32_t>(found ? st.slot[type].size : 0);
            rsp.push<uint32_t>(0);
            return 0;
        });
    });
}

}  // namespace

void register_pl() {
    add("pl:u", [] { return make_pl("pl:u"); });
    add("pl:s", [] { return make_pl("pl:s"); });
}

void pl_reset() {
    std::lock_guard<std::mutex> lk(state_mu());
    state_slot().reset();   // the next call builds the shared memory again, from the config as it is then
}

}  // namespace nxrt::services
