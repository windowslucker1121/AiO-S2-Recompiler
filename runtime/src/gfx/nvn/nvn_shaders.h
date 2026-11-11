// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from SuperMarioWonderRecomp runtime/src/nvn/nvn_shaders.{h,cpp} (MIT, Copyright (c) 2026 BelmanteGu); changes: namespace, cache lookup.
//
// NVN shader stages -> the SPIR-V translated offline (SuperMarioWonderRecomp docs/nvn/TRANSLATOR.md, "Runtime side").
//
// Every Maxwell code block of a game was translated ahead of time by Ryujinx's translator into <cache dir>/<hash>.spv,
// <hash> = the first 16 hex digits of the SHA-1 of the whole code block (the control section's +0x704 bytes). The
// harness's record of each shader sits in <cache dir>/results.tsv. At nvnProgramSetShaders time the runtime hashes the
// block the game hands in, loads that file and needs to know what each descriptor of it stands for in NVN terms: which
// uniform block, which texture binding, which storage buffer. That is what this file provides.
//
// Everything above "the runtime cache" is plain C++ with no dependency on the rest of the runtime.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nvn_shader_worker.h"

namespace nxrt::nvn::be {

// ---- hashing ----------------------------------------------------------------------------------------------------

// SHA-1 (FIPS 180-4). Only a cache key here, not a security measure.
class Sha1 {
public:
    Sha1();
    void update(const void* data, size_t size);
    void finish(uint8_t digest[20]);  // the object is spent afterwards

private:
    void block(const uint8_t* p);
    uint32_t h_[5];
    uint64_t bytes_ = 0;
    uint8_t buf_[64];
    size_t used_ = 0;
};

std::string sha1_hex(const void* data, size_t size);                 // 40 lowercase hex digits
std::string shader_hash(const uint8_t* code, uint32_t size);          // the first 16 of them: the .spv file name

// ---- the NVN control section ----------------------------------------------------------------------------------------

constexpr uint32_t kShaderControlMagic = 0x98761234;

struct ShaderControl {
    bool valid = false;           // magic present
    uint32_t program_size = 0;    // +0x6f8: SPH + instructions, from program_start
    uint32_t const_size = 0;      // +0x6fc: constant data (c[1]) bytes, 0 = none
    uint32_t const_offset = 0;    // +0x700: its offset in the code block (0x100-aligned)
    uint32_t code_size = 0;       // +0x704: the whole code block, the bytes the hash covers
    uint32_t program_start = 0;   // +0x708: 0x30 graphics (SPH there), 0x100 compute
    uint32_t local_size[3] = {};  // +0x748: compute work-group size
    uint32_t shared_size = 0;     // +0x754: compute shared memory bytes (0 = none)
    bool nvn2 = false;            // platform 2 control section (version word 201 at +4, 0xd8 bytes, Ampere SASS)
    int stage = -1;               // NVN2 only: u16 +0x56 (0 vs, 1 ps, 2 gs, 3 tcs, 4 tes, 5 cs)
    // NVN2 compute only: the slot counts of the program's c[0] resource table (bytes +0xb1 uniform blocks, +0xb2 storage
    // buffers, +0xb3 textures, +0xb6 images). See apply_nvn2_compute_table.
    uint8_t c0_ubos = 0, c0_ssbos = 0, c0_textures = 0, c0_images = 0;
    // The table's size in bytes (u16 +0xba) and the push constant area it holds after the 0x10-byte header: table size minus
    // header and slots, 0 for programs without push constants (all of the bfsha/vsetb corpus; the game's post-process chain built into
    // main has 0x80, the NDVE network 0x40). Byte +0xa9 is the number of push words the program reads.
    uint16_t c0_table = 0, c0_push = 0;
    uint8_t c0_push_words = 0;
};

ShaderControl read_control(const uint8_t* control);

// ---- SPIR-V reflection ----------------------------------------------------------------------------------------------

enum class ShaderStage : uint8_t { Unknown, Vertex, Fragment, Compute, Geometry, TessControl, TessEval, Task, Mesh };   // Task/Mesh: NVN2 stages 6/7 (VK_EXT_mesh_shader)

// Values are VkDescriptorType's (checked against the Vulkan headers in the runtime build).
enum class DescriptorType : uint32_t {
    Sampler = 0,
    CombinedImageSampler = 1,
    SampledImage = 2,
    StorageImage = 3,
    UniformTexelBuffer = 4,
    StorageTexelBuffer = 5,
    UniformBuffer = 6,
    StorageBuffer = 7,
};

enum class ScalarType : uint8_t { Other, Float, Sint, Uint, Bool };

// OpTypeImage of a texture or image binding. `dim` is SPIR-V's Dim: 0 1D, 1 2D, 2 3D, 3 Cube, 5 Buffer.
struct ImageShape {
    uint8_t dim = 0;
    bool depth = false, arrayed = false, ms = false;
    uint8_t sampled = 0;   // 1 sampled, 2 storage
    uint32_t format = 0;   // SPIR-V ImageFormat (0 Unknown)
    ScalarType component = ScalarType::Other;
};

// What a binding is in NVN terms. The translator's scheme (TRANSLATOR.md, "Where NVN puts resources"):
//   c[0] driver constants, c[1] the block's constant data, c[2] texture/image handles, c[3+n] uniform block n.
enum class NvnSource : uint8_t {
    Unknown,
    SupportBuffer,    // set 0 binding 0: the translator's own buffer (struct SupportBuffer below), filled by us
    DriverConstants,  // c[0]: NVN driver constants (compute reads its buffer descriptor table there)
    ConstantData,     // c[1]: this block's constant data (ShaderInfo::constants / control const_offset)
    UniformBlock,     // c[3+n]: what the game bound with nvnCommandBufferBindUniformBuffer(stage, n)
    StorageBuffer,    // the address in c[0] descriptor `c0_offset`: nvnCommandBufferBindStorageBuffer(stage, n)
    UniformAsStorage, // compute: the c[0] descriptor of uniform block n, read through a storage buffer
    SamplerBinding,   // texture whose handle is c[2] byte 0x20 + 8n: nvnCommandBufferBindTexture(stage, n)
    ImageBinding,     // image whose handle is c[2] byte 0x120 + 8n: nvnCommandBufferBindImage(stage, n) (inferred)
    HandleInUniform,  // texture whose handle the game wrote into uniform block `index` (c[bank]), at byte word * 4
    HandleExpr,       // texture whose handle the draw computes with NvnMeaning::expr (bank kHandleExprBank, word = expression number)
};

// Handle expressions (HandleExpr; tools/shaders/harness/translator/ampere-bindless-handle-expr.patch): NVN2 materials load their texture
// handles from a storage buffer table indexed from uniform blocks, which the translator writes as a postfix program over 32-bit integers.
// Leaves: a constant, word b of c[a] (1 = the block's constant data, 3+ = uniform blocks), a storage buffer word (index popped; the buffer
// whose descriptor is at byte a of c[0]). Comparisons give 1 or 0; and/or/xor serve the logical forms too.
struct HandleExprOp {
    enum Kind : uint8_t { Const, Cbuf, Storage, Add, Sub, Mul, Shl, Shr, Sar, And, Or, Xor, Inv, Eq, Ne, Lt, Ltu, Le, Leu, Gt, Gtu, Ge, Geu, Not,
                          Min, Minu, Max, Maxu, Sel };
    Kind kind = Const;
    uint32_t a = 0, b = 0;
};
constexpr int kHandleExprBank = 0x20000;   // HandleExprSlot in the patch
// False (and out cleared) on an unknown token or a program that does not leave exactly one value.
bool parse_handle_expr(std::string_view text, std::vector<HandleExprOp>& out);

struct NvnMeaning {
    NvnSource source = NvnSource::Unknown;
    int index = -1;      // the NVN binding n (uniform block, storage buffer, sampler, image)
    int bank = -1;       // constant bank c[bank]: the buffer itself (uniforms) or the bank holding the handle
    int word = -1;       // textures/images: 32-bit word of c[bank] holding the handle
    int sb_slot = -1;    // storage buffers: the translator's slot (order found), from the name "<stage>_s<slot>"
    int c0_offset = -1;  // storage buffers: byte offset of the descriptor in c[0]; -1 = not known (see apply_harness)
    bool default_bank = false;  // texture handle bank given by the translator as "the texture bank" (-1), i.e. c[2]
    std::vector<HandleExprOp> expr;   // HandleExpr: the handle's program
    std::string expr_text;            // HandleExpr: its text as the harness printed it
};

struct DescriptorBinding {
    uint32_t set = 0, binding = 0;
    DescriptorType type = DescriptorType::UniformBuffer;
    uint32_t count = 1;  // array length; 0 = runtime array
    std::string name;    // OpName, e.g. "fp_c3", "fp_t_tcb_8", "fp_t_cb7_20", "cp_s0", "cp_i_cb2_4A_r32ui"
    ImageShape image;    // texture and image bindings
    NvnMeaning nvn;
    // Some instruction references the variable. The translator declares c[0] (driver constants) in most shaders
    // without using it; the harness's record lists only used resources (checked on the whole cache), so
    // describe_like_harness skips the unused ones.
    bool used = false;
    // Some instruction may write through it (a store, an atomic, or the pointer escapes into a call or a phi). False: the code only reads
    // it (storage buffers whose bindings then need no copy-back and leave the pool copy valid).
    bool written = true;
    bool written_lit = true;   // the rule before [nvn] reflect_literals: a literal operand equal to the variable's id counted as a write
};

struct ShaderIo {
    uint32_t location = 0, component = 0;
    ScalarType type = ScalarType::Other;
    uint32_t components = 0;  // 1..4
    uint32_t array = 1;       // array length (1 = not an array)
};

struct ShaderReflection {
    ShaderStage stage = ShaderStage::Unknown;
    std::string entry;                             // entry point name ("main")
    uint32_t local_size[3] = {0, 0, 0};            // OpExecutionMode LocalSize (compute)
    std::vector<DescriptorBinding> bindings;       // sorted by (set, binding)
    std::vector<ShaderIo> inputs, outputs;         // Location-decorated Input/Output variables (not built-ins)
    std::vector<uint32_t> builtins_in, builtins_out;  // SPIR-V BuiltIn values (on variables or block members)
    std::vector<uint32_t> builtins_used_in, builtins_used_out;  // built-in block members an access chain reaches (used, not only declared)
    bool uses_support_buffer = false;
    // The harness's "gmem" token: global memory accesses resolved at run time against the stage's c[0] storage descriptors (A17,
    // tools/shaders/ampere/frontend/AmpereGlobal.cs): graphics stages then get a c[0] with those descriptors per draw (graphics_c0).
    bool global_fallback = false;
    // The harness's "gpt" token: those accesses walk the guest page table (c[0] 0xfe0; nvn_replay.cpp pool_table_words). Without it (older
    // translations) they binary-search the sorted import table at c[0] 0xff0/0xff8.
    bool global_pt = false;
    // The harness's "gmemw" token: some store or atomic goes through the page table (nvn_replay.cpp gmem_before_op).
    bool global_writes = false;

    const DescriptorBinding* find(uint32_t set, uint32_t binding) const;
    // Vertex: the attribute locations the shader reads. Fragment: the render targets it writes.
    const std::vector<ShaderIo>& vertex_inputs() const { return inputs; }
    const std::vector<ShaderIo>& fragment_outputs() const { return outputs; }
};

// Parses a SPIR-V module (`count` 32-bit words). False with a reason in `error` if the module is malformed.
bool reflect_spirv(const uint32_t* words, size_t count, ShaderReflection& out, std::string* error = nullptr);

// ---- the harness's own record (results.tsv, column "resources") -----------------------------------------------------
// The SPIR-V carries everything except where a storage buffer's address came from: the translator names storage
// buffers by slot ("vp_s0"), not by their c[0] offset. The harness printed that offset in results.tsv, so the
// runtime reads it from there. Format (tools/maxwell/spirv_harness/Program.cs, DescribeResources):
//   cb<slot>=s<set>b<binding>                      constant buffer c[slot]
//   sb[c<slot>+0x<off>]=s<set>b<binding>           storage buffer, address at c[slot] 32-bit word <off>
//   tex<type>[c<slot>:<word>(x<n>)]=s<set>b<binding>  texture, handle at word <word> of c[slot] (-1 = c[2])
//   img<type>[c<slot>:<word>(x<n>)]=s<set>b<binding>  image, likewise

struct HarnessResource {
    enum Kind : uint8_t { Uniform, Storage, Texture, Image, GlobalFallback, GlobalPageTable, GlobalWrites } kind = Uniform;   // GlobalFallback: the "gmem" token
    int set = -1, binding = -1;
    int bank = -1;           // c[bank] as printed (-1 = the default texture bank)
    int word = -1;           // storage: descriptor word in c[bank]; texture/image: handle word
    int array_length = 1;
    std::string type;        // Ryujinx SamplerType text, e.g. "Texture2D|Array|Shadow"
    std::string expr;        // texture with bank kHandleExprBank: the handle expression after '|' ("tex<type>[c131072:<n>|<rpn>]")
};

bool parse_harness_resources(std::string_view text, std::vector<HarnessResource>& out);
// results.tsv text -> hash -> resources column (every line; the cache indexes lines itself instead).
size_t parse_results_tsv(std::string_view text, std::unordered_map<std::string, std::string>& out);
// Fills the c[0] offset of each storage buffer (and anything the names did not give). False if a harness entry has
// no matching SPIR-V binding.
bool apply_harness(ShaderReflection& refl, const std::vector<HarnessResource>& resources);

// The "resources" text the harness would print for this reflection (after apply_harness); the test compares it.
std::string describe_like_harness(const ShaderReflection& refl);
std::string sampler_type_text(const ImageShape& s);  // Ryujinx SamplerType text of an image shape

// ---- Ryujinx's support buffer ---------------------------------------------------------------------------------------
// Set 0, binding 0 of every stage (std140), declared by every translated shader. Layout copied from Ryujinx
// (Ryujinx.Graphics.Shader/SupportBuffer.cs; filling rules from Ryujinx.Graphics.Gpu/Memory/SupportBufferUpdater.cs),
// MIT License, Copyright (c) Ryujinx Team and Contributors. Every field is a 16-byte vec4 slot.
//
// With the answers the harness baked (TRANSLATOR.md, "What only the runtime knows") the shaders read only
// render_scale (texture size queries, texel fetches, gl_FragCoord.xy) and frag_scale_count (vertex shaders index their
// scales past the fragment ones): with every scale 1 those reads are identities. viewport_* are read only by variants
// with viewport transform disabled or Y negate, is_bgra only when the host has no BGRA formats (Vulkan has), alpha_test
// never (the alpha test is baked into the shader from the graphics state).
struct SupportBuffer {
    int32_t alpha_test[4];        // 0x000 FragmentAlphaTest (declared, not read)
    int32_t is_bgra[8][4];        // 0x010 FragmentIsBgra: x = 1 if render target i is BGRA (R/B swapped on output)
    float viewport_inverse[4];    // 0x090 2*scale/width, 2*scale/height, 1, 1 if viewport transform disabled else 0
    float viewport_size[4];       // 0x0a0 width, |height|, 1, 0 (gl_FragCoord.y flip under Y negate)
    int32_t frag_scale_count[4];  // 0x0b0 x = number of fragment texture scales (vertex scales follow them)
    float render_scale[73][4];    // 0x0c0 x only: [0] render target, [1..64] textures, [65..72] images
    int32_t tfe_offset[4];        // 0x550 transform feedback emulation (unused: no transform feedback)
    int32_t tfe_vertex_count[4];  // 0x560
};
static_assert(sizeof(SupportBuffer) == 0x570, "Ryujinx SupportBuffer is 1392 bytes");
static_assert(offsetof(SupportBuffer, is_bgra) == 0x10 && offsetof(SupportBuffer, viewport_inverse) == 0x90 &&
                  offsetof(SupportBuffer, viewport_size) == 0xa0 && offsetof(SupportBuffer, frag_scale_count) == 0xb0 &&
                  offsetof(SupportBuffer, render_scale) == 0xc0 && offsetof(SupportBuffer, tfe_offset) == 0x550 &&
                  offsetof(SupportBuffer, tfe_vertex_count) == 0x560,
              "offsets as in the SPIR-V's OpMemberDecorate");

SupportBuffer support_buffer_defaults();  // render scale 1 everywhere, no BGRA, no alpha test, zero viewport
// What Ryujinx writes for viewport 0 (SetViewportTransformDisable / SetViewportSize), with render scale 1.
void support_buffer_set_viewport(SupportBuffer& sb, float width, float height, bool transform_disabled = false);

// ---- one stage ------------------------------------------------------------------------------------------------------

struct ShaderInfo {
    std::string hash;
    ShaderStage stage = ShaderStage::Unknown;  // from the SPIR-V execution model
    std::vector<uint32_t> spirv;
    ShaderReflection refl;
    ShaderControl control;
    // c[1]: the constant data at code block + control.const_offset. Copied because the game may reuse the pool. The
    // SPIR-V declares every c[k] as vec4[4096]; bind at least const_size rounded up to 16.
    std::vector<uint8_t> constants;
    uint32_t local_size[3] = {1, 1, 1};  // compute: from control +0x748 (equals the SPIR-V's LocalSize)
    // Not computed by this port (the original fills them from nvn_spirv_patch.cpp, which is not ported). masks_known
    // stays false and the replay must compute them on use.
    bool masks_known = false;
    uint64_t dynamic_uniforms = 0, written_storage = 0;
    // A generated stage that reads push constants (the pass-through tessellation control shader: its levels), bytes from offset 0; the
    // program's pipeline layout then has that range for this stage. 0 = none (every translated shader).
    uint32_t push_bytes = 0;
};

// Builds a ShaderInfo from a code block, its control section and the SPIR-V words (no I/O). `resources` = the
// block's results.tsv resources column, or null (storage buffer c[0] offsets then stay -1).
// `pre_refl`: the reflection of these words with the resources record already applied (the boot preload made it); then neither
// is done again and `resources` is not read.
// 1 when the graphics stages' c[0] storage buffer descriptors hold the slots in pairs swapped ([nvn] graphics_ssbo_slot_xor, the game), else 0.
int graphics_ssbo_slot_xor();

bool make_shader_info(const uint8_t* code, const uint8_t* control, std::vector<uint32_t> spirv,
                      const std::string* resources, ShaderInfo& out, std::string* error = nullptr,
                      const ShaderReflection* pre_refl = nullptr);

const char* stage_name(ShaderStage s);
const char* source_name(NvnSource s);

// ---- the runtime cache ----------------------------------------------------------------------------------------------

// A directory holding <hash>.spv files and results.tsv. Lookups are keyed by the hash of the code block.
class ShaderCache {
public:
    ShaderCache() = default;
    ~ShaderCache();
    ShaderCache(const ShaderCache&) = delete;
    ShaderCache& operator=(const ShaderCache&) = delete;

    // Opens `dir`: reads results.tsv once and indexes its lines by the hash in column 1. False (logged) if results.tsv
    // cannot be read or has no "resources" column. A cache opens once: a second open() returns false.
    bool open(const std::string& dir);
    // The stage whose code block is at `code` (guest = host memory) with control section `control`. Hashes the block,
    // loads <hash>.spv on first use and keeps the ShaderInfo for the life of the process (the pointer stays valid).
    // Null if there is no usable .spv (logged once per hash) or the control section is malformed.
    // Hashing costs a pass over the block: call it when the program is set, not per draw.
    // On-demand translation (start_translator): a block the cache has no record of goes to the translator and find() returns null with
    // miss->pending set (and miss->hash); call find() again once translator_state(hash) is no longer Pending. A block the translator
    // could not translate is then an ordinary miss.
    struct FindMiss {
        bool pending = false;
        std::string hash;
    };
    const ShaderInfo* find(const uint8_t* code, const uint8_t* control, FindMiss* miss = nullptr);
    // A ShaderInfo that find() already loaded, or null. Never loads: a ShaderInfo needs the code block (its control
    // section and constant data come from it).
    const ShaderInfo* find_by_hash(const std::string& hash);

    // A ShaderInfo for pipeline building only (S24, the shader preparation): SPIR-V, stage and reflection from <hash>.spv and its results.tsv line,
    // no code block, no control section, no constant data. Kept apart from find(): the game's own ShaderInfos never come from here. The
    // pointer stays valid for the life of the cache; only the SPIR-V words can be dropped (release_stub_spirv) and are read again by
    // load_stub. Null if the hash has no usable .spv. Any thread.
    const ShaderInfo* load_stub(const std::string& hash);
    void release_stub_spirv(const ShaderInfo* s);   // frees the words; the caller guarantees nobody is using them (single threaded point)
    // On-demand translation: a persistent spirv_harness --serve process that writes into this cache. Config [nvn] shader_translator
    // (default <exe folder>/tools/spirv_harness/spirv_harness.exe; "off" disables), [nvn] shader_corpus (default <cache>/../corpus),
    // [nvn] shader_translate_jobs. False (logged) when there is no harness: misses then stay misses, as before.
    bool start_translator();
    bool translator_on() const { return worker_ && worker_->running(); }
    uint64_t translator_generation() const { return worker_ ? worker_->generation() : 0; }
    ShaderTranslatorClient::State translator_state(const std::string& hash) const {
        return worker_ ? worker_->state(hash) : ShaderTranslatorClient::State::Unknown;
    }
    size_t translator_pending() const { return worker_ ? worker_->pending() : 0; }
    uint64_t translator_done() const { return worker_ ? worker_->done() : 0; }
    // Shader setup (nvn_setup.cpp): the corpus the translator reads (empty: none), whether results.tsv has a row for a block, and a
    // corpus block queued for translation by its hash.
    const std::string& corpus_dir() const { return corpus_; }
    bool has_record(const std::string& hash) const;
    bool usable(const std::string& hash) const;   // a record whose status is ok (or ok-retry)
    // What a block costs the driver, from its results.tsv row: the SPIR-V size (spv_bytes column) and the "gmem" resource token (the
    // global-memory fallback, whose pipelines take the driver far longer). False when there is no row.
    bool record_cost(const std::string& hash, uint64_t* spv_bytes, bool* gmem) const;
    bool translate_corpus_block(const std::string& hash) { return worker_ && worker_->submit_corpus(hash); }
    size_t loaded() const;   // ShaderInfos built so far
    size_t missing() const;  // hashes asked for that have no usable .spv (or do not reflect)
    bool is_open() const;
    const std::string& dir() const;

private:
    // No lock held while it runs: takes the preloaded words (waiting for a worker that is reading this very file) or reads the .spv
    // itself, builds the ShaderInfo, and only then inserts it under lock_ (if another thread inserted the hash first, that one wins).
    // `record`: the resources column of this block's results.tsv line, or null.
    const ShaderInfo* load(const std::string& hash, const uint8_t* code, const uint8_t* control, uint32_t code_size,
                           const std::string* record);
    // Boot preload ([nvn] shader_order, default shader_order.txt; [nvn] shader_preload_threads, default 6; [nvn] shader_preload_mb, default 1024
    // = how much prepared-but-untaken data the workers may hold): the hashes find() loaded,
    // in the order it first loaded them, are kept in the order file; the next run reads and reflects those .spv files on worker
    // threads from the moment the cache opens, ahead of the game's own program loads, which then only take the prepared words.
    struct Preloaded {
        std::vector<uint32_t> words;
        ShaderReflection refl;   // the resources record already applied
    };
    void start_preload();
    void preload_worker();
    std::unique_ptr<Preloaded> preload_one(const std::string& hash);
    void note_order_locked(const std::string& hash, std::string& flush);   // flush: lines to append, written by the caller after unlocking
    void flush_order(const std::string& lines);
    std::unique_ptr<Preloaded> take_preloaded(const std::string& hash);

    std::mutex pre_mu_;
    std::condition_variable pre_cv_;                                        // a worker finished a read / an entry was taken / stop
    std::unordered_set<std::string> pre_inflight_;                          // hashes a worker is reading right now (pre_mu_)
    size_t pre_bytes_ = 0;                                                  // words held in pre_ (pre_mu_)
    size_t pre_budget_ = 0;                                                 // workers pause above this
    std::mutex order_io_mu_;                                                // serialises appends to the order file
    std::unordered_map<std::string, std::unique_ptr<Preloaded>> pre_;     // prepared, not yet taken
    std::vector<std::string> pre_list_;                                     // the order file of the last run
    size_t pre_next_ = 0;                                                   // next pre_list_ entry for a worker (pre_mu_)
    bool pre_stop_ = false;
    std::vector<std::thread> pre_threads_;
    std::string order_path_;
    std::unordered_set<std::string> order_known_;                           // in the order file (lock_)
    std::string order_pending_;                                             // new lines not yet appended (lock_)

    void add_record(const std::string& hash, const std::string& row);   // a row the translator appended to results.tsv
    std::unique_ptr<ShaderTranslatorClient> worker_;
    std::string corpus_;                                                    // the translator's corpus (start_translator)

    mutable std::mutex lock_;
    std::string dir_;
    bool open_ = false;
    std::string results_;                                                   // results.tsv, read once
    int resources_col_ = -1;                                                // column index of "resources"
    int status_col_ = -1;                                                   // column index of "status"
    int spv_col_ = -1;                                                      // column index of "spv_bytes" (-1: none)
    std::unordered_map<std::string, std::pair<size_t, size_t>> lines_;     // hash -> (offset, length) in results_
    std::unordered_map<std::string, std::unique_ptr<ShaderInfo>> by_hash_; // null: no usable .spv (logged once)
    std::unordered_set<uintptr_t> bad_controls_;                           // malformed control pointers (logged once)
    std::unordered_map<std::string, std::unique_ptr<ShaderInfo>> stubs_;   // load_stub (see there); null: no usable .spv
    size_t loaded_ = 0, missing_ = 0;
};

// The process-wide cache. Opens itself on first use from config [nvn] shader_cache (environment NXRT_NVN_SHADER_CACHE);
// if that is empty, the first of ./shader_cache/spirv and <executable folder>/shader_cache/spirv that has a
// results.tsv. A failed open is logged once and every find returns null.
ShaderCache& shader_cache();

// ---- program lists (MPRPRG01, tools/shaders/scripts/bnsh_programs.py): what the shader preparation compiles ----------------------------
struct ProgramHashes { uint64_t vs, fs, gs; };   // gs 0 = none
bool read_programs_file(const std::string& path, std::vector<ProgramHashes>* out, std::string* error);
// Programs whose stages all have a usable translation in shader_cache() (the ones a preparation compiles). A preparation that finished is
// still good while this is not above its "prepared" + "failed" counts (prepare.state): translations added since (the on-demand
// translator, a re-run of the batch) only matter when they complete programs.
uint64_t usable_programs(const std::vector<ProgramHashes>& progs);

// ---- the shader preparation's state file (prepare.state next to programs.bin; written by gpl_prepare) --------------------------------------
// Version of what a preparation builds. 2: pre-raster parts for every raster flag set of [nvn] prepare_rasters (1 built only the console's
// default, depth clamp on, while the game draws 98% of its pipelines with depth clamp off), and the state names the pipeline cache file it wrote.
constexpr int kPrepareScheme = 2;
// [nvn] prepare_rasters: the raster flags (polygon mode . depth clamp . negative-one-to-one depth) every program's pre-raster part is prepared
// with, as "p.c.n" groups. Normalised text, the same in the state file.
std::string prepare_rasters_setting();
// Finished by this scheme with the current raster flags, and the pipeline cache file it wrote is still there with at least half of the bytes it
// had then (a later in-game save only adds to it). No device needed: the GPU/driver key is checked where a device exists. *why says what is off.
bool prepare_state_complete(const std::string& state_path, std::string* why);

}  // namespace nxrt::nvn::be
