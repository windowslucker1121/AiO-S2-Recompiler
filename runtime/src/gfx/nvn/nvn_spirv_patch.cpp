// SPDX-License-Identifier: GPL-3.0-or-later
// SPIR-V rewrites (declarations in nvn_spirv_patch.h).
#include "nvn_spirv_patch.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace nxrt::nvn::be::gpu {
namespace {

enum : uint32_t {
    OpEntryPoint = 15, OpTypeVoid = 19, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpTypeForwardPointer = 39,
    OpFunction = 54, OpVariable = 59, OpDecorate = 71,
    DecorationLocation = 30, StorageOutput = 3,
};

struct Inst {
    size_t at;
    uint32_t op, len;
};

bool is_type_or_constant(uint32_t op) { return (op >= OpTypeVoid && op <= OpTypeForwardPointer) || (op >= 41 && op <= 52); }

}  // namespace

bool spirv_add_outputs(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out) {
    if (slots.empty() || in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }

    uint32_t bound = in[3];
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ints;    // (width, signed) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecs;    // (component type, count) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ptrs;    // (storage class, pointee) -> id
    std::map<uint32_t, uint32_t> output_location;              // variable id -> Location
    std::map<uint32_t, uint32_t> output_vars;                  // output variable id -> pointee type id
    uint32_t f32 = 0;
    size_t first_type = 0, first_function = 0, entry_at = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!first_type && is_type_or_constant(n.op)) first_type = n.at;
        if (!first_function && n.op == OpFunction) first_function = n.at;
        if (n.op == OpEntryPoint && !entry_at) entry_at = n.at;
        if (n.op == OpTypeInt && n.len == 4) ints[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypeFloat && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4) vecs[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypePointer && n.len == 4) ptrs[{w[2], w[3]}] = w[1];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) output_location[w[1]] = w[3];
    }
    if (!first_type || !first_function || !entry_at || !f32) return false;
    for (const Inst& n : insts)
        if (n.op == OpVariable && n.len >= 4 && in[n.at + 3] == StorageOutput) output_vars[in[n.at + 2]] = in[n.at + 1];
    for (const SpvOutputSlot& s : slots)
        for (const auto& kv : output_location)
            if (output_vars.count(kv.first) && kv.second == s.location) return false;   // already declared (as a single variable)

    std::vector<uint32_t> decos, decls, new_vars;
    auto scalar_id = [&](SpvScalar t) -> uint32_t {
        if (t == SpvScalar::Float) return f32;
        const std::pair<uint32_t, uint32_t> k = {32, t == SpvScalar::Sint ? 1u : 0u};
        auto it = ints.find(k);
        if (it != ints.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypeInt, id, 32u, k.second});
        ints[k] = id;
        return id;
    };
    auto vec4_id = [&](uint32_t scalar) -> uint32_t {
        const std::pair<uint32_t, uint32_t> k = {scalar, 4};
        auto it = vecs.find(k);
        if (it != vecs.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypeVector, id, scalar, 4u});
        vecs[k] = id;
        return id;
    };
    auto out_ptr_id = [&](uint32_t pointee) -> uint32_t {
        const std::pair<uint32_t, uint32_t> k = {StorageOutput, pointee};
        auto it = ptrs.find(k);
        if (it != ptrs.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypePointer, id, StorageOutput, pointee});
        ptrs[k] = id;
        return id;
    };
    for (const SpvOutputSlot& s : slots) {
        const uint32_t ptr = out_ptr_id(vec4_id(scalar_id(s.type)));
        const uint32_t var = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpVariable, ptr, var, StorageOutput});
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, var, DecorationLocation, s.location});
        new_vars.push_back(var);
    }

    out.clear();
    out.reserve(in.size() + decos.size() + decls.size() + new_vars.size() + 4);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    out[3] = bound;
    const Inst* entry = nullptr;
    for (const Inst& n : insts)
        if (n.at == entry_at) entry = &n;
    for (const Inst& n : insts) {
        if (n.at == first_type) out.insert(out.end(), decos.begin(), decos.end());
        if (n.at == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (&n == entry) {
            // the interface list is the tail of OpEntryPoint
            out.push_back(((n.len + static_cast<uint32_t>(new_vars.size())) << 16) | OpEntryPoint);
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at) + 1, in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
            out.insert(out.end(), new_vars.begin(), new_vars.end());
            continue;
        }
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

bool spirv_dual_source_output(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, bool> is_output;
    for (const Inst& n : insts)
        if (n.op == OpVariable && n.len >= 4 && in[n.at + 3] == StorageOutput) is_output[in[n.at + 2]] = true;
    std::vector<uint32_t> result;
    result.reserve(in.size() + 4);
    result.insert(result.end(), in.begin(), in.begin() + 5);
    bool changed = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpDecorate && n.len == 4 && w[2] == DecorationLocation && w[3] == 1 && is_output.count(w[1])) {
            result.insert(result.end(), {(4u << 16) | OpDecorate, w[1], DecorationLocation, 0u});
            result.insert(result.end(), {(4u << 16) | OpDecorate, w[1], 32u /* Index */, 1u});
            changed = true;
            continue;
        }
        result.insert(result.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    if (!changed) return false;
    out = std::move(result);
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {
namespace {
enum : uint32_t { OpCapability = 17, OpExecutionMode = 16, OpConstant = 43, OpStore = 62, OpLoad = 61, OpCopyObject = 83, OpAccessChain = 65, OpInBoundsAccessChain = 66,
                  DecOffset = 35, DecXfbBuffer = 36, DecXfbStride = 37, CapTransformFeedback = 53, ExecXfb = 11 };
}  // namespace

bool spirv_add_xfb(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, std::vector<SpvXfbOutput>* layout, uint32_t* stride) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    uint32_t f32 = 0, entry_id = 0;
    size_t first_type = 0, after_entry = 0, first_cap_end = 0;
    std::map<uint32_t, uint32_t> location;                            // variable id -> Location
    std::map<uint32_t, uint32_t> output_ptr_type;                     // output variable id -> pointer type id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecs, ptrs;     // (component, count) / (storage, pointee) -> id
    std::map<uint32_t, uint32_t> constants;                           // OpConstant id -> low word
    std::map<uint32_t, uint32_t> max_component;                       // variable -> highest constant component index stored (+1)
    std::map<uint32_t, bool> whole;                                   // variable used as a whole (store, load, copy)
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!first_type && is_type_or_constant(n.op)) first_type = n.at;
        if (n.op == OpCapability) first_cap_end = n.at + n.len;
        if (n.op == OpEntryPoint && !entry_id) entry_id = w[2];
        if (n.op == OpEntryPoint || n.op == OpExecutionMode) after_entry = n.at + n.len;
        if (n.op == OpTypeFloat && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4) vecs[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypePointer && n.len == 4) ptrs[{w[2], w[3]}] = w[1];
        else if (n.op == OpConstant && n.len >= 4) constants[w[2]] = w[3];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageOutput) output_ptr_type[w[2]] = w[1];
    }
    if (!first_type || !first_cap_end || !after_entry || !f32 || !entry_id) return false;
    auto is_captured = [&](uint32_t v) { return location.count(v) && output_ptr_type.count(v); };
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 5) {
            const uint32_t base = w[3];
            if (!is_captured(base)) continue;
            auto c = constants.find(w[4]);
            if (c == constants.end()) whole[base] = true;
            else max_component[base] = std::max(max_component[base], c->second + 1);
        } else if (n.op == OpStore && n.len >= 3) {
            if (is_captured(w[1])) whole[w[1]] = true;
        } else if ((n.op == OpLoad || n.op == OpCopyObject) && n.len >= 4) {
            if (is_captured(w[3])) whole[w[3]] = true;
        }
    }
    // the captured variables in Location order, each narrowed to the components that are stored
    std::map<uint32_t, uint32_t> by_location;   // Location -> variable id
    for (const auto& kv : output_ptr_type)
        if (location.count(kv.first)) by_location[location[kv.first]] = kv.first;
    if (by_location.empty()) return false;
    std::vector<uint32_t> decos, decls;
    std::map<uint32_t, uint32_t> new_ptr;       // variable id -> replacement pointer type
    uint32_t offset = 0;
    std::vector<SpvXfbOutput> lay;
    std::vector<std::pair<uint32_t, uint32_t>> decorate;   // variable, byte offset
    for (const auto& kv : by_location) {
        const uint32_t var = kv.second;
        uint32_t ncomp = (whole.count(var) || !max_component.count(var)) ? 4u : std::min(4u, max_component[var]);
        if (ncomp < 2) ncomp = 4;                // a lone scalar component keeps its vec4 (a scalar cannot be indexed)
        if (ncomp != 4) {
            const std::pair<uint32_t, uint32_t> vk = {f32, ncomp};
            uint32_t vid;
            auto vit = vecs.find(vk);
            if (vit != vecs.end()) {
                vid = vit->second;
            } else {
                vid = bound++;
                decls.insert(decls.end(), {(4u << 16) | OpTypeVector, vid, f32, ncomp});
                vecs[vk] = vid;
            }
            const std::pair<uint32_t, uint32_t> pk = {StorageOutput, vid};
            uint32_t pid;
            auto pit = ptrs.find(pk);
            if (pit != ptrs.end()) {
                pid = pit->second;
            } else {
                pid = bound++;
                decls.insert(decls.end(), {(4u << 16) | OpTypePointer, pid, StorageOutput, vid});
                ptrs[pk] = pid;
            }
            new_ptr[var] = pid;
        }
        decorate.push_back({var, offset});
        lay.push_back({kv.first, ncomp, offset});
        offset += 4 * ncomp;
    }
    for (const auto& d : decorate) {
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, d.first, DecXfbBuffer, 0u});
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, d.first, DecXfbStride, offset});
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, d.first, DecOffset, d.second});
    }
    std::vector<uint32_t> res;
    res.reserve(in.size() + decos.size() + decls.size() + 8);
    res.insert(res.end(), in.begin(), in.begin() + 5);
    res[3] = bound;
    bool decls_done = false;
    for (const Inst& n : insts) {
        if (n.at == first_type) res.insert(res.end(), decos.begin(), decos.end());
        // the new vector and pointer types go in front of the first variable that uses one (every type precedes its users)
        if (!decls_done && ((n.op == OpVariable && n.len >= 4 && in[n.at + 3] == StorageOutput && new_ptr.count(in[n.at + 2])) || n.op == OpFunction)) {
            res.insert(res.end(), decls.begin(), decls.end());
            decls_done = true;
        }
        std::vector<uint32_t> copy(in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        if (n.op == OpVariable && copy.size() >= 4 && copy[3] == StorageOutput) {
            auto it = new_ptr.find(copy[2]);
            if (it != new_ptr.end()) copy[1] = it->second;
        }
        res.insert(res.end(), copy.begin(), copy.end());
        if (n.at + n.len == first_cap_end) res.insert(res.end(), {(2u << 16) | OpCapability, CapTransformFeedback});
        if (n.at + n.len == after_entry) res.insert(res.end(), {(3u << 16) | OpExecutionMode, entry_id, ExecXfb});
    }
    if (!decls_done) return false;
    out = std::move(res);
    if (layout) *layout = lay;
    if (stride) *stride = offset;
    return true;
}

bool spirv_outputs_to_private(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    enum : uint32_t { OpVariableOp = 59, OpDecorateOp = 71, OpEntryPointOp = 15, StoragePrivate = 6, DecIndex = 32, DecComponent = 31, DecBuiltIn = 11 };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, bool> out_var, has_location, builtin;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpVariableOp && n.len >= 4 && w[3] == StorageOutput) out_var[w[2]] = true;
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecorationLocation) has_location[w[1]] = true;
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecBuiltIn) builtin[w[1]] = true;
    }
    if (out_var.empty()) return false;
    for (const auto& kv : out_var)
        if (builtin.count(kv.first) || !has_location.count(kv.first)) return false;
    const bool before_14 = in[1] < 0x10400;
    std::vector<uint32_t> res(in.begin(), in.begin() + 5);
    res.reserve(in.size());
    for (const Inst& n : insts) {
        std::vector<uint32_t> c(in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        if (n.op == OpTypePointer && n.len == 4 && c[2] == StorageOutput) c[2] = StoragePrivate;
        else if (n.op == OpVariableOp && n.len >= 4 && c[3] == StorageOutput) c[3] = StoragePrivate;
        else if (n.op == OpDecorateOp && n.len >= 4 && out_var.count(c[1]) && (c[2] == DecorationLocation || c[2] == DecIndex || c[2] == DecComponent)) continue;
        else if (n.op == OpEntryPointOp && before_14) {
            // OpEntryPoint model id name... interface ids: drop the output variables from the interface
            size_t name_words = 0;
            const char* nm = reinterpret_cast<const char*>(&c[3]);
            const size_t max_chars = (c.size() - 3) * 4;
            size_t chars = 0;
            while (chars < max_chars && nm[chars]) ++chars;
            name_words = chars / 4 + 1;
            const size_t iface = 3 + name_words;
            std::vector<uint32_t> d(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(std::min(iface, c.size())));
            for (size_t i = iface; i < c.size(); ++i)
                if (!out_var.count(c[i])) d.push_back(c[i]);
            d[0] = (static_cast<uint32_t>(d.size()) << 16) | OpEntryPointOp;
            c = d;
        }
        res.insert(res.end(), c.begin(), c.end());
    }
    out = std::move(res);
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

bool spirv_gs_passthrough(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out) {
    enum : uint32_t { OpExecutionModeOp = 16, OpConstantOp = 43, OpLoadOp = 61, OpStoreOp = 62, OpAccessChainOp = 65, OpEmitVertexOp = 218,
                      OpTypeArrayOp = 28, StorageInput = 1, ExecInputTriangles = 22 };
    for (const SpvOutputSlot& s : slots)
        if (s.type != SpvScalar::Float) return false;
    std::vector<uint32_t> tmp;
    if (!spirv_add_outputs(in, slots, tmp)) return false;       // the outputs (never written yet) and the interface entries

    std::vector<Inst> insts;
    for (size_t i = 5; i < tmp.size();) {
        const uint32_t len = tmp[i] >> 16;
        if (!len || i + len > tmp.size()) return false;
        insts.push_back({i, tmp[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = tmp[3];
    uint32_t f32 = 0, int32 = 0, v4 = 0, ptr_in_v4 = 0, first_function = 0, input_triangles = 0;
    std::map<uint32_t, uint32_t> location, var_type, array_elem, array_len_id, int_const;   // var -> Location / pointer type; array type -> element / length id; value -> id
    std::map<uint32_t, uint32_t> ptr_pointee, const_value;
    std::map<uint32_t, uint32_t> input_var, output_var;                                     // Location -> variable id
    for (const Inst& n : insts) {
        const uint32_t* w = &tmp[n.at];
        if (!first_function && n.op == OpFunction) first_function = static_cast<uint32_t>(n.at);
        if (n.op == OpExecutionModeOp && n.len >= 3 && w[2] == ExecInputTriangles) input_triangles = 1;
        if (n.op == OpTypeFloat && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32 && w[3] == 1) int32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4 && w[3] == 4 && f32 && w[2] == f32) v4 = w[1];
        else if (n.op == OpTypePointer && n.len == 4) { ptr_pointee[w[1]] = w[3]; if (w[2] == StorageInput && w[3] == v4) ptr_in_v4 = w[1]; }
        else if (n.op == OpTypeArrayOp && n.len == 4) { array_elem[w[1]] = w[2]; array_len_id[w[1]] = w[3]; }
        else if (n.op == OpConstantOp && n.len == 4) { const_value[w[2]] = w[3]; if (w[1] == int32) int_const[w[3]] = w[2]; }
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpVariable && n.len >= 4) var_type[w[2]] = w[1];
    }
    if (!input_triangles || !f32 || !v4 || !int32 || !first_function) return false;
    for (const Inst& n : insts) {
        const uint32_t* w = &tmp[n.at];
        if (n.op != OpVariable || n.len < 4 || !location.count(w[2])) continue;
        (w[3] == StorageInput ? input_var : output_var)[location[w[2]]] = w[2];
    }
    // every slot: an Input array [3] of vec4 and the Output variable spirv_add_outputs made
    std::vector<std::pair<uint32_t, uint32_t>> copies;   // (input var, output var)
    for (const SpvOutputSlot& s : slots) {
        if (!input_var.count(s.location) || !output_var.count(s.location)) return false;
        const uint32_t ptr = var_type[input_var[s.location]];
        const uint32_t arr = ptr_pointee.count(ptr) ? ptr_pointee[ptr] : 0;
        if (!arr || !array_elem.count(arr) || array_elem[arr] != v4 || !const_value.count(array_len_id[arr]) || const_value[array_len_id[arr]] != 3) return false;
        copies.emplace_back(input_var[s.location], output_var[s.location]);
    }
    uint32_t emits = 0;
    for (const Inst& n : insts) emits += n.op == OpEmitVertexOp;
    if (emits != 3) return false;

    std::vector<uint32_t> decls;
    if (!ptr_in_v4) { ptr_in_v4 = bound++; decls.insert(decls.end(), {(4u << 16) | OpTypePointer, ptr_in_v4, StorageInput, v4}); }
    for (uint32_t k = 0; k < 3; ++k)
        if (!int_const.count(k)) { const uint32_t id = bound++; int_const[k] = id; decls.insert(decls.end(), {(4u << 16) | OpConstantOp, int32, id, k}); }

    out.clear();
    out.reserve(tmp.size() + decls.size() + 12 * copies.size() + 8);
    out.insert(out.end(), tmp.begin(), tmp.begin() + 5);
    uint32_t emit_index = 0;
    for (const Inst& n : insts) {
        if (n.at == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (n.op == OpEmitVertexOp) {
            for (const auto& c : copies) {
                const uint32_t chain = bound++, val = bound++;
                out.insert(out.end(), {(5u << 16) | OpAccessChainOp, ptr_in_v4, chain, c.first, int_const[emit_index]});
                out.insert(out.end(), {(4u << 16) | OpLoadOp, v4, val, chain});
                out.insert(out.end(), {(3u << 16) | OpStoreOp, c.second, val});
            }
            ++emit_index;
        }
        out.insert(out.end(), tmp.begin() + static_cast<std::ptrdiff_t>(n.at), tmp.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    out[3] = bound;
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

bool spirv_int_attributes(const std::vector<uint32_t>& in, const std::vector<SpvIntSlot>& slots, std::vector<uint32_t>& out) {
    enum : uint32_t { OpTypeIntOp = 21, OpTypeFloatOp = 22, OpTypeVectorOp = 23, OpTypeArrayOp = 28, OpTypePointerOp = 32, OpConstantOp = 43, OpVariableOp = 59,
                      OpLoadOp = 61, OpStoreOp = 62, OpAccessChainOp = 65, OpInBoundsAccessChainOp = 66, OpDecorateOp = 71, OpCompositeConstructOp = 80,
                      OpBitcastOp = 124, OpEntryPointOp = 15, OpFunctionOp = 54, DecComponent = 31, StorageInputC = 1, StoragePrivateC = 6 };
    if (slots.empty() || in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    uint32_t f32 = 0, v4 = 0, u32 = 0, s32 = 0, entry_id = 0;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ptrs;      // (storage, pointee) -> pointer type id
    std::map<uint32_t, uint32_t> ptr_pointee, location, var_type;
    std::map<uint32_t, size_t> type_at;
    std::map<uint32_t, uint32_t> vec4_of;                         // scalar type id -> its vec4 id
    std::map<uint32_t, uint32_t> array_elem, array_len_id, const_value;   // array type -> element type / length constant; constant -> value
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpTypeFloatOp && n.len >= 3 && w[2] == 32) { f32 = w[1]; type_at[w[1]] = n.at; }
        else if (n.op == OpTypeIntOp && n.len == 4 && w[2] == 32) { (w[3] ? s32 : u32) = w[1]; type_at[w[1]] = n.at; }
        else if (n.op == OpTypeVectorOp && n.len == 4 && w[3] == 4) { vec4_of[w[2]] = w[1]; type_at[w[1]] = n.at; }
        else if (n.op == OpTypeArrayOp && n.len == 4) { array_elem[w[1]] = w[2]; array_len_id[w[1]] = w[3]; type_at[w[1]] = n.at; }
        else if (n.op == OpConstantOp && n.len == 4) const_value[w[2]] = w[3];
        else if (n.op == OpTypePointerOp && n.len == 4) { ptrs[{w[2], w[3]}] = w[1]; ptr_pointee[w[1]] = w[3]; type_at[w[1]] = n.at; }
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpVariableOp && n.len >= 4) var_type[w[2]] = w[1];
        else if (n.op == OpEntryPointOp && !entry_id) entry_id = w[2];
    }
    if (!f32 || !vec4_of.count(f32) || !entry_id) return false;
    v4 = vec4_of[f32];
    // One piece of work per input variable that changes. A plain variable is one vec4 at its Location; the translator also declares an Input
    // array of vec4 (a shader that indexes its attributes with a computed index), which covers Locations base .. base + length - 1.
    struct Elem { uint32_t location; int kind; uint32_t new_var; };   // kind 0 float, 1 uint, 2 sint
    struct Work { uint32_t old_var, old_ptr, array_type; size_t var_at; std::vector<Elem> elems; };
    std::vector<Work> work;
    size_t first_var_at = SIZE_MAX;
    std::map<uint32_t, int> slot_kind;                            // Location -> 1 uint / 2 sint
    for (const SpvIntSlot& sl : slots) slot_kind[sl.location] = sl.is_signed ? 2 : 1;
    std::map<uint32_t, size_t> work_of_var;
    for (const SpvIntSlot& sl : slots) {
        uint32_t var = 0;
        size_t at = 0;
        uint32_t array_type = 0, base = 0, len = 0;
        for (const Inst& n : insts) {
            const uint32_t* w = &in[n.at];
            if (n.op != OpVariableOp || n.len < 4 || w[3] != StorageInputC || !location.count(w[2])) continue;
            const uint32_t loc = location[w[2]], ptr = w[1];
            if (loc == sl.location && ptr_pointee.count(ptr) && ptr_pointee[ptr] == v4) { var = w[2]; at = n.at; array_type = 0; }
            else if (ptr_pointee.count(ptr) && array_elem.count(ptr_pointee[ptr]) && array_elem[ptr_pointee[ptr]] == v4 &&
                     const_value.count(array_len_id[ptr_pointee[ptr]])) {
                const uint32_t n_elems = const_value[array_len_id[ptr_pointee[ptr]]];
                if (sl.location >= loc && sl.location < loc + n_elems) { var = w[2]; at = n.at; array_type = ptr_pointee[ptr]; base = loc; len = n_elems; }
            }
        }
        if (!var) return false;
        if (work_of_var.count(var)) continue;                      // the array of an earlier slot
        Work wk{var, var_type[var], array_type, at, {}};
        if (array_type) {
            for (uint32_t e = 0; e < len; ++e) wk.elems.push_back({base + e, slot_kind.count(base + e) ? slot_kind[base + e] : 0, 0});
        } else {
            wk.elems.push_back({sl.location, sl.is_signed ? 2 : 1, 0});
        }
        work_of_var[var] = work.size();
        work.push_back(std::move(wk));
        first_var_at = std::min(first_var_at, at);
    }
    // the types the new variables need have to be declared before the first variable that changes
    std::vector<uint32_t> types;
    auto need_scalar = [&](bool is_signed) -> uint32_t {
        const uint32_t t = is_signed ? s32 : u32;
        return t && type_at.count(t) && type_at[t] < first_var_at ? t : 0;
    };
    auto vec4_for = [&](uint32_t scalar) -> uint32_t {
        auto it = vec4_of.find(scalar);
        if (it != vec4_of.end() && type_at[it->second] < first_var_at) return it->second;
        const uint32_t id = bound++;
        types.insert(types.end(), {(4u << 16) | OpTypeVectorOp, id, scalar, 4u});
        vec4_of[scalar] = id;
        type_at[id] = 0;
        return id;
    };
    auto ptr_for = [&](uint32_t storage, uint32_t pointee) -> uint32_t {
        auto it = ptrs.find({storage, pointee});
        if (it != ptrs.end() && type_at[it->second] < first_var_at) return it->second;
        const uint32_t id = bound++;
        types.insert(types.end(), {(4u << 16) | OpTypePointerOp, id, storage, pointee});
        ptrs[{storage, pointee}] = id;
        ptr_pointee[id] = pointee;
        type_at[id] = 0;
        return id;
    };
    std::map<uint32_t, uint32_t> priv_ptr_of;    // old Input pointer type -> the Private pointer type to the same pointee
    auto private_of = [&](uint32_t old_ptr) -> uint32_t {
        auto it = priv_ptr_of.find(old_ptr);
        if (it != priv_ptr_of.end()) return it->second;
        return priv_ptr_of[old_ptr] = ptr_for(StoragePrivateC, ptr_pointee[old_ptr]);
    };
    std::map<uint32_t, uint32_t> old_to_work;    // old var -> index in work
    std::vector<uint32_t> decls_after_types, decos;
    uint32_t v4u = 0, v4i = 0;
    for (size_t i = 0; i < work.size(); ++i) {
        Work& wk = work[i];
        for (Elem& el : wk.elems) {
            uint32_t vec = v4;
            if (el.kind) {
                const uint32_t sc = need_scalar(el.kind == 2);
                if (!sc) return false;
                uint32_t& vv = el.kind == 2 ? v4i : v4u;
                if (!vv) vv = vec4_for(sc);
                vec = vv;
            }
            const uint32_t in_ptr = ptr_for(StorageInputC, vec);
            el.new_var = bound++;
            decls_after_types.insert(decls_after_types.end(), {(4u << 16) | OpVariableOp, in_ptr, el.new_var, StorageInputC});
            decos.insert(decos.end(), {(4u << 16) | OpDecorateOp, el.new_var, DecorationLocation, el.location});
        }
        private_of(wk.old_ptr);
        old_to_work[wk.old_var] = static_cast<uint32_t>(i);
    }

    // the pointer types the access chains into the old inputs get (declared before the first changed variable)
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChainOp || n.op == OpInBoundsAccessChainOp) && n.len >= 5 && old_to_work.count(w[3])) {
            if (!ptr_pointee.count(w[1])) return false;
            private_of(w[1]);
        }
    }

    // the entry function: the new instructions go behind its leading OpVariable run
    uint32_t entry_func = 0;
    for (const Inst& n : insts)
        if (n.op == OpEntryPointOp) { entry_func = in[n.at + 2]; break; }
    size_t insert_at = 0;
    for (size_t k = 0; k < insts.size() && !insert_at; ++k) {
        if (insts[k].op == OpFunctionOp && in[insts[k].at + 2] == entry_func) {
            size_t j = k + 1;
            while (j < insts.size() && insts[j].op != 248 /* OpLabel */) ++j;
            ++j;
            while (j < insts.size() && insts[j].op == OpVariableOp) ++j;
            if (j < insts.size()) insert_at = insts[j].at;
        }
    }
    if (!insert_at) return false;
    std::vector<uint32_t> init;
    for (const Work& wk : work) {
        std::vector<uint32_t> values;
        for (const Elem& el : wk.elems) {
            if (!el.kind) {                                        // a float element: the new input holds the same value
                const uint32_t loaded = bound++;
                init.insert(init.end(), {(4u << 16) | OpLoadOp, v4, loaded, el.new_var});
                values.push_back(loaded);
                continue;
            }
            const uint32_t vec = el.kind == 2 ? v4i : v4u;
            const uint32_t loaded = bound++, cast = bound++;
            init.insert(init.end(), {(4u << 16) | OpLoadOp, vec, loaded, el.new_var});
            init.insert(init.end(), {(4u << 16) | OpBitcastOp, v4, cast, loaded});
            values.push_back(cast);
        }
        if (!wk.array_type) {
            init.insert(init.end(), {(3u << 16) | OpStoreOp, wk.old_var, values[0]});
        } else {
            const uint32_t whole = bound++;
            init.push_back(((3u + static_cast<uint32_t>(values.size())) << 16) | OpCompositeConstructOp);
            init.push_back(wk.array_type);
            init.push_back(whole);
            init.insert(init.end(), values.begin(), values.end());
            init.insert(init.end(), {(3u << 16) | OpStoreOp, wk.old_var, whole});
        }
    }

    out.clear();
    out.reserve(in.size() + types.size() + decls_after_types.size() + decos.size() + init.size() + 16);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    size_t first_function = 0;
    for (const Inst& n : insts)
        if (n.op == OpFunctionOp) { first_function = n.at; break; }
    bool deco_done = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!deco_done && is_type_or_constant(n.op)) { out.insert(out.end(), decos.begin(), decos.end()); deco_done = true; }
        if (n.at == first_var_at) out.insert(out.end(), types.begin(), types.end());
        if (n.at == first_function) out.insert(out.end(), decls_after_types.begin(), decls_after_types.end());
        if (n.at == insert_at) out.insert(out.end(), init.begin(), init.end());
        std::vector<uint32_t> c(in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        if (n.op == OpVariableOp && n.len >= 4 && old_to_work.count(w[2])) {            // the old input becomes a private variable
            c[1] = private_of(work[old_to_work[w[2]]].old_ptr);
            c[3] = StoragePrivateC;
        } else if (n.op == OpDecorateOp && n.len >= 4 && old_to_work.count(w[1]) && (w[2] == DecorationLocation || w[2] == DecComponent)) {
            continue;
        } else if ((n.op == OpAccessChainOp || n.op == OpInBoundsAccessChainOp) && n.len >= 5 && old_to_work.count(w[3])) {
            c[1] = private_of(w[1]);                                                       // a pointer into the old input is a Private pointer now
        } else if (n.op == OpEntryPointOp) {
            // OpEntryPoint model id name... interface ids: the name is a literal string. The old input leaves the interface and its new
            // inputs join it.
            const char* nm = reinterpret_cast<const char*>(&c[3]);
            size_t chars = 0;
            while (chars < (c.size() - 3) * 4 && nm[chars]) ++chars;
            const size_t iface = 3 + chars / 4 + 1;
            std::vector<uint32_t> nc(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(iface));
            for (size_t k = iface; k < c.size(); ++k) {
                if (old_to_work.count(c[k])) {
                    for (const Elem& el : work[old_to_work[c[k]]].elems) nc.push_back(el.new_var);
                } else {
                    nc.push_back(c[k]);
                }
            }
            nc[0] = (static_cast<uint32_t>(nc.size()) << 16) | (nc[0] & 0xffff);
            c = std::move(nc);
        }
        out.insert(out.end(), c.begin(), c.end());
    }
    out[3] = bound;
    return true;
}


std::vector<SpvIntSlot> spirv_predict_int_inputs(const std::vector<uint32_t>& in) {
    enum : uint32_t { OpTypeIntOp = 21, OpTypeFloatOp = 22, OpTypeVectorOp = 23, OpTypeArrayOp = 28, OpTypePointerOp = 32, OpConstantOp = 43, OpVariableOp = 59,
                      OpLoadOp = 61, OpStoreOp = 62, OpAccessChainOp = 65, OpInBoundsAccessChainOp = 66, OpDecorateOp = 71, OpBitcastOp = 124, StorageInputC = 1 };
    std::vector<SpvIntSlot> result;
    if (in.size() < 5) return result;
    uint32_t f32 = 0, v4 = 0;
    std::map<uint32_t, bool> is_int32;                               // type id -> 32-bit integer
    std::map<uint32_t, uint32_t> ptr_pointee, array_elem, array_len, const_value, location, var_ptr;
    std::map<uint32_t, uint32_t> var_elems;                          // candidate Input variable -> number of vec4 elements (1 for a plain vec4)
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return result;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpTypeFloatOp && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeIntOp && n.len == 4 && w[2] == 32) is_int32[w[1]] = true;
        else if (n.op == OpTypeVectorOp && n.len == 4 && w[3] == 4 && w[2] == f32 && f32) v4 = w[1];
        else if (n.op == OpTypeArrayOp && n.len == 4) { array_elem[w[1]] = w[2]; array_len[w[1]] = w[3]; }
        else if (n.op == OpConstantOp && n.len == 4) const_value[w[2]] = w[3];
        else if (n.op == OpTypePointerOp && n.len == 4 && w[2] == StorageInputC) ptr_pointee[w[1]] = w[3];
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpVariableOp && n.len >= 4 && w[3] == StorageInputC) var_ptr[w[2]] = w[1];
    }
    if (!f32 || !v4) return result;
    for (const auto& [var, ptr] : var_ptr) {
        if (!location.count(var) || !ptr_pointee.count(ptr)) continue;
        const uint32_t t = ptr_pointee[ptr];
        if (t == v4) var_elems[var] = 1;
        else if (array_elem.count(t) && array_elem[t] == v4 && const_value.count(array_len[t]) && const_value[array_len[t]] <= 16) var_elems[var] = const_value[array_len[t]];
    }
    if (var_elems.empty()) return result;
    // use counts per location: [0] bitcast to a 32-bit integer, [1] anything else
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> uses;
    std::map<uint32_t, uint32_t> chain_loc;                          // access chain result -> location
    std::map<uint32_t, uint32_t> load_loc;                           // load result -> location
    std::map<uint32_t, uint32_t> pvar_loc;                           // local variable that holds a loaded attribute -> location
    auto spoil_var = [&](uint32_t var) { for (uint32_t e = 0; e < var_elems[var]; ++e) ++uses[location[var] + e].second; };
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChainOp || n.op == OpInBoundsAccessChainOp) && n.len >= 5 && var_elems.count(w[3])) {
            const uint32_t var = w[3];
            const uint32_t indices = n.len - 4;
            const bool array = var_elems[var] > 1 || indices == 2;
            const uint32_t want = array ? 2 : 1;
            // a computed index reads whichever attribute the shader picks at run time: not counted either way
            if (indices != want || !const_value.count(w[4]) || (want == 2 && !const_value.count(w[5]))) continue;
            const uint32_t elem = array ? const_value[w[4]] : 0;
            if (elem >= var_elems[var]) continue;
            chain_loc[w[2]] = location[var] + elem;
        } else if (n.op == OpLoadOp && n.len >= 4) {
            if (chain_loc.count(w[3])) load_loc[w[2]] = chain_loc[w[3]];
            else if (pvar_loc.count(w[3])) load_loc[w[2]] = pvar_loc[w[3]];          // the value came back out of a local variable
            else if (var_elems.count(w[3])) spoil_var(w[3]);
        } else if (n.op == OpStoreOp && n.len >= 3 && load_loc.count(w[2]) && !chain_loc.count(w[1]) && !var_elems.count(w[1])) {
            auto it = pvar_loc.find(w[1]);                                            // stored into a local variable: later loads of it stand for the attribute
            if (it != pvar_loc.end() && it->second != load_loc[w[2]]) { ++uses[it->second].second; ++uses[load_loc[w[2]]].second; }
            else pvar_loc[w[1]] = load_loc[w[2]];
        }
    }
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpLoadOp || n.op == OpAccessChainOp || n.op == OpInBoundsAccessChainOp) continue;
        if (n.op == OpStoreOp && n.len >= 3 && pvar_loc.count(w[1])) continue;          // the store into a local variable was followed above
        for (uint32_t k = 1; k < n.len; ++k) {
            auto it = load_loc.find(w[k]);
            if (it == load_loc.end()) continue;
            if (n.op == OpBitcastOp && n.len == 4 && k == 3 && is_int32.count(w[1])) ++uses[it->second].first;
            else ++uses[it->second].second;
        }
    }
    for (const auto& [loc, u] : uses)
        if (u.first > 0 && u.second == 0) {
            SpvIntSlot sl;
            sl.location = loc;
            sl.is_signed = false;
            result.push_back(sl);
        }
    return result;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

// Ported from featureJosh/pokopia-recomp (GPL-3.0-or-later), runtime/src/gfx/nvn/nvn_spirv_patch.cpp, unchanged.
// Approach from pokopia-native (MIT) windows/docs/ballot-hang.md and windows/src/nxrt/vk/vk_shader.c (vkspv_ballot_per_lane) @4a72d23:
// make NVN's warp-aggregated counters per lane by replacing their ballot(true) with the lane's own bit. Their matcher wants one ballot
// that reaches both the leader and the count; our translator writes a counter's ballot once per use (the leader, the count and the
// rank: three OpGroupNonUniformBallot, AstOptimizer propagates the unary ballot into each use), so the matcher here is our own: it
// follows each ballot's value, joins the ballots of one counter into a group through what they meet (the leader test that guards the
// add, the shuffle call that hands the result out, the add of the rank to that result) and rewrites a group only as a whole.
bool spirv_ballot_per_lane(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, BallotRewrite* stats) {
    enum : uint32_t {
        oName = 5, oMemberName = 6, oExtInst = 12, oTypeInt = 21, oTypeVector = 23, oTypePointer = 32, oConstantTrue = 41, oConstant = 43,
        oFunction = 54, oFunctionEnd = 56, oFunctionCall = 57, oVariable = 59, oLoad = 61, oStore = 62, oAccessChain = 65,
        oInBoundsAccessChain = 66, oDecorate = 71, oCompositeConstruct = 80, oCompositeExtract = 81, oCopyObject = 83, oBitcast = 124,
        oIAdd = 128, oIMul = 132, oIEqual = 170, oShrL = 194, oShrA = 195, oShl = 196, oAnd = 199, oBitCount = 205, oAtomicIAdd = 234,
        oSelectionMerge = 247, oLabel = 248, oBranchConditional = 250, oBallot = 339, oShuffle = 345,
        DecBuiltIn = 11, BiLane = 41, BiLtMask = 4420, ExtFindSMsb = 74, ExtFindUMsb = 75,
    };
    // value flags (every use of an id that carries one is checked) and tags (followed, never checked)
    enum : uint32_t { B = 1, M = 2, N = 4, A = 8, K = 16, CHECKED = 31, LANE = 32, LTM = 64, LTEST = 128, S = 256 };
    BallotRewrite st;
    if (stats) st.why = stats->why;
    if (in.size() < 5 || in[0] != 0x07230203u || in[3] == 0 || in[3] > (1u << 22)) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    const uint32_t bound = in[3];
    std::vector<size_t> def(bound, 0);
    std::vector<uint8_t> is_const(bound, 0), is_true(bound, 0), shuf_fn(bound, 0), nst(bound, 0);   // nst: stores into each id (2: more)
    std::vector<uint32_t> cval(bound, 0);
    uint32_t tu = 0, c0 = 0, c1 = 0, lane_var = 0, lt_var = 0;
    size_t first_fn = 0;
    std::vector<size_t> ballots;   // instruction index of each ballot(true)
    for (size_t k = 0, fn = 0; k < insts.size(); k++) {
        const Inst& n = insts[k];
        const uint32_t* w = &in[n.at];
        if ((n.op == oTypeInt || n.op == oTypeVector || n.op == oTypePointer) && n.len >= 2 && w[1] < bound) def[w[1]] = n.at;
        if (n.op == oVariable && n.len >= 4 && w[2] < bound) def[w[2]] = n.at;
        if (n.op == oStore && n.len >= 3 && w[1] < bound && nst[w[1]] < 2) nst[w[1]]++;
        if (n.op == oTypeInt && n.len == 4 && w[2] == 32 && w[3] == 0 && !tu) tu = w[1];
        if (n.op == oConstantTrue && n.len == 3 && w[2] < bound) is_true[w[2]] = 1;
        if (n.op == oConstant && n.len == 4 && w[2] < bound) {
            is_const[w[2]] = 1;
            cval[w[2]] = w[3];
            if (tu && w[1] == tu && w[3] == 0 && !c0) c0 = w[2];
            if (tu && w[1] == tu && w[3] == 1 && !c1) c1 = w[2];
        }
        if (n.op == oDecorate && n.len == 4 && w[2] == DecBuiltIn && w[3] == BiLane) lane_var = w[1];
        if (n.op == oDecorate && n.len == 4 && w[2] == DecBuiltIn && w[3] == BiLtMask) lt_var = w[1];
        if (n.op == oFunction && n.len >= 3) { fn = w[2]; if (!first_fn) first_fn = k; }
        if (n.op == oFunctionEnd) fn = 0;
        if (n.op == oShuffle && fn && fn < bound) shuf_fn[fn] = 1;
        if (n.op == oBallot && n.len == 5 && w[4] < bound && is_true[w[4]]) ballots.push_back(k);
    }
    if (ballots.empty() || !tu || !lane_var || lane_var >= bound || !def[lane_var] || !first_fn) return false;
    {   // the lane index: an Input uint, as the leader test reads it
        const uint32_t pt = in[def[lane_var] + 1];
        if (pt >= bound || !def[pt] || (in[def[pt]] & 0xffff) != oTypePointer || in[def[pt] + 3] != tu) return false;
    }
    // groups: one per ballot, joined by union-find
    const uint32_t nb = static_cast<uint32_t>(ballots.size());
    std::vector<uint32_t> parent(nb);
    for (uint32_t g = 0; g < nb; g++) parent[g] = g;
    auto find = [&](uint32_t g) { while (parent[g] != g) g = parent[g] = parent[parent[g]]; return g; };
    constexpr uint32_t NOG = ~0u;
    std::vector<uint32_t> F(bound, 0), G(bound, NOG);   // flags and group of each id (values and pointers alike)
    bool changed = false;
    auto join = [&](uint32_t a, uint32_t b) {   // groups a and b (NOG: none) as one; returns the group
        if (a == NOG) return b;
        if (b == NOG) return a;
        a = find(a), b = find(b);
        if (a != b) { parent[std::max(a, b)] = std::min(a, b); changed = true; }
        return std::min(a, b);
    };
    auto give = [&](uint32_t id, uint32_t f, uint32_t g) {
        if (id >= bound || (!f && g == NOG)) return;
        if ((F[id] | f) != F[id]) { F[id] |= f; changed = true; }
        if (g != NOG) {
            if (G[id] == NOG) { G[id] = find(g); changed = true; }
            else join(G[id], g);
        }
    };
    for (uint32_t g = 0; g < nb; g++) give(in[insts[ballots[g]].at + 2], B, g);
    give(lane_var, LANE, NOG);
    if (lt_var && lt_var < bound) give(lt_var, LTM, NOG);
    std::vector<uint8_t> bad(nb, 0), candidate(nb, 0), has_msb(nb, 0);
    struct Region { size_t from, to; uint32_t g; };
    std::vector<Region> regions;
    struct Atom { size_t at; uint32_t g; };
    std::vector<Atom> atoms;
    auto fl = [&](uint32_t id) { return id < bound ? F[id] : 0u; };
    auto gr = [&](uint32_t id) { return id < bound ? G[id] : NOG; };
    auto cst = [&](uint32_t id) { return id < bound && is_const[id]; };
    // One walk: flow (final = false, repeated until nothing changes), then the checks (final = true): every use of a checked value
    // must be one the rewrite keeps right, else its group is left alone.
    auto walk = [&](bool final) {
        std::vector<size_t> label_at(final ? bound : 0, 0);
        if (final) for (const Inst& n : insts) if (n.op == oLabel && in[n.at + 1] < bound) label_at[in[n.at + 1]] = n.at;
        uint32_t cur_op = 0;
        auto mark = [&](uint32_t id) {
            if (final && id < bound && (F[id] & CHECKED) && G[id] != NOG) {
                const uint32_t g = find(G[id]);
                if (!bad[g] && stats && stats->why) stats->why->push_back("group " + std::to_string(g) + ": value %" + std::to_string(id) + " (flags " + std::to_string(F[id] & CHECKED) + ") used by op " + std::to_string(cur_op));
                bad[g] = 1;
            }
        };
        for (size_t k = 0; k < insts.size(); k++) {
            if (k < first_fn) continue;   // declarations: their literals are not ids
            const Inst& n = insts[k];
            const uint32_t* w = &in[n.at];
            const uint32_t op = n.op, wc = n.len;
            cur_op = op;
            auto u = [&](uint32_t a) { return a < wc ? w[a] : 0u; };
            switch (op) {
            case oName: case oMemberName: case oDecorate: case oBallot: case oVariable: case oFunction: case oLabel:
                continue;
            case oCompositeExtract:   // only .x of a ballot (the other words are 0 with 32-lane subgroups, and 0 once rewritten)
                if ((fl(u(3)) & B) && (wc != 5 || w[4] != 0)) mark(u(3));
                give(u(2), fl(u(3)), gr(u(3)));
                continue;
            case oBitcast: case oCopyObject: case oLoad:
                if (wc >= 4) give(w[2], fl(w[3]), gr(w[3]));
                continue;
            case oStore:
                // the rank kept in a GPU register (a variable stored more than once): not followed. NVN's rank is dead after its add
                // (pokopia-native's note: 560a0262fe47818a); here 0f9696499923d6d3 copies it into a float register that is overwritten
                // before it is read
                if (wc >= 3 && (fl(w[2]) & CHECKED) == K && w[1] < bound && nst[w[1]] > 1) continue;
                if (wc >= 3) give(w[1], fl(w[2]), gr(w[2]));
                continue;
            case oAccessChain: case oInBoundsAccessChain:
                if (wc < 4) continue;
                for (uint32_t a = 4; a < wc; a++) mark(w[a]);   // a counter value as an index
                if (fl(w[3]) & LTM) { if (wc == 5 && cst(w[4]) && cval[w[4]] == 0) give(w[2], LTM, NOG); continue; }
                give(w[2], fl(w[3]), gr(w[3]));
                continue;
            case oExtInst:
                if (wc == 6 && (w[4] == ExtFindUMsb || w[4] == ExtFindSMsb) && (fl(w[5]) & CHECKED) == B) {
                    give(w[2], M, gr(w[5]));
                    if (final) { uint32_t g = find(gr(w[5])); candidate[g] = has_msb[g] = 1; }
                    continue;
                }
                for (uint32_t a = 5; a < wc; a++) mark(w[a]);
                continue;
            case oBitCount:
                if (wc == 4 && (fl(w[3]) & CHECKED) == B) { give(w[2], N, gr(w[3])); continue; }
                if (wc == 4 && (fl(w[3]) & CHECKED) == K) { give(w[2], K, gr(w[3])); continue; }
                if (wc >= 4) mark(w[3]);
                continue;
            case oAnd: case oIMul: case oShl: case oShrL: case oShrA: case oIAdd: {
                if (wc != 5) break;
                const uint32_t a = w[3], b = w[4], fa = fl(a) & CHECKED, fb = fl(b) & CHECKED;
                if (!fa && !fb) {   // tags only: the slot (the result handed out plus the rank) is free
                    if (op == oIAdd && ((fl(a) & S) || (fl(b) & S))) give(w[2], 0, join(gr(a), gr(b)));
                    continue;
                }
                if (op == oAnd && ((fa == B && (fl(b) & LTM)) || (fb == B && (fl(a) & LTM)))) {   // the rank's mask
                    give(w[2], K, fa ? gr(a) : gr(b));
                    if (final) candidate[find(fa ? gr(a) : gr(b))] = 1;
                    continue;
                }
                const bool by_const = cst(b) || ((op == oIMul || op == oAnd || op == oIAdd) && cst(a));
                const uint32_t f = fa | fb;
                // a count scaled by a constant (entries of a constant size), a rank masked, scaled or split into halves
                if (by_const && op != oIAdd && ((f == N && (op == oIMul || op == oShl)) || f == K)) {
                    give(w[2], f, join(gr(a), gr(b)));
                    continue;
                }
                if (op == oIAdd && f == K && (fa == 0 || fb == 0)) {
                    const uint32_t o = fa ? b : a;
                    if (fl(o) & S) { give(w[2], 0, join(gr(a), gr(b))); give(w[2], S, NOG); continue; }   // rank + the result handed out
                    if (cst(o)) { give(w[2], K, join(gr(a), gr(b))); continue; }
                }
                if (op == oIAdd && fa == K && fb == K) { give(w[2], K, join(gr(a), gr(b))); continue; }   // halves of a scaled rank
                mark(a); mark(b);
                continue;
            }
            case oIEqual:   // the leader test: the leader against this lane
                if (wc == 5 && (((fl(w[3]) & CHECKED) == M && (fl(w[4]) & LANE)) || ((fl(w[4]) & CHECKED) == M && (fl(w[3]) & LANE)))) {
                    give(w[2], LTEST, (fl(w[3]) & M) ? gr(w[3]) : gr(w[4]));
                    continue;
                }
                break;
            case oBranchConditional:
                if (final && wc >= 4 && (fl(w[1]) & LTEST) && k > 0 && insts[k - 1].op == oSelectionMerge) {
                    const uint32_t t = w[2], m = in[insts[k - 1].at + 1];
                    if (t < bound && m < bound && label_at[t] && label_at[m] > label_at[t]) regions.push_back({label_at[t], label_at[m], gr(w[1])});
                }
                if (wc >= 2) mark(w[1]);
                continue;
            case oAtomicIAdd:
                if (wc == 7 && (fl(w[6]) & CHECKED) == N && !(fl(w[3]) & CHECKED) && !(fl(w[4]) & CHECKED) && !(fl(w[5]) & CHECKED)) {
                    give(w[2], A, gr(w[6]));
                    if (final) { atoms.push_back({n.at, gr(w[6])}); candidate[find(gr(w[6]))] = 1; }
                    continue;
                }
                break;
            case oFunctionCall:   // the result handed out from the leader: a call of a function that shuffles, the leader among its arguments
                if (wc >= 4 && w[3] < bound && shuf_fn[w[3]]) {
                    uint32_t g = NOG, f = 0;
                    for (uint32_t a = 4; a < wc; a++) { f |= fl(w[a]); if (fl(w[a]) & CHECKED) g = join(g, gr(w[a])); }
                    if (f & CHECKED) {
                        if ((f & (N | A)) && !(f & M)) for (uint32_t a = 4; a < wc; a++) mark(w[a]);
                        if (f & (B | K)) for (uint32_t a = 4; a < wc; a++) mark(w[a]);
                        give(w[2], S, g);
                    }
                    continue;
                }
                break;
            default:
                break;
            }
            // any other use of a checked value
            const uint32_t from = (op == oStore || op == 63 || op == 228 || op == 250 || op == 251 || op == 254) ? 1 : 3;
            for (uint32_t a = from; a < wc; a++) mark(w[a]);
        }
    };
    for (int pass = 0; pass < 64; pass++) {
        changed = false;
        walk(false);
        if (!changed) break;
    }
    walk(true);
    // a group is taken when it holds a leader, an add of its count inside its own leader test, and nothing it reaches is used otherwise
    std::vector<uint8_t> grp_bad(nb, 0), grp_cand(nb, 0), grp_msb(nb, 0), grp_atom(nb, 0);
    for (uint32_t g = 0; g < nb; g++) {
        const uint32_t r = find(g);
        grp_bad[r] |= bad[g]; grp_cand[r] |= candidate[g]; grp_msb[r] |= has_msb[g];
    }
    for (const Atom& at : atoms) {
        const uint32_t r = find(at.g);
        bool inside = false;
        for (const Region& rg : regions) if (rg.g != NOG && find(rg.g) == r && at.at > rg.from && at.at < rg.to) inside = true;
        if (inside) grp_atom[r] = 1;
        else { if (!grp_bad[r] && stats && stats->why) stats->why->push_back("group " + std::to_string(r) + ": its atomic add is outside its leader test"); grp_bad[r] = 1; }
    }
    std::vector<uint8_t> take(nb, 0);
    bool any = false, partial = false;
    for (uint32_t g = 0; g < nb; g++) {
        const uint32_t r = find(g);
        if (!grp_cand[r]) continue;   // not a counter's ballot (REDUX / MATCH masks, first-lane broadcasts): left alone
        if (grp_bad[r] || !grp_msb[r] || !grp_atom[r]) {
            if (stats && stats->why && find(g) == g && !grp_bad[r]) stats->why->push_back("group " + std::to_string(r) + (grp_msb[r] ? "" : ": no leader (FindUMsb)") + (grp_atom[r] ? "" : ": no atomic add of its count"));
            partial = true;
            continue;
        }
        take[g] = 1;
        any = true;
    }
    // a counter's ballot that could not be joined to its group might belong to a taken one: then nothing is rewritten
    if (partial) any = false;
    for (uint32_t g = 0; g < nb; g++) {   // the ballot's type: a vector of 4 uints
        const uint32_t vt = in[insts[ballots[g]].at + 1];
        if (take[g] && !(vt < bound && def[vt] && (in[def[vt]] & 0xffff) == oTypeVector && in[def[vt] + 2] == tu && in[def[vt] + 3] == 4)) any = false;
    }
    if (stats) {
        for (uint32_t g = 0; g < nb; g++) if (find(g) == g && grp_cand[g]) { st.groups++; if (grp_bad[g] || !grp_msb[g] || !grp_atom[g]) st.groups_left++; }
    }
    if (!any) { if (stats) *stats = st; return false; }
    // the rewrite: the constants 0 and 1 when missing (before the first function), each ballot -> this lane's bit in .x
    uint32_t nbound = bound;
    out.clear();
    out.reserve(in.size() + 16 * nb + 8);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    std::vector<uint8_t> at_ballot(insts.size(), 0);
    for (uint32_t g = 0; g < nb; g++) if (take[g]) at_ballot[ballots[g]] = 1;
    for (size_t k = 0; k < insts.size(); k++) {
        const Inst& n = insts[k];
        if (k == first_fn) {
            if (!c0) { c0 = nbound++; out.insert(out.end(), {4u << 16 | oConstant, tu, c0, 0u}); }
            if (!c1) { c1 = nbound++; out.insert(out.end(), {4u << 16 | oConstant, tu, c1, 1u}); }
        }
        if (!at_ballot[k]) { out.insert(out.end(), in.begin() + n.at, in.begin() + n.at + n.len); continue; }
        const uint32_t lid = nbound++, bit = nbound++;
        out.insert(out.end(), {4u << 16 | oLoad, tu, lid, lane_var});
        out.insert(out.end(), {5u << 16 | oShl, tu, bit, c1, lid});
        out.insert(out.end(), {7u << 16 | oCompositeConstruct, in[n.at + 1], in[n.at + 2], bit, c0, c0, c0});
        st.ballots++;
    }
    out[3] = nbound;
    if (stats) *stats = st;
    return true;
}

}  // namespace nxrt::nvn::be::gpu
