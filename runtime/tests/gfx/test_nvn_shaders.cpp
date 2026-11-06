// SPDX-License-Identifier: GPL-3.0-or-later
// Shader cache (src/gfx/nvn/nvn_shaders.{h,cpp}): SHA-1 vectors, the control section, the cache's failure path, and the
// full offline cache: every block of the corpus manifest is looked up through shader_cache() and checked against the
// manifest (size, hash, stage, constants) and against the harness's results.tsv record (reflected bindings).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "nvn_shaders.h"
#include "test_framework.h"
#include "project_root.h"
NXTEST_GROUP(gfx_nvn_shaders)

using namespace nxrt::nvn::be;
namespace fs = std::filesystem;

namespace {

const std::string kCorpusDirStr = nxtest::project_path("work/shader_cache/corpus_code");
const char* kCorpusDir = kCorpusDirStr.c_str();

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) out.push_back(cur);
    if (!s.empty() && s.back() == sep) out.push_back("");
    return out;
}

std::vector<std::string> sorted_tokens(const std::string& s) {
    std::vector<std::string> t;
    for (auto& x : split(s, ' '))
        if (!x.empty()) t.push_back(x);
    std::sort(t.begin(), t.end());
    return t;
}

// The control section of a block as the corpus files hold it (0x880 bytes), zeroed except what is set.
void make_control(uint8_t* ctrl, uint32_t code_size, uint32_t const_offset, uint32_t const_size) {
    std::memset(ctrl, 0, 0x880);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(ctrl + off, &v, 4); };
    put(0, kShaderControlMagic);
    put(0x6f8, 0x100);
    put(0x6fc, const_size);
    put(0x700, const_offset);
    put(0x704, code_size);
    put(0x708, 0x30);
    put(0x748, 8);
    put(0x74c, 4);
    put(0x750, 2);
}

}  // namespace

NXTEST(sha1_standard_vectors) {
    CHECK_EQ(sha1_hex("", 0), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(sha1_hex("abc", 3), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    const std::string two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_EQ(sha1_hex(two_block.data(), two_block.size()), std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    // One million 'a', fed in uneven pieces to exercise the buffering.
    Sha1 s;
    std::string a(1000000, 'a');
    size_t off = 0, step = 1;
    while (off < a.size()) {
        size_t n = std::min(step, a.size() - off);
        s.update(a.data() + off, n);
        off += n;
        step = step * 7 % 131 + 1;
    }
    uint8_t d[20];
    s.finish(d);
    char hex[41];
    for (int i = 0; i < 20; i++) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
    CHECK_EQ(std::string(hex), std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

// Numbers in a harness record that do not fit an int are refused (the parser used a 32-bit long and overflowed before its range check).
NXTEST(harness_numbers_out_of_range) {
    std::vector<HarnessResource> r;
    CHECK(parse_harness_resources("cb2147483647=s0b1", r));
    CHECK_EQ(r.size(), size_t(1));
    if (!r.empty()) CHECK_EQ(r[0].bank, 2147483647);
    CHECK(!parse_harness_resources("cb2147483648=s0b1", r));
    CHECK(!parse_harness_resources("cb4294967297=s0b1", r));      // wrapped to 1 with a 32-bit accumulator
    CHECK(!parse_harness_resources("cb99999999999=s0b1", r));
    CHECK(!parse_harness_resources("sb[c0+0x100000000]=s0b1", r));
}

// A texture whose handle the draw computes (translator/ampere-bindless-handle-expr.patch): the expression after '|' (it holds an "x" in
// "xor", which is not the array length), parsed into a program that leaves one value.
NXTEST(harness_handle_expression) {
    std::vector<HarnessResource> r;
    CHECK(parse_harness_resources("texTexture2D[c131072:2|c4.33,#c0000000,and,#80000000,ne,not,c5.187,#5,shl,#10,xor,#10,sel,#2,shr,s324]=s2b150", r));
    CHECK_EQ(r.size(), size_t(1));
    if (r.empty()) return;
    CHECK_EQ(r[0].bank, kHandleExprBank);
    CHECK_EQ(r[0].word, 2);
    CHECK_EQ(r[0].array_length, 1);
    std::vector<HandleExprOp> prog;
    CHECK(parse_handle_expr(r[0].expr, prog));
    CHECK_EQ(prog.size(), size_t(16));
    if (prog.size() == 16) {
        CHECK(prog[0].kind == HandleExprOp::Cbuf && prog[0].a == 4 && prog[0].b == 33);
        CHECK(prog[1].kind == HandleExprOp::Const && prog[1].a == 0xc0000000u);
        CHECK(prog[15].kind == HandleExprOp::Storage && prog[15].a == 324 * 4);
    }
    CHECK(!parse_handle_expr("c4.33,#1", prog));        // two values left
    CHECK(!parse_handle_expr("add", prog));             // nothing to add
    CHECK(!parse_handle_expr("c4.33,frob", prog));      // unknown token
}

NXTEST(shader_hash_is_sha1_prefix) {
    CHECK_EQ(shader_hash(reinterpret_cast<const uint8_t*>("abc"), 3), std::string("a9993e364706816a"));
}

NXTEST(read_control_fields) {
    uint8_t ctrl[0x880];
    make_control(ctrl, 0x1400, 0x1300, 0x18);
    ShaderControl c = read_control(ctrl);
    CHECK(c.valid);
    CHECK_EQ(c.code_size, 0x1400u);
    CHECK_EQ(c.const_offset, 0x1300u);
    CHECK_EQ(c.const_size, 0x18u);
    CHECK_EQ(c.program_start, 0x30u);
    CHECK_EQ(c.program_size, 0x100u);
    CHECK_EQ(c.local_size[0], 8u);
    CHECK_EQ(c.local_size[1], 4u);
    CHECK_EQ(c.local_size[2], 2u);
    std::memset(ctrl, 0, sizeof ctrl);
    CHECK(!read_control(ctrl).valid);
}

// NVN2 compute: the c[0] resource table. Control bytes of OoT's fea98fe719bc0348 (a compute program built into main, part of the
// post-process chain): no uniform blocks, 3 storage buffers, 1 texture, 2 images, table 0xd8 bytes = 0x10 header + 0x80 push constants
// + 3 * 0x10 + 3 * 8, 29 push words read. The harness printed sb[c0+0x2c], texTexture2D[c0:48] and imgTexture2D[c0:52].
namespace {
void make_nvn2_compute_control(uint8_t* ctrl, uint8_t ubos, uint8_t ssbos, uint8_t textures, uint8_t images, uint16_t table, uint8_t push_words) {
    std::memset(ctrl, 0, 0xd8);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(ctrl + off, &v, 4); };
    put(0, kShaderControlMagic);
    put(4, 201);
    put(0x38, 0x100);   // program
    put(0x44, 0x100);   // code block
    ctrl[0x56] = 5;     // compute
    put(0x58, 128);
    put(0x5c, 1);
    put(0x60, 1);
    ctrl[0xa9] = push_words;
    ctrl[0xb1] = ubos;
    ctrl[0xb2] = ssbos;
    ctrl[0xb3] = textures;
    ctrl[0xb6] = images;
    ctrl[0xba] = static_cast<uint8_t>(table);
    ctrl[0xbb] = static_cast<uint8_t>(table >> 8);
}
DescriptorBinding binding_at(uint32_t b, DescriptorType type, int c0_offset, int word) {
    DescriptorBinding d;
    d.set = type == DescriptorType::StorageBuffer ? 1 : type == DescriptorType::StorageImage ? 3 : 2;
    d.binding = b;
    d.type = type;
    d.used = true;
    d.nvn.c0_offset = c0_offset;
    d.nvn.bank = word >= 0 ? 0 : -1;
    d.nvn.word = word;
    return d;
}
}  // namespace

NXTEST(nvn2_compute_table_with_push_constants) {
    uint8_t ctrl[0xd8], code[0x100] = {};
    make_nvn2_compute_control(ctrl, 0, 3, 1, 2, 0xd8, 0x1d);
    const ShaderControl c = read_control(ctrl);
    CHECK(c.valid && c.nvn2);
    CHECK_EQ(c.stage, 5);
    CHECK_EQ(c.c0_table, 0xd8);
    CHECK_EQ(c.c0_push, 0x80);
    CHECK_EQ(c.c0_push_words, 0x1d);
    ShaderReflection refl;
    refl.stage = ShaderStage::Compute;
    refl.bindings.push_back(binding_at(0, DescriptorType::StorageBuffer, 0x2c * 4, -1));
    refl.bindings.push_back(binding_at(0, DescriptorType::CombinedImageSampler, -1, 48));
    refl.bindings.push_back(binding_at(0, DescriptorType::StorageImage, -1, 52));
    ShaderInfo info;
    std::string err;
    CHECK(make_shader_info(code, ctrl, {}, nullptr, info, &err, &refl));
    if (info.refl.bindings.size() != 3) return;
    const NvnMeaning& sb = info.refl.bindings[0].nvn;
    const NvnMeaning& tex = info.refl.bindings[1].nvn;
    const NvnMeaning& img = info.refl.bindings[2].nvn;
    CHECK(sb.source == NvnSource::StorageBuffer);
    CHECK_EQ(sb.index, 2);          // 0xb0 = 0x90 + 2 * 0x10
    CHECK(tex.source == NvnSource::SamplerBinding);
    CHECK_EQ(tex.index, 0);         // 0xc0
    CHECK(img.source == NvnSource::ImageBinding);
    CHECK_EQ(img.index, 1);         // 0xd0 = 0xc8 + 8
    // the same program without push constants (a corpus-style table of 0x58 bytes) keeps the 0x10-byte header only: 0xb0 is then outside
    make_nvn2_compute_control(ctrl, 0, 3, 1, 2, 0x58, 0);
    CHECK_EQ(read_control(ctrl).c0_push, 0);
    CHECK(make_shader_info(code, ctrl, {}, nullptr, info, &err, &refl));
    if (info.refl.bindings.size() == 3) CHECK(info.refl.bindings[0].nvn.source == NvnSource::Unknown);
    // 54ecc4b4d30b3e2c: 11 uniform blocks, 9 storage buffers, 7 textures, 2 images, table 0x218: uniform block 10 at 0x130 (read as
    // storage), storage buffer 8 at 0x1c0, textures 4 and 6 at words 124 and 128, image 1 at word 132
    make_nvn2_compute_control(ctrl, 11, 9, 7, 2, 0x218, 0x1d);
    refl.bindings.clear();
    refl.bindings.push_back(binding_at(0, DescriptorType::StorageBuffer, 0x4c * 4, -1));
    refl.bindings.push_back(binding_at(1, DescriptorType::StorageBuffer, 0x70 * 4, -1));
    refl.bindings.push_back(binding_at(0, DescriptorType::CombinedImageSampler, -1, 124));
    refl.bindings.push_back(binding_at(1, DescriptorType::CombinedImageSampler, -1, 128));
    refl.bindings.push_back(binding_at(0, DescriptorType::StorageImage, -1, 132));
    CHECK(make_shader_info(code, ctrl, {}, nullptr, info, &err, &refl));
    if (info.refl.bindings.size() != 5) return;
    CHECK(info.refl.bindings[0].nvn.source == NvnSource::UniformAsStorage);
    CHECK_EQ(info.refl.bindings[0].nvn.index, 10);
    CHECK(info.refl.bindings[1].nvn.source == NvnSource::StorageBuffer);
    CHECK_EQ(info.refl.bindings[1].nvn.index, 8);
    CHECK_EQ(info.refl.bindings[2].nvn.index, 4);
    CHECK_EQ(info.refl.bindings[3].nvn.index, 6);
    CHECK(info.refl.bindings[4].nvn.source == NvnSource::ImageBinding);
    CHECK_EQ(info.refl.bindings[4].nvn.index, 1);
}

NXTEST(read_control_on_first_corpus_block) {
    std::string manifest, code, ctrl;
    if (!read_file(fs::path(kCorpusDir) / "manifest.tsv", manifest)) {
        nxtest::skip("corpus manifest missing");
        return;
    }
    auto lines = split(manifest, '\n');
    if (lines.size() < 2) {
        nxtest::skip("corpus manifest empty");
        return;
    }
    auto head = split(lines[0], '\t');
    auto row = split(lines[1], '\t');
    auto col = [&](const char* name) {
        for (size_t i = 0; i < head.size(); i++)
            if (head[i] == name) return row[i];
        return std::string();
    };
    if (!read_file(fs::path(kCorpusDir) / col("code_file"), code) ||
        !read_file(fs::path(kCorpusDir) / col("ctrl_file"), ctrl) || ctrl.size() < 0x880) {
        nxtest::skip("corpus files missing");
        return;
    }
    ShaderControl c = read_control(reinterpret_cast<const uint8_t*>(ctrl.data()));
    CHECK(c.valid);
    CHECK_EQ(uint64_t(c.code_size), uint64_t(code.size()));
    CHECK_EQ(uint64_t(c.const_offset), std::stoull(col("const_off"), nullptr, 16));
    CHECK_EQ(uint64_t(c.const_size), std::stoull(col("const_size"), nullptr, 16));
    CHECK_EQ(uint64_t(c.program_start), std::stoull(col("program_start"), nullptr, 16));
}

NXTEST(cache_missing_directory_fails_quietly) {
    ShaderCache c;
    CHECK(!c.open("D:/no/such/shader/cache"));
    CHECK(!c.is_open());
    uint8_t code[64] = {}, ctrl[0x880];
    make_control(ctrl, sizeof code, 0, 0);
    CHECK(c.find(code, ctrl) == nullptr);
    CHECK(c.find_by_hash("0123456789abcdef") == nullptr);
    CHECK_EQ(c.loaded(), size_t(0));
    CHECK_EQ(c.missing(), size_t(0));
}

NXTEST(cache_rejects_malformed_control) {
    ShaderCache c;
    uint8_t code[64] = {}, ctrl[0x880] = {};  // no magic
    CHECK(c.find(code, ctrl) == nullptr);
    CHECK(c.find(nullptr, ctrl) == nullptr);
}

NXTEST(corpus_all_blocks_through_shader_cache) {
    std::string manifest;
    if (!read_file(fs::path(kCorpusDir) / "manifest.tsv", manifest)) {
        nxtest::skip("corpus manifest missing");
        return;
    }
    if (!shader_cache().is_open()) {
        nxtest::skip("shader cache not open (no results.tsv in the configured or default folder)");
        return;
    }
    const fs::path dir(kCorpusDir);
    const std::string cache_dir = shader_cache().dir();
    std::string tsv;
    if (!read_file(fs::path(cache_dir) / "results.tsv", tsv)) {
        nxtest::skip("results.tsv unreadable");
        return;
    }
    std::unordered_map<std::string, std::string> resources;
    parse_results_tsv(tsv, resources);

    std::vector<std::string> head, failures, unknown;
    size_t rows = 0, found = 0, mismatches = 0, bindings = 0, with_storage = 0;
    std::map<std::string, size_t> stages, sources;
    auto fail_row = [&](const std::string& id, const std::string& why) {
        if (mismatches++ < 10) failures.push_back(id + ": " + why);
    };
    for (const std::string& raw : split(manifest, '\n')) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto cells = split(line, '\t');
        if (cells.size() < 13) continue;
        if (head.empty()) {
            head = cells;
            continue;
        }
        auto col = [&](const char* name) {
            for (size_t i = 0; i < head.size(); i++)
                if (head[i] == name) return cells[i];
            return std::string();
        };
        rows++;
        const std::string id = col("id"), hash = col("code_sha1_16"), stage = col("stage");
        std::string code, ctrl;
        if (!read_file(dir / col("code_file"), code) || !read_file(dir / col("ctrl_file"), ctrl) || ctrl.size() < 0x880) {
            fail_row(id, "code or control file missing");
            continue;
        }
        const auto* cb = reinterpret_cast<const uint8_t*>(code.data());
        const auto* cc = reinterpret_cast<const uint8_t*>(ctrl.data());
        ShaderControl sc = read_control(cc);
        if (!sc.valid) {
            fail_row(id, "control magic");
            continue;
        }
        if (sc.code_size != code.size() || sc.code_size != std::stoul(col("code_size"), nullptr, 16)) {
            fail_row(id, "code size at control +0x704 differs from the file");
            continue;
        }
        if (shader_hash(cb, sc.code_size) != hash) {
            fail_row(id, "shader_hash(code) differs from code_sha1_16");
            continue;
        }
        const ShaderInfo* info = shader_cache().find(cb, cc);
        if (!info) {
            fail_row(id, "not found in the cache");
            continue;
        }
        found++;
        if (found == 1) {  // the same block again returns the same ShaderInfo, and find_by_hash sees it
            CHECK(shader_cache().find(cb, cc) == info);
            CHECK(shader_cache().find_by_hash(hash) == info);
        }
        if (info->hash != hash) fail_row(id, "ShaderInfo.hash differs");
        if (stage_name(info->stage) != stage) fail_row(id, "stage " + std::string(stage_name(info->stage)) + " vs " + stage);
        if (info->constants.size() != std::stoul(col("const_size"), nullptr, 16) ||
            info->constants.size() != sc.const_size)
            fail_row(id, "constant data length differs from const_size");
        auto rec = resources.find(hash);
        if (rec == resources.end()) {
            fail_row(id, "no results.tsv record");
        } else {
            std::string mine = describe_like_harness(info->refl);
            if (sorted_tokens(mine) != sorted_tokens(rec->second))
                fail_row(id, "reflected \"" + mine + "\" vs harness \"" + rec->second + "\"");
        }
        stages[stage_name(info->stage)]++;
        bool storage = false;
        for (const auto& b : info->refl.bindings) {
            bindings++;
            sources[source_name(b.nvn.source)]++;
            if (b.nvn.source == NvnSource::Unknown) {
                unknown.push_back(id + " set" + std::to_string(b.set) + " binding" + std::to_string(b.binding) + " " +
                                  b.name);
                fail_row(id, "binding " + b.name + " has no NVN meaning");
            }
            if (b.type == DescriptorType::StorageBuffer) storage = true;
        }
        if (storage) with_storage++;
    }

    std::printf("corpus: %zu blocks checked, %zu found in %s, %zu mismatches, %zu bindings, %zu blocks with storage "
                "buffers\n",
                rows, found, cache_dir.c_str(), mismatches, bindings, with_storage);
    for (auto& [k, v] : stages) std::printf("  stage %-9s %zu\n", k.c_str(), v);
    for (auto& [k, v] : sources) std::printf("  binding %-28s %zu\n", k.c_str(), v);
    for (auto& u : unknown) std::printf("  UNKNOWN meaning: %s\n", u.c_str());
    for (auto& f : failures) std::printf("  FAIL %s\n", f.c_str());
    std::printf("  loaded %zu, missing %zu\n", shader_cache().loaded(), shader_cache().missing());
    CHECK(rows > 0);
    CHECK_EQ(found, rows);
    CHECK_EQ(mismatches, size_t(0));
}
