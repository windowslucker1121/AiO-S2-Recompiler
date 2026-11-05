// SPDX-License-Identifier: GPL-3.0-or-later
// nx_plain_nso: `nx_extract --plain-nso` without the container readers of tools/setup, so it builds from this repository alone.
// Writes every file of an ExeFS folder to <out>: ZBIC NSOs (runtime/docs/zbic.md) become plain NSOs (segments stored, compressed
// sizes = sizes, bits 0-2 and 7 cleared, hash bits and hashes kept), everything else is copied. Same output as extract.cpp
// nso_to_plain; each decoded segment is checked against its hash in the NSO header.
//   nx_plain_nso <exefs folder> <out folder>
// Exit codes as nx_extract: 0 done, 1 error, 2 bad dump, 4 disk.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "zbic.h"

namespace fs = std::filesystem;
using u8 = unsigned char;
using u32 = uint32_t;

static u32 le32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
static void put32(u8* p, u32 v) { std::memcpy(p, &v, 4); }

static bool sha256_is(const std::vector<u8>& d, const u8* want) {
    u8 h[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<u8*>(d.data()), static_cast<ULONG>(d.size()), h, 32) != 0)
        return false;
    return std::memcmp(h, want, 32) == 0;
}

static bool is_zbic(const std::vector<u8>& d) {
    return d.size() >= 0x100 && std::memcmp(d.data(), "NSO0", 4) == 0 && (le32(&d[0x0C]) & nxrt::kNsoFlagZbic);
}

static const char* to_plain(const std::vector<u8>& d, std::vector<u8>& o) {
    const u32 flags = le32(&d[0x0C]);
    std::vector<u8> seg[3];
    u32 first_off = 0xFFFFFFFF;
    for (int i = 0; i < 3; ++i) {
        const u32 foff = le32(&d[0x10 + 0x10 * i]), dsize = le32(&d[0x18 + 0x10 * i]), csize = le32(&d[0x60 + 4 * i]);
        const bool comp = (flags >> i) & 1;
        const uint64_t stored = comp ? csize : dsize;
        if (foff < 0x100 || foff > d.size() || stored > d.size() - foff) return "a segment lies outside the file";
        first_off = std::min(first_off, foff);
        seg[i].resize(dsize);
        if (!comp) std::memcpy(seg[i].data(), d.data() + foff, dsize);
        else if (!nxrt::zbic_decompress(seg[i].data(), dsize, d.data() + foff, csize)) return "a segment does not decompress (ZBIC)";
        if (((flags >> (3 + i)) & 1) && !sha256_is(seg[i], &d[0xA0 + 0x20 * i])) return "a segment does not match its hash";
    }
    // header and whatever sits between it and the first segment (the module name), then the three segments back to back
    o.assign(d.begin(), d.begin() + first_off);
    put32(&o[0x0C], flags & ~(0x7u | nxrt::kNsoFlagZbic));
    for (int i = 0; i < 3; ++i) {
        put32(&o[0x10 + 0x10 * i], static_cast<u32>(o.size()));
        put32(&o[0x60 + 4 * i], static_cast<u32>(seg[i].size()));
        o.insert(o.end(), seg[i].begin(), seg[i].end());
    }
    return nullptr;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fputs("usage: nx_plain_nso <exefs folder> <out folder>\n", stderr);
        return 1;
    }
    const fs::path in = argv[1], out = argv[2];
    std::error_code ec;
    fs::create_directories(out, ec);
    int converted = 0;
    for (const auto& e : fs::directory_iterator(in, ec)) {
        if (!e.is_regular_file()) continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::vector<u8> d(static_cast<size_t>(e.file_size())), o;
        if (!f.read(reinterpret_cast<char*>(d.data()), d.size())) {
            std::fwprintf(stderr, L"@error cannot read %ls\n", e.path().c_str());
            return 1;
        }
        const bool zbic = is_zbic(d);
        if (zbic) {
            if (const char* err = to_plain(d, o)) {
                std::fwprintf(stderr, L"@error %ls: %hs (the file is damaged)\n", e.path().c_str(), err);
                return 2;
            }
            converted++;
        }
        const fs::path dst = out / e.path().filename(), tmp = dst.wstring() + L".part";
        const std::vector<u8>& w = zbic ? o : d;
        {
            std::ofstream of(tmp, std::ios::binary | std::ios::trunc);
            if (!of.write(reinterpret_cast<const char*>(w.data()), w.size())) {
                std::fwprintf(stderr, L"@error cannot write %ls\n", tmp.c_str());
                return 4;
            }
        }
        fs::rename(tmp, dst, ec);
        if (ec) {
            std::fwprintf(stderr, L"@error cannot write %ls\n", dst.c_str());
            return 4;
        }
    }
    if (ec) {
        std::fwprintf(stderr, L"@error cannot read the folder %ls\n", in.c_str());
        return 1;
    }
    std::printf("@result %d ZBIC module(s) converted\n", converted);
    return 0;
}
