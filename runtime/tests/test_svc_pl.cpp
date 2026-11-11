// SPDX-License-Identifier: GPL-3.0-or-later
// nn::pl:u and pl:s (S3c): the shared fonts. Request bytes and expected reply bytes are written out here from Switchbrew "Shared Database
// services" (command ids, u8 + u32 reply, the three u32 arrays) and from the shared memory layout of Ryujinx SharedFontManager (header magic
// 0x18029A7F, size XOR 0x49621806 big-endian, font data 8 bytes after the header, the next header right after it). Nothing is read back
// from pl.cpp for the expected values.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "svc_test_util.h"

NXTEST_GROUP(svc_pl)

namespace nxrt::services {
void pl_reset();   // src/services/pl.cpp
}

namespace {

constexpr uint32_t kTypes = 6;
const char* kFonts = NXRT_TEST_DATA_DIR "/../../assets/fonts";   // the repository's DejaVu Sans (assets/fonts/DejaVuSans.ttf)

bool read_all(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    out.resize(size_t(n));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return f.good() || f.gcount() == n;
}

// Points the service at a folder and makes the next call build the shared memory from it.
void use_fonts(const char* dir) {
    nxrt::config_clear();
    nxrt::config_set("pl", "font_dir", dir);
    nxrt::services::pl_reset();
}

// Shared memory offset of font type t when every slot holds `size` bytes: header 8 bytes, data, next header.
// Ryujinx: offset(0) = 0 + 8, offset(t+1) = offset(t) + size + 8.
uint32_t expected_offset(uint32_t t, uint64_t size) { return uint32_t(8 + t * (8 + size)); }

struct TempDir {
    std::filesystem::path p;
    explicit TempDir(const char* name) : p(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
    }
};

void put_le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

}  // namespace

NXTEST(pl_request_load_and_state) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("pl:u");
    REQUIRE(s.valid());
    // RequestLoad(u32 font type 4 = Korean): no output.
    nxsvc::Reply r = s.call(0, nxsvc::Args().u32(4));
    CHECK_EQ(r.result, 0u);
    // GetLoadState(u32 4): output u32 1 (Loaded). Bytes 01 00 00 00.
    r = s.call(1, nxsvc::Args().u32(4));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint32_t>(0), 1u);
    // pl:s answers the same way.
    nxsvc::Session sys = nxsvc::connect("pl:s");
    REQUIRE(sys.valid());
    r = sys.call(1, nxsvc::Args().u32(0));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint32_t>(0), 1u);
}

NXTEST(pl_size_and_offset_of_each_font) {
    std::vector<uint8_t> ttf;
    if (!read_all(std::filesystem::path(kFonts) / "DejaVuSans.ttf", ttf)) {
        nxtest::skip("assets/fonts/DejaVuSans.ttf not found");
        return;
    }
    nxsvc::Env env;
    use_fonts(kFonts);
    nxsvc::Session s = nxsvc::connect("pl:u");
    REQUIRE(s.valid());
    for (uint32_t t = 0; t < kTypes; ++t) {
        // GetSize(u32 t): the TTF length (every slot is the same file).
        nxsvc::Reply r = s.call(2, nxsvc::Args().u32(t));
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.get<uint32_t>(0), uint32_t(ttf.size()));
        // GetSharedMemoryAddressOffset(u32 t): 8 + t * (8 + size).
        r = s.call(3, nxsvc::Args().u32(t));
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.get<uint32_t>(0), expected_offset(t, ttf.size()));
    }
}

NXTEST(pl_shared_memory_handle_and_contents) {
    std::vector<uint8_t> ttf;
    if (!read_all(std::filesystem::path(kFonts) / "DejaVuSans.ttf", ttf)) {
        nxtest::skip("assets/fonts/DejaVuSans.ttf not found");
        return;
    }
    nxsvc::Env env;
    use_fonts(kFonts);
    nxsvc::Session s = nxsvc::connect("pl:u");
    REQUIRE(s.valid());
    // GetSharedMemoryNativeHandle(): copy handle, no input, no output data. Shared memory size 0x1100000.
    nxsvc::Reply r = s.call(4);
    CHECK_EQ(r.result, 0u);
    REQUIRE(r.copy.size() == 1);
    auto obj = nxrt::kernel::handle_as<nxrt::kernel::KSharedMemory>(r.copy[0]);
    REQUIRE(obj != nullptr);
    CHECK_EQ(obj->size, 0x1100000ull);
    const uint8_t* mem = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(obj->host));
    REQUIRE(mem != nullptr);
    // Slot 0 header at 0: magic 0x18029A7F, stored little-endian: 7F 9A 02 18.
    CHECK_EQ(mem[0], 0x7Fu);
    CHECK_EQ(mem[1], 0x9Au);
    CHECK_EQ(mem[2], 0x02u);
    CHECK_EQ(mem[3], 0x18u);
    // Bytes 4..7: the size XOR 0x49621806, big-endian. size ^ key, written out per byte.
    const uint32_t enc = uint32_t(ttf.size()) ^ 0x49621806u;
    CHECK_EQ(mem[4], uint8_t(enc >> 24));
    CHECK_EQ(mem[5], uint8_t(enc >> 16));
    CHECK_EQ(mem[6], uint8_t(enc >> 8));
    CHECK_EQ(mem[7], uint8_t(enc));
    // The font data at 8 is the TTF file, byte for byte (the service stores it plain, as Switchbrew describes).
    CHECK(std::equal(ttf.begin(), ttf.end(), mem + 8));
    // Slot 5 (the console's extended symbols) at the offset GetSharedMemoryAddressOffset reports: header just before it, same TTF.
    const uint64_t off5 = expected_offset(5, ttf.size());
    CHECK_EQ(mem[off5 - 8], 0x7Fu);
    CHECK(std::equal(ttf.begin(), ttf.end(), mem + off5));
    nxrt::kernel::handle_close(r.copy[0]);
}

NXTEST(pl_in_order_of_priority_en_us) {
    std::vector<uint8_t> ttf;
    if (!read_all(std::filesystem::path(kFonts) / "DejaVuSans.ttf", ttf)) {
        nxtest::skip("assets/fonts/DejaVuSans.ttf not found");
        return;
    }
    nxsvc::Env env;
    use_fonts(kFonts);
    nxsvc::Session s = nxsvc::connect("pl:u");
    REQUIRE(s.valid());
    // LanguageCode "en-US" as the u64 the client sends: bytes 65 6E 2D 55 53 00 00 00.
    uint32_t types[kTypes] = {}, offs[kTypes] = {}, sizes[kTypes] = {};
    nxsvc::Reply r = s.call(5, nxsvc::Args().u64(0x53552d6e65ull).buf_c(types, 24).buf_c(offs, 24).buf_c(sizes, 24));
    CHECK_EQ(r.result, 0u);
    // Reply: u8 loaded = 1, three bytes padding, u32 count = 6 at offset 4.
    CHECK_EQ(r.get<uint8_t>(0), 1u);
    CHECK_EQ(r.get<uint32_t>(4), 6u);
    for (uint32_t i = 0; i < kTypes; ++i) {
        CHECK_EQ(types[i], i);
        CHECK_EQ(offs[i], expected_offset(i, ttf.size()));
        CHECK_EQ(sizes[i], uint32_t(ttf.size()));
    }
}

NXTEST(pl_in_order_of_priority_ja_and_for_system) {
    std::vector<uint8_t> ttf;
    if (!read_all(std::filesystem::path(kFonts) / "DejaVuSans.ttf", ttf)) {
        nxtest::skip("assets/fonts/DejaVuSans.ttf not found");
        return;
    }
    nxsvc::Env env;
    use_fonts(kFonts);
    nxsvc::Session s = nxsvc::connect("pl:s");
    REQUIRE(s.valid());
    // "ja" as the u64 LanguageCode: bytes 6A 61 00 ... The order is the same as for en-US (type order).
    uint32_t types[kTypes] = {}, offs[kTypes] = {}, sizes[kTypes] = {};
    nxsvc::Reply r = s.call(5, nxsvc::Args().u64(0x616aull).buf_c(types, 24).buf_c(offs, 24).buf_c(sizes, 24));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint8_t>(0), 1u);
    CHECK_EQ(r.get<uint32_t>(4), 6u);
    for (uint32_t i = 0; i < kTypes; ++i) {
        CHECK_EQ(types[i], i);
        CHECK_EQ(offs[i], expected_offset(i, ttf.size()));
        CHECK_EQ(sizes[i], uint32_t(ttf.size()));
    }
    // GetSharedFontInOrderOfPriorityForSystem (6): the same reply.
    uint32_t types2[kTypes] = {}, offs2[kTypes] = {}, sizes2[kTypes] = {};
    r = s.call(6, nxsvc::Args().u64(0x616aull).buf_c(types2, 24).buf_c(offs2, 24).buf_c(sizes2, 24));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint8_t>(0), 1u);
    CHECK_EQ(r.get<uint32_t>(4), 6u);
    for (uint32_t i = 0; i < kTypes; ++i) {
        CHECK_EQ(types2[i], i);
        CHECK_EQ(offs2[i], expected_offset(i, ttf.size()));
        CHECK_EQ(sizes2[i], uint32_t(ttf.size()));
    }
}

NXTEST(pl_invalid_font_type_answers_zero) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("pl:u");
    REQUIRE(s.valid());
    // Type 6 is past SharedFontType (Count = 6). Switchbrew lists no result for it; ScarletNX answers 0 and so do we.
    nxsvc::Reply r = s.call(0, nxsvc::Args().u32(6));
    CHECK_EQ(r.result, 0u);
    r = s.call(2, nxsvc::Args().u32(6));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint32_t>(0), 0u);
    r = s.call(3, nxsvc::Args().u32(0xFFFFFFFFu));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint32_t>(0), 0u);
}

NXTEST(pl_bfttf_dump_replaces_its_slot) {
    nxsvc::Env env;
    // Payload: 0x40 bytes, a pattern. A BFTTF file holds it as little-endian words XOR 0x06186249 (Ryujinx DecryptFont), after a magic
    // word (0x18029A7F XOR the key) and a size word.
    std::vector<uint8_t> payload(0x40);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7 + 3);
    std::vector<uint8_t> file;
    put_le32(file, 0x18029A7Fu ^ 0x06186249u);
    put_le32(file, uint32_t(payload.size()) ^ 0x06186249u);
    for (size_t i = 0; i < payload.size(); i += 4) {
        const uint32_t w = uint32_t(payload[i]) | (uint32_t(payload[i + 1]) << 8) | (uint32_t(payload[i + 2]) << 16) |
                           (uint32_t(payload[i + 3]) << 24);
        put_le32(file, w ^ 0x06186249u);
    }
    TempDir dir("nxrt_pl_dump_test");
    {
        std::ofstream f(dir.p / "FontStandard.bfttf", std::ios::binary);
        f.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    }
    use_fonts(dir.p.string().c_str());
    nxsvc::Session s = nxsvc::connect("pl:u");
    REQUIRE(s.valid());
    // Slot 0 comes from the dump: size 0x40, data at 8.
    nxsvc::Reply r = s.call(2, nxsvc::Args().u32(0));
    CHECK_EQ(r.get<uint32_t>(0), 0x40u);
    r = s.call(3, nxsvc::Args().u32(0));
    CHECK_EQ(r.get<uint32_t>(0), 8u);
    // The shared memory holds the decoded payload there.
    r = s.call(4);
    REQUIRE(r.copy.size() == 1);
    auto obj = nxrt::kernel::handle_as<nxrt::kernel::KSharedMemory>(r.copy[0]);
    REQUIRE(obj != nullptr);
    const uint8_t* mem = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(obj->host));
    CHECK(std::equal(payload.begin(), payload.end(), mem + 8));
    nxrt::kernel::handle_close(r.copy[0]);
}

NXTEST(pl_no_font_found_still_answers) {
    nxsvc::Env env;
    // An empty folder, and no DejaVu next to the test binary: every slot is an 8-byte header with size 0.
    TempDir dir("nxrt_pl_empty_test");
    use_fonts(dir.p.string().c_str());
    nxsvc::Session s = nxsvc::connect("pl:u");
    REQUIRE(s.valid());
    for (uint32_t t = 0; t < kTypes; ++t) {
        nxsvc::Reply r = s.call(2, nxsvc::Args().u32(t));
        CHECK_EQ(r.get<uint32_t>(0), 0u);
        r = s.call(3, nxsvc::Args().u32(t));
        CHECK_EQ(r.get<uint32_t>(0), expected_offset(t, 0));
        r = s.call(1, nxsvc::Args().u32(t));
        CHECK_EQ(r.get<uint32_t>(0), 1u);
    }
}

NXTEST(pl_unknown_command_answers_stub) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("pl:u");
    REQUIRE(s.valid());
    // Command 99 is not in the table: the framework answers kStubService (0x0BCBD5).
    nxsvc::Reply r = s.call(99);
    CHECK_EQ(r.result, nxrt::kernel::kStubService);
}

// platform 2 pl:u 2000 / 2010 (Eglantine NX2_NOTES row 36): the built-in fonts as plain TTF. 2010 takes the font type in the first byte and
// answers 24 bytes: u32 found at 0, descriptor at 8 {u32 flags, u32 offset, u32 size, u32}. The offset must hold the same bytes
// GetSharedMemoryAddressOffset points at (the shared memory is plain TTF behind its 8-byte header).
NXTEST(pl_switch2_built_in_font_data) {
    std::vector<uint8_t> ttf;
    if (!read_all(std::filesystem::path(kFonts) / "DejaVuSans.ttf", ttf)) {
        nxtest::skip("assets/fonts/DejaVuSans.ttf not found");
        return;
    }
    nxsvc::Env env;
    use_fonts(kFonts);
    nxsvc::Session s = nxsvc::connect("pl:u");
    REQUIRE(s.valid());
    nxsvc::Reply mem = s.call(2000);   // GetBuiltInFontSharedMemory: u32 size + copy handle
    CHECK_EQ(mem.result, 0u);
    CHECK_EQ(mem.copy.size(), size_t(1));
    CHECK(mem.get<uint32_t>(0) >= expected_offset(kTypes - 1, ttf.size()) + ttf.size());
    for (uint32_t t = 0; t < kTypes; ++t) {
        nxsvc::Reply r = s.call(2010, nxsvc::Args().u32(t));
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.get<uint32_t>(0), 1u);
        CHECK_EQ(r.get<uint32_t>(12), expected_offset(t, ttf.size()));
        CHECK_EQ(r.get<uint32_t>(16), uint32_t(ttf.size()));
    }
    nxsvc::Reply none = s.call(2010, nxsvc::Args().u32(77));   // not a font type
    CHECK_EQ(none.result, 0u);
    CHECK_EQ(none.get<uint32_t>(0), 0u);
    CHECK_EQ(none.get<uint32_t>(16), 0u);
}

// chat:u (platform 2 GameChat): cmd 0 opens a session, whose cmd 1 returns an event handle.
NXTEST(chat_u_opens_a_session_with_an_event) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("chat:u");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(0);
    REQUIRE(r.ok());
    nxsvc::Session chat = s.sub(r);
    REQUIRE(chat.valid());
    nxsvc::Reply ev = chat.call(1, nxsvc::Args().u32(2));
    CHECK_EQ(ev.result, 0u);
    CHECK_EQ(ev.copy.size(), size_t(1));
}
