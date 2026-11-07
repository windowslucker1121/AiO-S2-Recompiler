// SPDX-License-Identifier: GPL-3.0-or-later
// SPIR-V rewrites the back end applies to the translated shaders when it builds a pipeline (render thread). The offline translator
// knows one shader at a time; the console's rules are looser than Vulkan's about what the stages of a pipeline have to agree on.
//
//   spirv_add_outputs   a fragment shader reads varyings (or an indexed array of them) that the stage before it never declared. The
//                       console hands it undefined values; Vulkan wants an output in every slot (VUID-RuntimeSpirv-OpEntryPoint-08743).
//                       The producer gets unwritten outputs there.
//   spirv_dual_source_output
//                       a fragment shader of a draw with dual-source blending: the console takes the second blend source from render
//                       target 1's output; Vulkan from the output at Location 0, Index 1 (and allows a single such attachment).
//
//   spirv_add_xfb      a vertex shader that runs with transform feedback: the Location outputs are captured into buffer 0 in Location
//                       order, each narrowed to the components the shader stores (a vec4 whose xyz are written is captured as 12
//                       bytes), tightly packed. The console's layout lives in the driver's compile data; this is the layout the
//                       game's stream-out shaders use (docs/S4e.md).
//
// The idea (pattern-matching on the module's declarations, new ids from the bound) follows SuperMarioWonderRecomp's
// nvn_spirv_patch.cpp (MIT); the code is new.
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt::nvn::be::gpu {

enum class SpvScalar : uint8_t { Float, Sint, Uint };

struct SpvOutputSlot {
    uint32_t location = 0;
    SpvScalar type = SpvScalar::Float;
};

// Adds one never-written vec4 Output variable per slot (a 4-component vector of the slot's scalar type) to the entry point of `in`.
// Returns false (out untouched) when the module cannot be rewritten (malformed, no entry point, a slot that already has an output).
bool spirv_add_outputs(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out);

// Vertex inputs that carry integers. The console hands a vertex shader the raw bits of an integer vertex attribute (RGBA8UI bone
// indices, say) in the float registers the translation reads, and the translated shader declares every input as a float and
// bitcasts it where it wants an integer. A Vulkan integer format needs an integer input. For each slot the Input variable becomes a
// Private variable and a new Input variable of uvec4 (ivec4) takes its Location; the entry point copies it over with an OpBitcast, so
// the shader reads the same bits as on the console. The slot's input is a vec4 of floats, or an element of an Input array of vec4 (the
// translation of a shader that indexes its attributes by a computed value): then the array becomes Private as a whole, every Location it
// covers gets its own new Input (float, uvec4 or ivec4 by what the draw feeds) and the entry point builds the array from them. False (out
// untouched) otherwise.
struct SpvIntSlot {
    uint32_t location = 0;
    bool is_signed = false;
};
bool spirv_int_attributes(const std::vector<uint32_t>& in, const std::vector<SpvIntSlot>& slots, std::vector<uint32_t>& out);
// The vertex input locations (is_signed = false) a game would have to feed with integer attributes: the vector inputs whose every use, in any
// component, is a bitcast to a 32-bit integer (the translator reads raw integer attribute bits that way). A guess from the code alone, made
// so the patched variant can be compiled before the first draw; a draw whose real integer slots differ builds its own variant.
std::vector<SpvIntSlot> spirv_predict_int_inputs(const std::vector<uint32_t>& in);

// A geometry shader the translator made for a console "passthrough" geometry shader (the hardware copies every attribute of the input
// vertices to the output vertices; the shader code only sets the position and gl_Layer, so its translation writes no Location output at
// all). Adds the outputs for `slots` like spirv_add_outputs and makes the k-th OpEmitVertex copy the input at the same Location of vertex k.
// Needs an Input array [3] of vec4 at each slot's Location and exactly three OpEmitVertex in a module with triangle input. False (out
// untouched) otherwise; the caller then uses spirv_add_outputs.
bool spirv_gs_passthrough(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out);

// Moves the fragment output at Location 1 to Location 0, Index 1. False (out untouched) when the module has no such output.
bool spirv_dual_source_output(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

// A fragment shader of a pass without colour attachments: every Location output becomes a private variable (the decorations go, and
// the entry point's interface loses them before SPIR-V 1.4). False (out untouched) when no Location output exists or a built-in output
// (FragDepth, SampleMask) does.
bool spirv_outputs_to_private(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

struct SpvXfbOutput {
    uint32_t location, components, offset;   // offset in bytes inside a captured vertex
};
// Adds XfbBuffer / XfbStride / Offset decorations, the TransformFeedback capability and the Xfb execution mode. layout receives the
// captured outputs, stride the bytes of one captured vertex. False (out untouched): no Location outputs or an unexpected module.
bool spirv_add_xfb(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, std::vector<SpvXfbOutput>* layout, uint32_t* stride);

// spirv_ballot_per_lane (ported from featureJosh/pokopia-recomp, runtime/src/gfx/nvn/nvn_spirv_patch.cpp, GPL-3.0-or-later; approach
// from pokopia-native (MIT) windows/docs/ballot-hang.md): a compute shader's warp-aggregated atomic counter (ballot(true), the highest
// lane leads, adds the bit count for all, hands each lane its slot by a shuffle plus its rank) can stop NVIDIA GPUs for good after some
// lanes returned (device lost). Each such ballot becomes the lane's own bit: every lane leads itself and adds 1.
struct BallotRewrite {
    int ballots = 0;       // ballots rewritten
    int groups = 0;        // counters found (a group of ballots joined by what they meet)
    int groups_left = 0;   // of those, left as translated (a use the rewrite would change); then the module is left alone
};
// Each warp-aggregated counter's ballot(true) (the leader's FindUMsb, the count's BitCount added by an OpAtomicIAdd inside the leader
// test, the rank's BitCount of LtMask & ballot added to the result the shuffle helper hands out) becomes uvec4(1 << lane, 0, 0, 0),
// every ballot of a counter or none: each lane leads itself, adds its own share and gets a slot of its own (as many slots, in the same
// range, as before; only which lane takes which within a warp changes). Assumes 32-lane subgroups. Other ballots (REDUX / MATCH masks,
// first-lane broadcasts) are left alone. False (out untouched) when nothing is rewritten; stats filled either way when given.
bool spirv_ballot_per_lane(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, BallotRewrite* stats = nullptr);

}  // namespace nxrt::nvn::be::gpu
