// SPDX-License-Identifier: GPL-3.0-or-later
// nx_plain_nso on a synthetic ZBIC NSO made from known segments (zbic_frames.inc): the output is a plain NSO with the same segments and
// header, other files are copied, and a damaged NSO is refused (exit code 2) without writing it.
//   test_plain_nso <path of nx_plain_nso.exe>
#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#include "zbic_frames.inc"

int g_failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

void write_file(const fs::path& path, const std::vector<uint8_t>& d) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void sha256(const std::vector<uint8_t>& d, uint8_t out[32]) {
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(d.data()), static_cast<ULONG>(d.size()), out, 32);
}

uint8_t zbic_pattern(size_t i, int seed) {
    static const char* w = "MOD0 text ro data bss nn::os::Thread nn::fs::Mount ";
    return (i % 5 == 0) ? uint8_t(seed + (i * i * 31) % 23) : uint8_t(w[(i / 3 + seed) % 50]);
}

// Runs the tool and returns its exit code (-1 if it could not start).
int run(const fs::path& exe, const fs::path& in, const fs::path& out) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" \"" + in.wstring() + L"\" \"" + out.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::printf("usage: test_plain_nso <nx_plain_nso.exe>\n");
        return 1;
    }
    const fs::path exe = argv[1];
    const fs::path root = fs::temp_directory_path() / ("nxx_plain_nso_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);

    const size_t sz[3] = {0x3000, 0x1800, 0x900};
    const uint8_t* fr[3] = {kSeg0, kSeg1, kSeg2};
    const size_t fn[3] = {sizeof kSeg0, sizeof kSeg1, sizeof kSeg2};
    std::vector<uint8_t> plain[3];
    for (int s = 0; s < 3; ++s) {
        plain[s].resize(sz[s]);
        for (size_t i = 0; i < sz[s]; ++i) plain[s][i] = zbic_pattern(i, s * 11 + 1);
    }
    std::vector<uint8_t> nso(0x100, 0);
    std::memcpy(nso.data(), "NSO0", 4);
    put32(&nso[0x0C], 0x3F | 0x80);
    uint32_t mem = 0;
    for (int s = 0; s < 3; ++s) {
        put32(&nso[0x10 + 0x10 * s], uint32_t(nso.size()));
        put32(&nso[0x14 + 0x10 * s], mem);
        put32(&nso[0x18 + 0x10 * s], uint32_t(sz[s]));
        put32(&nso[0x60 + 4 * s], uint32_t(fn[s]));
        sha256(plain[s], &nso[0xA0 + 0x20 * s]);
        nso.insert(nso.end(), fr[s], fr[s] + fn[s]);
        mem += uint32_t((sz[s] + 0xFFF) & ~size_t(0xFFF));
    }
    std::memset(&nso[0x40], 0xAB, 0x20);

    const fs::path in = root / "exefs", out = root / "plain";
    write_file(in / "main", nso);
    write_file(in / "main.npdm", {'M', 'E', 'T', 'A'});
    CHECK(run(exe, in, out) == 0);
    const std::vector<uint8_t> p = read_file(out / "main");
    CHECK(p.size() == 0x100 + sz[0] + sz[1] + sz[2]);
    if (p.size() >= 0x100) {
        uint32_t flags;
        std::memcpy(&flags, &p[0x0C], 4);
        CHECK(flags == 0x38u);
        CHECK(std::memcmp(&p[0x40], &nso[0x40], 0x20) == 0);
        for (int s = 0; s < 3; ++s) {
            uint32_t off, csz;
            std::memcpy(&off, &p[0x10 + 0x10 * s], 4);
            std::memcpy(&csz, &p[0x60 + 4 * s], 4);
            CHECK(csz == uint32_t(sz[s]));
            CHECK(off + sz[s] <= p.size() && std::memcmp(&p[off], plain[s].data(), sz[s]) == 0);
        }
    }
    CHECK(read_file(out / "main.npdm").size() == 4);

    nso[0x100 + 40] ^= 0x5A;   // damage the text frame: refused, not written as plain
    const fs::path bad_in = root / "bad_exefs", bad_out = root / "bad_plain";
    write_file(bad_in / "main", nso);
    CHECK(run(exe, bad_in, bad_out) == 2);
    CHECK(!fs::exists(bad_out / "main"));

    fs::remove_all(root, ec);
    std::printf(g_failures ? "test_plain_nso: %d failure(s)\n" : "test_plain_nso: ok\n", g_failures);
    return g_failures ? 1 : 0;
}
