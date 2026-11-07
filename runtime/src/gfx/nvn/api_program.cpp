// SPDX-License-Identifier: GPL-3.0-or-later
// NVNprogram: the host Program object (nvn_core.h) behind the 0xC0-byte guest object, and the entry points that set its
// stages. SetShaders takes each NVNshaderData{data, control}: data is the GPU address of the code block (a host pointer into
// the game's pool), control the control section. The shader cache (nvn_shaders.h) gives the ShaderInfo of the block; its
// NVN stage comes from the SPIR-V execution model. A block the cache cannot translate leaves the program unlinked.
//
// Signatures: the five entries in work/nvn/sigs.txt use those types. nvnProgramSetSampleShading has no signature in sigs.txt
// (table entry '?'): assumed void(NVNprogram*, int), the argument order smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0) uses
// (layout/defaults checked, no code copied). nvnProgramSetShadersExt(NVNprogram*, int count, entries): 0x18-byte entries whose first
// two words are NVNshaderData's {data, control}; the third word is not used here. Stride from ACNH 3.0.3 main+0x24d9a8 (the caller
// fills the array with `mov w24, #0x18`), as ACNHRecomp (MIT) found. nvnProgramSetSubroutineLinkage is accepted and ignored: the back end has no subroutine support.
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <cstring>
#include <string>

#include "nvn_core.h"
#include "nvn_gpu.h"
#include "nvn_impl.h"
#include "nvn_shaders.h"
#include "nxrt/gaps.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
using namespace be;
namespace {

// Live programs (nxrt/gauge.h): from Initialize to Finalize, or to an Initialize over the program.
nxrt::GaugeCounter g_programs("nvn.programs");

static_assert(sizeof(GuestHeader) <= sizeof(NVNprogram), "program header fits the NVN object");

// Blocks larger than this are not hashed for a log key (a corrupt control section must not make us read far).
constexpr uint32_t kMaxLogHashBytes = 16u << 20;

// NVN stage index of a translated stage (NVNshaderStage values); -1 for a stage the back end does not know.
int nvn_stage_of(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex: return 0;
        case ShaderStage::Fragment: return 1;
        case ShaderStage::Geometry: return 2;
        case ShaderStage::TessControl: return 3;
        case ShaderStage::TessEval: return 4;
        case ShaderStage::Compute: return 5;
        default: return -1;
    }
}

// The 16 hex digits of a shader hash as a number. 0 if the text is not hex.
uint64_t parse_hash(const std::string& hex) {
    uint64_t v = 0;
    for (char ch : hex) {
        uint64_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint64_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint64_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint64_t>(ch - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    return v;
}

// True the first time a key is seen: the log line of a missing shader is written once per hash.
bool first_report(const std::string& key) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lk(mu);
    return seen.insert(key).second;
}

// The key of a block the cache could not load: its hash when the control section is sane, else the control address.
std::string missing_key(const uint8_t* code, const uint8_t* control) {
    const ShaderControl c = read_control(control);
    if (c.valid && c.code_size != 0 && c.code_size <= kMaxLogHashBytes) return shader_hash(code, c.code_size);
    char buf[40];
    std::snprintf(buf, sizeof buf, "control@%p", static_cast<const void*>(control));
    return buf;
}

void report_missing(const uint8_t* code, const uint8_t* control) {
    if (!code || !control) {
        if (first_report("null")) NXLOG_W("nvn", "nvnProgramSetShaders: a stage has no code or control address; program not linked");
        return;
    }
    const std::string key = missing_key(code, control);
    nxrt::gap("shader", "program set with an untranslated block", key);
    if (first_report(key)) NXLOG_W("nvn", "nvnProgramSetShaders: no translated shader for block %s; program not linked", key.c_str());
}

NVNboolean nvnProgramInitialize(NVNprogram* program, NVNdevice* device) {
    (void)device;   // the program needs no device state of its own
    if (!program) return 0;
    const bool was_live = host_of<Program>(program) != nullptr;   // initialised over a live program: replaced, not added
    bind_guest(program, new Program());
    if (!was_live) g_programs.add(1);
    return 1;
}

void nvnProgramFinalize(NVNprogram* program) {
    if (!host_of<Program>(program)) return;
    g_programs.sub(1);
    unbind_guest(program);
}

void nvnProgramSetDebugLabel(NVNprogram* program, const char* label) {
    if (Program* p = host_of<Program>(program)) p->label = label ? label : "";
}

NVNboolean nvnProgramSetShaders(NVNprogram* program, int count, const NVNshaderData* shaders) {
    Program* p = host_of<Program>(program);
    if (!p) return 0;
    std::lock_guard pending_lk(p->pending_mu);   // the replay thread's program_resolve_pending
    for (int s = 0; s < 6; ++s) {
        p->stage[s] = nullptr;
        p->code_hash[s] = 0;
        p->code_addr[s] = 0;
        p->control_addr[s] = 0;
    }
    p->pending.clear();
    p->has_pending.store(false, std::memory_order_release);
    p->missing_hard = false;
    p->missing[0] = 0;
    ++p->serial;
    p->linked = false;
    p->gpl_bound.store(0, std::memory_order_relaxed);
    if (count > 0 && !shaders) return 0;

    bool all_found = true;
    bool addresses_ok = true;     // every stage had a code and a control address (the block is just not in the cache)
    for (int k = 0; k < count; ++k) {
        const uint8_t* code = host_ptr(shaders[k].data);
        const uint8_t* control = static_cast<const uint8_t*>(shaders[k].control);
        ShaderCache::FindMiss miss;
        const ShaderInfo* si = (code && control) ? shader_cache().find(code, control, &miss) : nullptr;
        if (!si && miss.pending) {   // being translated on demand: the program's draws wait for it (program_resolve_pending)
            p->pending.push_back(Program::PendingBlock{code, control, miss.hash});
            all_found = false;
            continue;
        }
        if (!si) {
            p->missing_hard = true;
            report_missing(code, control);
            if (code && control) {   // remembered for the trace of the program's binds and dispatches
                const std::string key = missing_key(code, control);
                const size_t used = std::strlen(p->missing);
                std::snprintf(p->missing + used, sizeof p->missing - used, "%s%s", used ? "," : "", key.c_str());
            }
            all_found = false;
            if (!code || !control) {
                addresses_ok = false;
                if (first_report("null:" + std::to_string(k) + ":" + std::to_string(count)))
                    NXLOG_W("nvn", "nvnProgramSetShaders: stage %d of %d has code %llx control %p (null): SetShaders fails", k, count,
                            static_cast<unsigned long long>(shaders[k].data), shaders[k].control);
            }
            continue;
        }
        const int stage = nvn_stage_of(si->stage);
        if (stage < 0) {
            if (first_report("stage:" + si->hash))
                NXLOG_W("nvn", "nvnProgramSetShaders: block %s is a stage the back end does not run; program not linked", si->hash.c_str());
            p->missing_hard = true;
            all_found = false;
            continue;
        }
        p->stage[stage] = si;
        p->code_hash[stage] = parse_hash(si->hash);
        p->code_addr[stage] = reinterpret_cast<uint64_t>(code);
        p->control_addr[stage] = reinterpret_cast<uint64_t>(control);
    }
    p->has_pending.store(!p->pending.empty(), std::memory_order_release);
    p->linked = all_found;
    if (all_found && gpu::active()) gpu::prewarm_program_shaders(p->stage);
    // A block missing from the shader cache leaves the program unlinked (its draws are skipped). The call still reports success: the game treats a
    // failed SetShaders as "shader not built" and rebuilds it (programs, buffer allocations out of its pool allocator) on every bind, which costs
    // milliseconds per attempt under the allocator's lock and grows without bound (S18). [nvn] accept_unlinked = 0 restores the old return value.
    static const bool accept_unlinked = config_get_bool("nvn", "accept_unlinked", true);
    return (all_found || (accept_unlinked && addresses_ok)) ? 1 : 0;
}

}  // namespace

bool be::program_resolve_pending(Program* p) {
    if (!p || !p->has_pending.load(std::memory_order_acquire)) return false;
    std::unique_lock lk(p->pending_mu, std::try_to_lock);
    if (!lk.owns_lock()) return true;   // SetShaders is replacing the program right now
    using St = ShaderTranslatorClient::State;
    ShaderCache& cache = shader_cache();
    std::vector<Program::PendingBlock> keep;
    bool changed = false;
    for (Program::PendingBlock& b : p->pending) {
        if (cache.translator_state(b.hash) == St::Pending) {   // cheap: no pass over the block until the translator answered
            keep.push_back(std::move(b));
            continue;
        }
        ShaderCache::FindMiss miss;
        const ShaderInfo* si = cache.find(b.code, b.control, &miss);
        if (!si) {
            if (miss.pending) {
                keep.push_back(std::move(b));
            } else {
                p->missing_hard = true;   // the translator could not do it: missing, as without on-demand translation
                report_missing(b.code, b.control);
            }
            continue;
        }
        const int stage = nvn_stage_of(si->stage);
        if (stage < 0) {
            p->missing_hard = true;
            continue;
        }
        p->stage[stage] = si;
        p->code_hash[stage] = parse_hash(si->hash);
        p->code_addr[stage] = reinterpret_cast<uint64_t>(b.code);
        p->control_addr[stage] = reinterpret_cast<uint64_t>(b.control);
        changed = true;
    }
    p->pending = std::move(keep);
    if (changed) ++p->serial;
    if (!p->pending.empty()) return true;
    p->has_pending.store(false, std::memory_order_release);
    p->linked = !p->missing_hard;
    p->gpl_bound.store(0, std::memory_order_relaxed);   // the next bind queues the new stages' pipeline parts
    return false;
}

namespace {

// The Ext entry: NVNshaderData plus one more word (see the header comment).
struct ShaderDataExt {
    NVNshaderData base;
    uint64_t extra;
};
static_assert(sizeof(ShaderDataExt) == 0x18, "nvnProgramSetShadersExt entries are 0x18 bytes");

NVNboolean nvnProgramSetShadersExt(NVNprogram* program, int count, const ShaderDataExt* shaders) {
    if (count > 0 && !shaders) return nvnProgramSetShaders(program, count, nullptr);
    NVNshaderData plain[6]{};
    if (count < 0 || count > 6) {
        NXLOG_W("nvn", "nvnProgramSetShadersExt: %d stages (at most 6)", count);
        return 0;
    }
    for (int k = 0; k < count; ++k) plain[k] = shaders[k].base;
    return nvnProgramSetShaders(program, count, plain);
}

NVNboolean nvnProgramSetSubroutineLinkage(NVNprogram* program, int stage, const NVNsubroutineLinkageMapPtr* map) {
    (void)stage;
    (void)map;
    if (!host_of<Program>(program)) return 0;
    if (first_report("subroutine-linkage")) NXLOG_W("nvn", "nvnProgramSetSubroutineLinkage: subroutines are not supported; the linkage is ignored");
    return 1;
}

// Assumed signature (see the header comment). The back end keeps no sample-shading state yet, so this only validates.
void nvnProgramSetSampleShading(NVNprogram* program, int mode) {
    // sdk 0x5E5DB0: the mode is a 4-bit field (bits 28..31 of the program word at +0x80). Kept on the program; pipelines do not use it.
    if (Program* p = host_of<Program>(program)) p->sample_shading = static_cast<uint32_t>(mode) & 0xFu;
}

// NVN2 only: nvnProgramInitializeFromBuilder(program, builder) (sdk 0x5E6CB0). Initialize + the builder's shaders (count at +0x28, NVNshaderData
// array at +0x30) + its sample shading (+0x18). The ray-tracing modules and recursion depths in the builder are not used by this back end.
NVNboolean nvnProgramInitializeFromBuilder(NVNprogram* program, const NVNprogramBuilder* builder) {
    if (!program || !builder) return 0;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(builder);
    int32_t count;
    uint64_t shaders;
    uint32_t sample;
    std::memcpy(&sample, b + 0x18, 4);
    std::memcpy(&count, b + 0x28, 4);
    std::memcpy(&shaders, b + 0x30, 8);
    if (!nvnProgramInitialize(program, nullptr)) return 0;
    nvnProgramSetSampleShading(program, static_cast<int>(sample));
    if (count <= 0) return 1;
    const NVNboolean ok = nvnProgramSetShaders(program, count, reinterpret_cast<const NVNshaderData*>(static_cast<uintptr_t>(shaders)));
    if (!ok && first_report("from_builder"))
        NXLOG_W("nvn", "nvnProgramInitializeFromBuilder: %d shaders at %llx: SetShaders failed", count, static_cast<unsigned long long>(shaders));
    return ok;
}

// NVN 55 name the game asks for and never calls (signature assumed): the sub-tile size is not modelled; it reports 0 x 0.
int nvnProgramGetSubtileSize(const NVNprogram*, int* width, int* height) {
    if (width) *width = 0;
    if (height) *height = 0;
    return 0;
}

}  // namespace

void register_api_program() {
    NVN_REG(nvnProgramGetSubtileSize);
    NVN_REG(nvnProgramInitialize);
    NVN_REG(nvnProgramFinalize);
    NVN_REG(nvnProgramSetDebugLabel);
    NVN_REG(nvnProgramSetShaders);
    NVN_REG(nvnProgramSetShadersExt);
    NVN_REG(nvnProgramSetSubroutineLinkage);
    NVN_REG(nvnProgramSetSampleShading);
    NVN2_REG(nvnProgramInitializeFromBuilder);
}

}  // namespace nxrt::nvn
