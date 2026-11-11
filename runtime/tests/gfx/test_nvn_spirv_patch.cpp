// SPDX-License-Identifier: GPL-3.0-or-later
// SPIR-V rewrites of the back end (src/gfx/nvn/nvn_spirv_patch.{h,cpp}) on the translated shaders of the shader cache. Each
// rewritten module must still pass spirv-val (vulkan1.3) and reflect to the declarations the rewrite promises. Cases skip
// when the cache or the Vulkan SDK's spirv-val is absent.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "nvn_shaders.h"
#include "nvn_spirv_patch.h"
#include "test_framework.h"
#include "project_root.h"
NXTEST_GROUP(gfx_nvn_spirv_patch)

using namespace nxrt::nvn::be;
namespace gpu = nxrt::nvn::be::gpu;
namespace fs = std::filesystem;

namespace {

const std::string kSpvDirStr = nxtest::project_path("work/shader_cache/spirv");
const char* kSpvDir = kSpvDirStr.c_str();
const char* kSpirvVal = "C:\\VulkanSDK\\1.4.363.0\\Bin\\spirv-val.exe";
const char* kVertexHash = "a656883b0cdb8ebc";      // position only, no varyings
const char* kConstFragHash = "062cfa5657f5388d";   // constant colour fragment shader, no Location 1 output

constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kDecorationLocation = 30;
constexpr uint32_t kDecorationIndex = 32;

// nullptr when the inputs of a case are present, else the reason to skip.
const char* absent_input() {
    std::error_code ec;
    if (!fs::exists(fs::path(kSpvDir) / (std::string(kVertexHash) + ".spv"), ec))
        return "shader cache absent (a656883b0cdb8ebc.spv not under work/shader_cache/spirv)";
    if (!fs::exists(kSpirvVal, ec)) return "spirv-val not found at C:\\VulkanSDK\\1.4.363.0\\Bin";
    return nullptr;
}

bool load_words(const fs::path& p, std::vector<uint32_t>& w) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0 || n % 4) return false;
    f.seekg(0);
    w.resize(size_t(n) / 4);
    return bool(f.read(reinterpret_cast<char*>(w.data()), n));
}

bool load_hash(const std::string& hash, std::vector<uint32_t>& w) { return load_words(fs::path(kSpvDir) / (hash + ".spv"), w); }

bool write_words(const std::string& path, const std::vector<uint32_t>& w) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size() * 4));
    return bool(f);
}

// Runs spirv-val --target-env vulkan1.3 on the words (through a temp file in the working directory). Returns its exit code
// (0 = valid), or -1 when the file could not be written. `log` gets the start of its output.
int run_spirv_val(const std::vector<uint32_t>& w, const std::string& tag, std::string& log) {
    const std::string spv = tag + ".spv", txt = tag + ".log";
    if (!write_words(spv, w)) return -1;
    const std::string cmd = std::string(kSpirvVal) + " --target-env vulkan1.3 " + spv + " > " + txt + " 2>&1";
    const int rc = std::system(cmd.c_str());
    std::ifstream f(txt);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    log = text.substr(0, 600);
    std::error_code ec;
    fs::remove(spv, ec);
    fs::remove(txt, ec);
    return rc;
}

bool reflect(const std::vector<uint32_t>& w, ShaderReflection& r, std::string& err) {
    err.clear();
    return reflect_spirv(w.data(), w.size(), r, &err);
}

const ShaderIo* find_io(const std::vector<ShaderIo>& v, uint32_t location) {
    for (const ShaderIo& io : v)
        if (io.location == location) return &io;
    return nullptr;
}

const char* type_name(ScalarType t) {
    switch (t) {
        case ScalarType::Float: return "Float";
        case ScalarType::Sint: return "Sint";
        case ScalarType::Uint: return "Uint";
        case ScalarType::Bool: return "Bool";
        default: return "Other";
    }
}

struct Deco {
    uint32_t target, kind, value;
};

// Every OpDecorate with the 4-word form (target, decoration, one literal) of the module.
std::vector<Deco> decorations(const std::vector<uint32_t>& w) {
    std::vector<Deco> out;
    for (size_t i = 5; i < w.size();) {
        const uint32_t len = w[i] >> 16;
        if (!len || i + len > w.size()) break;
        if ((w[i] & 0xffff) == kOpDecorate && len == 4) out.push_back({w[i + 1], w[i + 2], w[i + 3]});
        i += len;
    }
    return out;
}

std::set<uint32_t> locations(const std::vector<ShaderIo>& v) {
    std::set<uint32_t> s;
    for (const ShaderIo& io : v) s.insert(io.location);
    return s;
}

}  // namespace

NXTEST(add_outputs_to_real_vertex_shader) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    std::vector<uint32_t> in;
    REQUIRE(load_hash(kVertexHash, in));
    const std::vector<uint32_t> before = in;

    ShaderReflection r_in;
    std::string err;
    CHECK_MSG(reflect(in, r_in, err), err);
    CHECK(r_in.stage == ShaderStage::Vertex);

    const std::vector<gpu::SpvOutputSlot> slots = {
        {0, gpu::SpvScalar::Float}, {3, gpu::SpvScalar::Float}, {5, gpu::SpvScalar::Uint}, {6, gpu::SpvScalar::Sint}};
    std::vector<uint32_t> out;
    REQUIRE(gpu::spirv_add_outputs(in, slots, out));
    CHECK(in == before);
    CHECK(out.size() > in.size());
    CHECK(out[3] > in[3]);

    std::string log;
    const int rc = run_spirv_val(out, "spirv_patch_vs", log);
    CHECK_MSG(rc == 0, "spirv-val exit " + std::to_string(rc) + ": " + log);

    ShaderReflection r;
    CHECK_MSG(reflect(out, r, err), err);
    CHECK_EQ(r.outputs.size(), slots.size());
    struct Want {
        uint32_t loc;
        ScalarType type;
    };
    const std::vector<Want> want = {{0, ScalarType::Float}, {3, ScalarType::Float}, {5, ScalarType::Uint}, {6, ScalarType::Sint}};
    for (const Want& wv : want) {
        const ShaderIo* io = find_io(r.outputs, wv.loc);
        CHECK_MSG(io != nullptr, "no output at Location " + std::to_string(wv.loc));
        if (!io) continue;
        CHECK_MSG(io->type == wv.type, "Location " + std::to_string(wv.loc) + " is " + type_name(io->type) + ", want " + type_name(wv.type));
        CHECK_MSG(io->components == 4, "Location " + std::to_string(wv.loc) + " has " + std::to_string(io->components) + " components");
    }
}

NXTEST(add_outputs_twice_is_refused) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    std::vector<uint32_t> in;
    REQUIRE(load_hash(kVertexHash, in));
    const std::vector<gpu::SpvOutputSlot> slots = {
        {0, gpu::SpvScalar::Float}, {3, gpu::SpvScalar::Float}, {5, gpu::SpvScalar::Uint}, {6, gpu::SpvScalar::Sint}};
    std::vector<uint32_t> once;
    REQUIRE(gpu::spirv_add_outputs(in, slots, once));

    const std::vector<uint32_t> keep = {0xdeadbeefu, 7u};
    std::vector<uint32_t> out = keep;
    CHECK(!gpu::spirv_add_outputs(once, slots, out));
    CHECK(out == keep);
}

NXTEST(add_outputs_bad_input) {
    // The vertex shader when the cache is present, else a header-only stand-in: the checks below need only a module with
    // one instruction at word 5, so they run without the cache too.
    std::vector<uint32_t> base;
    if (!load_hash(kVertexHash, base)) base = {0x07230203u, 0x00010300u, 0u, 5u, 0u, 0u};
    const std::vector<gpu::SpvOutputSlot> slots = {{0, gpu::SpvScalar::Float}};
    const std::vector<uint32_t> keep = {0xdeadbeefu, 7u};

    std::vector<uint32_t> out = keep;
    CHECK(!gpu::spirv_add_outputs(base, {}, out));
    CHECK(out == keep);

    out = keep;
    CHECK(!gpu::spirv_add_outputs({}, slots, out));
    CHECK(out == keep);

    out = keep;
    CHECK(!gpu::spirv_add_outputs({0x07230203u, 0x00010300u, 0u, 5u}, slots, out));
    CHECK(out == keep);

    // First instruction word count 0: the word at index 5 keeps its opcode bits and loses its length.
    std::vector<uint32_t> zero_len = base;
    zero_len[5] &= 0xffffu;
    out = keep;
    CHECK(!gpu::spirv_add_outputs(zero_len, slots, out));
    CHECK(out == keep);
}

NXTEST(dual_source_on_real_fragment_shader) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    // Directory order; the first 400 files first, then the rest.
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(kSpvDir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".spv") files.push_back(it->path());

    std::vector<uint32_t> in;
    ShaderReflection r_in;
    fs::path chosen;
    auto try_file = [&](const fs::path& p) {
        std::vector<uint32_t> w;
        if (!load_words(p, w)) return false;
        ShaderReflection rr;
        if (!reflect_spirv(w.data(), w.size(), rr, nullptr) || rr.stage != ShaderStage::Fragment) return false;
        for (const ShaderIo& o : rr.outputs)
            if (o.location == 1) {
                in = std::move(w);
                r_in = std::move(rr);
                chosen = p;
                return true;
            }
        return false;
    };
    const size_t first = std::min<size_t>(400, files.size());
    for (size_t i = 0; i < first && chosen.empty(); i++) try_file(files[i]);
    for (size_t i = first; i < files.size() && chosen.empty(); i++) try_file(files[i]);
    if (chosen.empty()) {
        nxtest::skip("no fragment shader with a Location 1 output in the shader cache");
        return;
    }
    std::printf("    fragment shader %s: outputs at locations", chosen.filename().string().c_str());
    for (const ShaderIo& o : r_in.outputs) std::printf(" %u", o.location);
    std::printf("\n");

    // The variable that carries Location 1 in the input.
    bool have_t1 = false;
    uint32_t t1 = 0;
    for (const Deco& d : decorations(in))
        if (d.kind == kDecorationLocation && d.value == 1) {
            t1 = d.target;
            have_t1 = true;
        }
    CHECK_MSG(have_t1, "input has no OpDecorate Location 1 (4-word form)");

    std::vector<uint32_t> out;
    REQUIRE(gpu::spirv_dual_source_output(in, out));

    std::string log;
    const int rc = run_spirv_val(out, "spirv_patch_ds", log);
    CHECK_MSG(rc == 0, "spirv-val exit " + std::to_string(rc) + ": " + log);

    ShaderReflection r;
    std::string err;
    CHECK_MSG(reflect(out, r, err), err);
    CHECK(!r.outputs.empty());
    for (const ShaderIo& o : r.outputs)
        CHECK_MSG(o.location == 0, "output at Location " + std::to_string(o.location) + " after the patch");

    bool loc0 = false, index1 = false, loc1 = false;
    for (const Deco& d : decorations(out)) {
        if (d.kind == kDecorationLocation && d.value == 1) loc1 = true;
        if (d.target == t1 && d.kind == kDecorationLocation && d.value == 0) loc0 = true;
        if (d.target == t1 && d.kind == kDecorationIndex && d.value == 1) index1 = true;
    }
    CHECK_MSG(!loc1, "a Location 1 decoration is left");
    CHECK_MSG(loc0, "the variable has no Location 0 decoration");
    CHECK_MSG(index1, "the variable has no OpDecorate Index 1");
}

NXTEST(dual_source_without_second_output) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    std::vector<uint32_t> vs, fs_frag;
    REQUIRE(load_hash(kVertexHash, vs));
    REQUIRE(load_hash(kConstFragHash, fs_frag));

    // Precondition: the fragment shader really has no Location 1 output.
    ShaderReflection rf;
    std::string err;
    CHECK_MSG(reflect(fs_frag, rf, err), err);
    CHECK(rf.stage == ShaderStage::Fragment);
    CHECK_MSG(locations(rf.outputs).count(1) == 0, "fragment shader 062cfa5657f5388d has a Location 1 output");

    const std::vector<uint32_t> keep = {0xdeadbeefu, 7u};
    for (const std::vector<uint32_t>* m : {&vs, &fs_frag}) {
        std::vector<uint32_t> out = keep;
        CHECK(!gpu::spirv_dual_source_output(*m, out));
        CHECK(out == keep);
    }
}

NXTEST(xfb_variant_of_real_stream_out_shader) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    // the stream-out vertex shader of the game's shadow decals: one Location 0 output of which xyz are written
    std::vector<uint32_t> in;
    REQUIRE(load_hash("51242c6ad2a045f2", in));
    const std::vector<uint32_t> before = in;
    std::vector<uint32_t> out;
    std::vector<gpu::SpvXfbOutput> lay;
    uint32_t stride = 0;
    REQUIRE(gpu::spirv_add_xfb(in, out, &lay, &stride));
    CHECK(in == before);
    CHECK_EQ(stride, 12u);
    REQUIRE(lay.size() == 1);
    CHECK_EQ(lay[0].location, 0u);
    CHECK_EQ(lay[0].components, 3u);
    CHECK_EQ(lay[0].offset, 0u);

    std::string log;
    const int rc = run_spirv_val(out, "spirv_patch_xfb", log);
    CHECK_MSG(rc == 0, "spirv-val exit " + std::to_string(rc) + ": " + log);

    // the decorations the pipeline needs: XfbBuffer 0, XfbStride 12 and Offset 0 on the output variable
    bool buffer = false, stride_deco = false, offset = false;
    for (const Deco& dc : decorations(out)) {
        buffer |= dc.kind == 36 && dc.value == 0;
        stride_deco |= dc.kind == 37 && dc.value == 12;
        offset |= dc.kind == 35 && dc.value == 0;
    }
    CHECK(buffer);
    CHECK(stride_deco);
    CHECK(offset);
}

NXTEST(xfb_variant_without_outputs_is_refused) {
    // a module with no Location outputs: the vertex shader of an old test in this group has only a position builtin
    std::vector<uint32_t> out = {0xdeadbeefu};
    std::vector<uint32_t> bad = {0x07230203u, 0x10300u, 0, 1, 0};
    CHECK(!gpu::spirv_add_xfb(bad, out, nullptr, nullptr));
    CHECK_EQ(out.size(), size_t(1));
}

NXTEST(outputs_to_private_on_real_fragment_shader) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    std::vector<uint32_t> in;
    REQUIRE(load_hash(kConstFragHash, in));
    const std::vector<uint32_t> before = in;
    std::vector<uint32_t> out;
    REQUIRE(gpu::spirv_outputs_to_private(in, out));
    CHECK(in == before);
    std::string log;
    const int rc = run_spirv_val(out, "spirv_patch_priv", log);
    CHECK_MSG(rc == 0, "spirv-val exit " + std::to_string(rc) + ": " + log);
    ShaderReflection r;
    std::string err;
    CHECK_MSG(reflect(out, r, err), err);
    CHECK_MSG(r.outputs.empty(), "the module still has " + std::to_string(r.outputs.size()) + " outputs");
    // a vertex shader has built-in outputs: refused
    std::vector<uint32_t> vs, keep = {1u, 2u};
    REQUIRE(load_hash(kVertexHash, vs));
    CHECK(!gpu::spirv_outputs_to_private(vs, keep));
    CHECK(keep.size() == 2);
}

// The geometry shader 80271a63 of the environment cube blend (one cube face per layer): a console passthrough geometry shader. Its translation sets
// the position and gl_Layer only; without the passthrough rewrite the fragment shader's Location 0 (u, v, layer) was an output nobody wrote and the
// blended cube came out as garbage (the rainbow noise on the Frigate; docs/S4f.md).
NXTEST(gs_passthrough_copies_the_input_vertices) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    std::vector<uint32_t> in;
    if (!load_hash("80271a630230f99a", in)) {
        nxtest::skip("geometry shader 80271a630230f99a not in the shader cache");
        return;
    }
    auto count_op = [](const std::vector<uint32_t>& w, uint32_t op) {
        size_t n = 0;
        for (size_t i = 5; i < w.size();) {
            const uint32_t len = w[i] >> 16;
            if (!len || i + len > w.size()) break;
            n += (w[i] & 0xffff) == op;
            i += len;
        }
        return n;
    };
    const std::vector<gpu::SpvOutputSlot> slots = {{0, gpu::SpvScalar::Float}};
    std::vector<uint32_t> out;
    REQUIRE(gpu::spirv_gs_passthrough(in, slots, out));
    CHECK_EQ(count_op(out, 62), count_op(in, 62) + 3);       // OpStore: one copy per emitted vertex
    CHECK_EQ(count_op(out, 218), size_t(3));                 // OpEmitVertex untouched

    std::string log;
    const int rc = run_spirv_val(out, "spirv_patch_gs", log);
    CHECK_MSG(rc == 0, "spirv-val exit " + std::to_string(rc) + ": " + log);
    ShaderReflection r;
    std::string err;
    CHECK_MSG(reflect(out, r, err), err);
    CHECK(r.stage == ShaderStage::Geometry);
    const ShaderIo* o = find_io(r.outputs, 0);
    CHECK_MSG(o != nullptr, "no output at Location 0");

    // no Input at the Location: refused, output untouched; an integer slot: refused
    const std::vector<uint32_t> keep = {0xdeadbeefu, 7u};
    std::vector<uint32_t> out2 = keep;
    CHECK(!gpu::spirv_gs_passthrough(in, {{7, gpu::SpvScalar::Float}}, out2));
    CHECK(out2 == keep);
    CHECK(!gpu::spirv_gs_passthrough(in, {{0, gpu::SpvScalar::Uint}}, out2));
    CHECK(out2 == keep);
    // a vertex shader is no passthrough geometry shader
    std::vector<uint32_t> vs;
    REQUIRE(load_hash(kVertexHash, vs));
    CHECK(!gpu::spirv_gs_passthrough(vs, slots, out2));
    CHECK(out2 == keep);
}

// Skinned meshes: the vertex shader edfebf23 bitcasts its Location 1 input (RGBA8UI bone indices) to integers. The console hands it the raw bits;
// a scaled float format (3 becomes 3.0) made every bone index but 0 wrong, and with it the matrix palette lookup (the first person weapon
// sat out of the picture, characters were torn; docs/S4f.md). The patch makes the input an integer vector and bitcasts it back.
NXTEST(int_attributes_give_the_shader_raw_bits) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    std::vector<uint32_t> in;
    if (!load_hash("edfebf23808d2d31", in)) {
        nxtest::skip("vertex shader edfebf23808d2d31 not in the shader cache");
        return;
    }
    auto count_op = [](const std::vector<uint32_t>& w, uint32_t op) {
        size_t n = 0;
        for (size_t i = 5; i < w.size();) {
            const uint32_t len = w[i] >> 16;
            if (!len || i + len > w.size()) break;
            n += (w[i] & 0xffff) == op;
            i += len;
        }
        return n;
    };
    std::vector<uint32_t> out;
    REQUIRE(gpu::spirv_int_attributes(in, {{1, false}}, out));
    CHECK_EQ(count_op(out, 124), count_op(in, 124) + 1);     // OpBitcast: one more, the copy of the integer input
    std::string log;
    const int rc = run_spirv_val(out, "spirv_patch_intattr", log);
    CHECK_MSG(rc == 0, "spirv-val exit " + std::to_string(rc) + ": " + log);
    ShaderReflection r;
    std::string err;
    CHECK_MSG(reflect(out, r, err), err);
    const ShaderIo* io = find_io(r.inputs, 1);
    CHECK_MSG(io != nullptr, "no input at Location 1");
    if (io) CHECK_MSG(io->type == ScalarType::Uint, std::string("Location 1 is ") + type_name(io->type) + ", want Uint");
    const ShaderIo* other = find_io(r.inputs, 0);
    if (other) CHECK(other->type == ScalarType::Float);       // the others stay floats

    // signed variant, and refusals: no such input, and a module that is not a vertex shader with that input
    std::vector<uint32_t> outs;
    REQUIRE(gpu::spirv_int_attributes(in, {{1, true}}, outs));
    CHECK_MSG(run_spirv_val(outs, "spirv_patch_intattr_s", log) == 0, log);
    const std::vector<uint32_t> keep = {0xdeadbeefu, 7u};
    std::vector<uint32_t> out2 = keep;
    CHECK(!gpu::spirv_int_attributes(in, {{9, false}}, out2));
    CHECK(out2 == keep);
}

// A shader that indexes its attributes by a computed value gets one Input array of vec4 (Locations 0..15) from the translator. Its draws used to
// be skipped (sweep: ef17e15bcbcfaa67 and seven more), so the objects they draw were missing. The array becomes Private, every Location gets its own
// Input and the entry point builds the array from them.
NXTEST(int_attributes_reach_an_indexed_attribute_array) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    std::vector<uint32_t> in;
    if (!load_hash("ef17e15bcbcfaa67", in)) {
        nxtest::skip("vertex shader ef17e15bcbcfaa67 not in the shader cache");
        return;
    }
    std::vector<uint32_t> out;
    REQUIRE(gpu::spirv_int_attributes(in, {{1, false}, {5, true}}, out));
    std::string log;
    const int rc = run_spirv_val(out, "spirv_patch_intattr_array", log);
    CHECK_MSG(rc == 0, "spirv-val exit " + std::to_string(rc) + ": " + log);
    ShaderReflection r;
    std::string err;
    CHECK_MSG(reflect(out, r, err), err);
    const ShaderIo* a = find_io(r.inputs, 1);
    const ShaderIo* b = find_io(r.inputs, 5);
    const ShaderIo* c = find_io(r.inputs, 2);
    CHECK_MSG(a && a->type == ScalarType::Uint, "Location 1 is not Uint");
    CHECK_MSG(b && b->type == ScalarType::Sint, "Location 5 is not Sint");
    CHECK_MSG(c && c->type == ScalarType::Float, "Location 2 is not Float");
    // a slot outside the array is refused
    std::vector<uint32_t> keep = {0xdeadbeefu, 7u}, out2 = keep;
    CHECK(!gpu::spirv_int_attributes(in, {{40, false}}, out2));
    CHECK(out2 == keep);
}

// The integer inputs of a vertex shader can be told from its code: an input read only through bitcasts to integers. These are shaders of the
// Frigate route; the slots are the ones the game's draws fed with integer formats (S24), and a shader with no such input gives none.
NXTEST(predict_int_inputs_from_the_code) {
    if (const char* why = absent_input()) {
        nxtest::skip(why);
        return;
    }
    struct Case { const char* hash; std::vector<uint32_t> locations; };
    const Case cases[] = {{"edfebf23808d2d31", {1}}, {"703a0d4c57b5b6cd", {3}}, {"c439de15f8c11afe", {2}}};
    for (const Case& c : cases) {
        std::vector<uint32_t> in;
        if (!load_hash(c.hash, in)) { nxtest::skip("a vertex shader of the route is not in the shader cache"); return; }
        const std::vector<gpu::SpvIntSlot> got = gpu::spirv_predict_int_inputs(in);
        std::vector<uint32_t> locs;
        for (const gpu::SpvIntSlot& g : got) {
            locs.push_back(g.location);
            CHECK(!g.is_signed);
        }
        std::printf("    %s: predicted %zu slot(s)\n", c.hash, locs.size());
        CHECK_MSG(locs == c.locations, std::string("predicted slots differ for ") + c.hash);
    }
    std::vector<uint32_t> plain;
    REQUIRE(load_hash(kVertexHash, plain));
    CHECK(gpu::spirv_predict_int_inputs(plain).empty());
    CHECK(gpu::spirv_predict_int_inputs(std::vector<uint32_t>{1u, 2u}).empty());
}


// reflect_spirv's written-variable rule ([nvn] reflect_literals): a literal operand equal to a storage variable's id is not a write. A
// synthetic fragment module reads storage buffer (set 0, binding 3, id 10) and passes the value to OpExtInst instruction 10 (a literal);
// a second module also stores into it.
namespace {
std::vector<uint32_t> literal_collision_module(bool store) {
    std::vector<uint32_t> w = {0x07230203u, 0x00010300u, 0u, 32u, 0u};
    auto ins = [&](uint32_t op, std::initializer_list<uint32_t> a) {
        w.push_back(static_cast<uint32_t>((a.size() + 1) << 16) | op);
        w.insert(w.end(), a);
    };
    ins(17, {1});                                                       // OpCapability Shader
    ins(11, {1, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0u});            // OpExtInstImport %1 "GLSL.std.450"
    ins(14, {0, 1});                                                    // OpMemoryModel Logical GLSL450
    ins(15, {4, 20, 0x6e69616du, 0u});                                  // OpEntryPoint Fragment %20 "main"
    ins(16, {20, 7});                                                   // OpExecutionMode OriginUpperLeft
    ins(71, {5, 2});                                                    // OpDecorate %5 Block
    ins(72, {5, 0, 35, 0});                                             // OpMemberDecorate %5 0 Offset 0
    ins(71, {10, 34, 0});                                               // DescriptorSet 0
    ins(71, {10, 33, 3});                                               // Binding 3
    ins(19, {2});                                                       // void
    ins(33, {3, 2});                                                    // fn void()
    ins(22, {4, 32});                                                   // float
    ins(30, {5, 4});                                                    // struct { float }
    ins(32, {6, 12, 5});                                                // ptr StorageBuffer struct
    ins(21, {7, 32, 0});                                                // uint
    ins(43, {7, 8, 0});                                                 // const 0
    ins(32, {9, 12, 4});                                                // ptr StorageBuffer float
    ins(59, {6, 10, 12});                                               // %10 = OpVariable StorageBuffer
    ins(54, {2, 20, 0, 3});                                             // OpFunction
    ins(248, {21});                                                     // OpLabel
    ins(65, {9, 22, 10, 8});                                            // %22 = OpAccessChain %10 0
    ins(61, {4, 23, 22});                                               // %23 = OpLoad %22
    ins(12, {4, 24, 1, 10, 23});                                        // %24 = OpExtInst %1 10 (literal == %10) %23
    if (store) ins(62, {22, 24});                                       // OpStore %22 %24
    ins(253, {});                                                       // OpReturn
    ins(56, {});                                                        // OpFunctionEnd
    return w;
}
}  // namespace

NXTEST(written_rule_ignores_literal_operands) {
    for (const bool store : {false, true}) {
        const std::vector<uint32_t> w = literal_collision_module(store);
        ShaderReflection r;
        std::string err;
        const bool ok = reflect(w, r, err);
        CHECK_MSG(ok, err);
        REQUIRE(ok);
        const DescriptorBinding* b = r.find(0, 3);
        REQUIRE(b != nullptr);
        CHECK(b->written_lit);              // the old rule took the literal 10 for the variable %10
        CHECK_EQ(b->written, store);        // only the store writes
    }
}