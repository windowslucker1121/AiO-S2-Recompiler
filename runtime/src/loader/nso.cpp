// SPDX-License-Identifier: GPL-3.0-or-later
// NSO loader: turns a platform 1 executable (the NSO0 container of a title's ExeFS: main, rtld, sdk, subsdk*) into a mapped
// Module. The three segments are LZ4-decompressed (block format) into a synthetic ELF64 image (ET_DYN, one PT_LOAD per segment,
// PT_DYNAMIC found through the MOD0 header whose offset sits at text +4) which the ordinary ELF loader (module.cpp) then maps,
// parses and links. The NSO's own addresses are module relative, as in the ELF, so the lifted code's baked bases and DynSym values
// agree with the mapping.
// Format: Switchbrew "NSO" page (header at 0x0, segments at 0x10/0x20/0x30, bss size 0x3C, compressed sizes 0x60). LZ4 block format:
// lz4_Block_format.md of github.com/lz4/lz4 (reimplemented here). Flag bit 7 (UseZbicCompression, firmware 22.0.0+): the compressed
// segments are ZBIC frames instead of LZ4 blocks (zbic.cpp, docs/zbic.md).
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "elf.h"
#include "zbic.h"
#include "nxrt/log.h"
#include "nxrt/module.h"

namespace nxrt {
namespace {

constexpr const char* kTag = "loader";
constexpr uint64_t kHeaderPad = 0x1000;     // the synthetic ELF keeps its headers in the first page, the image follows at +0x1000

bool fail(std::string* err, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (err) *err = buf;
    NXLOG_E(kTag, "%s", buf);
    return false;
}

template <class T>
T rd(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof v); return v; }

// LZ4 block decompression of exactly dst_size bytes. False on any malformed or truncated stream.
bool lz4_block_decode(const uint8_t* src, size_t n, uint8_t* dst, size_t dst_size) {
    size_t ip = 0, op = 0;
    while (ip < n) {
        const unsigned token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= n) return false;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > n - ip || lit > dst_size - op) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip >= n) break;                                   // the last sequence has literals only
        if (n - ip < 2) return false;
        const size_t off = src[ip] | (static_cast<size_t>(src[ip + 1]) << 8);
        ip += 2;
        if (off == 0 || off > op) return false;
        size_t len = token & 15;
        if (len == 15) {
            unsigned b;
            do {
                if (ip >= n) return false;
                b = src[ip++];
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (len > dst_size - op) return false;
        for (size_t i = 0; i < len; i++) dst[op + i] = dst[op - off + i];   // overlapping copies are the point of LZ4
        op += len;
    }
    return op == dst_size;
}

}  // namespace

Module* module_load_nso_memory(const void* data, size_t size, const LoadOptions& opt, std::string* err) {
    const uint8_t* d = static_cast<const uint8_t*>(data);
    if (size < 0x100 || std::memcmp(d, "NSO0", 4) != 0) { fail(err, "not an NSO image (no NSO0 magic)"); return nullptr; }
    const uint32_t flags = rd<uint32_t>(d + 0x0C);
    struct Seg { uint32_t file_off, mem_off, dsize, csize; };
    Seg seg[3];
    for (int i = 0; i < 3; i++) {
        seg[i].file_off = rd<uint32_t>(d + 0x10 + 0x10 * i);
        seg[i].mem_off = rd<uint32_t>(d + 0x14 + 0x10 * i);
        seg[i].dsize = rd<uint32_t>(d + 0x18 + 0x10 * i);
        seg[i].csize = rd<uint32_t>(d + 0x60 + 4 * i);
    }
    const uint32_t bss = rd<uint32_t>(d + 0x3C);
    uint64_t image_end = 0;
    for (int i = 0; i < 3; i++) {
        const bool lz4 = (flags >> i) & 1;
        const uint64_t stored = lz4 ? seg[i].csize : seg[i].dsize;
        if (seg[i].file_off > size || stored > size - seg[i].file_off) { fail(err, "NSO segment %d is outside the file", i); return nullptr; }
        if (i && seg[i].mem_off < seg[i - 1].mem_off + seg[i - 1].dsize) { fail(err, "NSO segments overlap in memory"); return nullptr; }
        image_end = static_cast<uint64_t>(seg[i].mem_off) + seg[i].dsize;
    }
    if (image_end + bss > opt.max_image_size) { fail(err, "NSO image span 0x%llx exceeds the limit", static_cast<unsigned long long>(image_end + bss)); return nullptr; }

    // The synthetic ELF: [headers: 0x1000][memory image 0 .. image_end)
    std::vector<uint8_t> elf(static_cast<size_t>(kHeaderPad + image_end), 0);
    uint8_t* img = elf.data() + kHeaderPad;
    for (int i = 0; i < 3; i++) {
        const bool lz4 = (flags >> i) & 1;
        if (lz4 && (flags & kNsoFlagZbic)) {
            if (!zbic_decompress(img + seg[i].mem_off, seg[i].dsize, d + seg[i].file_off, seg[i].csize)) {
                fail(err, "NSO segment %d: ZBIC stream is corrupt (compressed 0x%x, expected 0x%x bytes)", i, seg[i].csize, seg[i].dsize);
                return nullptr;
            }
        } else if (lz4) {
            if (!lz4_block_decode(d + seg[i].file_off, seg[i].csize, img + seg[i].mem_off, seg[i].dsize)) {
                fail(err, "NSO segment %d: LZ4 stream is corrupt (compressed 0x%x, expected 0x%x bytes)", i, seg[i].csize, seg[i].dsize);
                return nullptr;
            }
        } else {
            std::memcpy(img + seg[i].mem_off, d + seg[i].file_off, seg[i].dsize);
        }
    }

    // MOD0: text +4 holds its offset; the header gives the dynamic section relative to itself.
    uint64_t dyn_vaddr = 0, dyn_size = 0;
    if (seg[0].dsize >= 8) {
        const uint32_t mod0 = rd<uint32_t>(img + 4);
        if (mod0 + 8ull <= image_end && std::memcmp(img + mod0, "MOD0", 4) == 0) {
            dyn_vaddr = mod0 + static_cast<int64_t>(rd<int32_t>(img + mod0 + 4));
            if (dyn_vaddr + sizeof(elf::Dyn) > image_end) dyn_vaddr = 0;
            for (uint64_t off = 0; dyn_vaddr && dyn_vaddr + off + sizeof(elf::Dyn) <= image_end; off += sizeof(elf::Dyn)) {
                dyn_size = off + sizeof(elf::Dyn);
                if (rd<int64_t>(img + dyn_vaddr + off) == elf::DT_NULL) break;
            }
        }
    }
    if (!dyn_vaddr) NXLOG_W(kTag, "NSO %s has no MOD0 / dynamic section: loaded without relocations or symbols", opt.name.c_str());

    elf::Ehdr eh{};
    std::memcpy(eh.ident, "\x7f" "ELF", 4);
    eh.ident[4] = 2; eh.ident[5] = 1; eh.ident[6] = 1;
    eh.type = elf::ET_DYN;
    eh.machine = elf::EM_AARCH64;
    eh.version = 1;
    eh.phoff = sizeof(elf::Ehdr);
    eh.ehsize = sizeof(elf::Ehdr);
    eh.phentsize = sizeof(elf::Phdr);
    elf::Phdr ph[4] = {};
    int n = 0;
    const uint32_t kFlags[3] = {elf::PF_R | elf::PF_X, elf::PF_R, elf::PF_R | elf::PF_W};
    for (int i = 0; i < 3; i++) {
        if (!seg[i].dsize && !(i == 2 && bss)) continue;
        ph[n].type = elf::PT_LOAD;
        ph[n].flags = kFlags[i];
        ph[n].offset = kHeaderPad + seg[i].mem_off;
        ph[n].vaddr = ph[n].paddr = seg[i].mem_off;
        ph[n].filesz = seg[i].dsize;
        ph[n].memsz = seg[i].dsize + (i == 2 ? bss : 0);
        ph[n].align = 0x1000;
        n++;
    }
    if (dyn_vaddr) {
        ph[n].type = elf::PT_DYNAMIC;
        ph[n].flags = elf::PF_R | elf::PF_W;
        ph[n].offset = kHeaderPad + dyn_vaddr;
        ph[n].vaddr = ph[n].paddr = dyn_vaddr;
        ph[n].filesz = ph[n].memsz = dyn_size;
        ph[n].align = 8;
        n++;
    }
    eh.phnum = static_cast<uint16_t>(n);
    std::memcpy(elf.data(), &eh, sizeof eh);
    std::memcpy(elf.data() + sizeof eh, ph, sizeof(elf::Phdr) * n);
    NXLOG_I(kTag, "NSO %s: text 0x%x ro 0x%x data 0x%x bss 0x%x, flags 0x%x, dynamic at 0x%llx", opt.name.c_str(), seg[0].dsize, seg[1].dsize, seg[2].dsize, bss, flags,
            static_cast<unsigned long long>(dyn_vaddr));
    return module_load_memory(elf.data(), elf.size(), opt, err);
}

Module* module_load_nso_file(const char* path, const LoadOptions& opt_in, std::string* err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { fail(err, "cannot open %s", path); return nullptr; }
    const std::streamsize sz = f.tellg();
    if (sz <= 0 || static_cast<uint64_t>(sz) > (1ull << 31)) { fail(err, "%s has an unreasonable size (%lld)", path, static_cast<long long>(sz)); return nullptr; }
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), sz);
    if (!f) { fail(err, "short read on %s", path); return nullptr; }
    LoadOptions opt = opt_in;
    if (opt.name.empty()) {
        const char* s1 = std::strrchr(path, '\\');
        const char* s2 = std::strrchr(path, '/');
        if (s2 > s1) s1 = s2;
        opt.name = s1 ? s1 + 1 : path;
    }
    return module_load_nso_memory(bytes.data(), bytes.size(), opt, err);
}

}  // namespace nxrt
