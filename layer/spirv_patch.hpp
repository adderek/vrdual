// Minimal SPIR-V rewriter: shifts clip-space X of the vertex shader output.
//
// Before every OpReturn of each Vertex entry point we append:
//     v   = load gl_Position
//     p   = eye params vec4 (a, b, wf, inv)   -- baked constant, or loaded
//                                               from a GPU buffer (live mode)
//     off = mix(a, b, clamp((wf - v.w) * inv, 0, 1))
//     x'  = (skip_w1 && v.w == 1.0) ? v.x : v.x + off
//     store gl_Position = insert(x', v, 0)
//
// For a perspective projection P, translating the camera by d along view X
// changes clip.x by -P[0][0]*d and leaves clip.w unchanged, so a constant clip
// offset is the exact per-eye parallel camera shift — no need to find the
// app's view matrix (3D Vision's "automatic" trick). clip.w is view depth, so
// a/b let far and near objects use different IPDs (see README, near/far IPD).
//
// Live mode reads the vec4 through a baked 64-bit buffer device address
// (SPV_KHR_physical_storage_buffer), so the layer can change IPD at runtime by
// writing host-visible memory — no pipeline rebuild, app stays unaware.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace spv {

enum : uint32_t {
    OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15,
    OpCapability = 17, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpConstantComposite = 44,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62,
    OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72, OpCompositeExtract = 81,
    OpCompositeInsert = 82, OpBitcast = 124, OpFAdd = 129, OpFSub = 131, OpFMul = 133,
    OpSelect = 169, OpFOrdEqual = 180, OpReturn = 253,
};
enum : uint32_t {
    ExecModelVertex = 0, StorageOutput = 3, StoragePhysicalStorageBuffer = 5349, DecBuiltIn = 11,
    BuiltInPosition = 0, AddrLogical = 0, AddrPhysicalStorageBuffer64 = 5348,
    CapPhysicalStorageBufferAddresses = 5347, MemAccessAligned = 2, GLSLFClamp = 43,
};

struct PatchOptions {
    float params[4] = {0, 0, 0, 0};  // baked params (used when addr == 0)
    uint64_t addr = 0;               // live: device address of a vec4 (16-byte aligned)
    bool skip_w1 = true;
};

struct PatchResult {
    bool patched = false;
    std::string why;  // reason when not patched
};

inline uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// Literal string -> words (nul-terminated, zero-padded).
inline std::vector<uint32_t> str_words(const char* s) {
    size_t n = strlen(s) + 1;
    std::vector<uint32_t> w((n + 3) / 4, 0);
    memcpy(w.data(), s, n);
    return w;
}
inline std::string word_str(const uint32_t* w, size_t max_words) {
    std::string s;
    for (size_t i = 0; i < max_words * 4; ++i) {
        char c = ((const char*)w)[i];
        if (!c) break;
        s += c;
    }
    return s;
}

inline PatchResult patch_vertex(std::vector<uint32_t>& in, const PatchOptions& o) {
    PatchResult r;
    if (in.size() < 5 || in[0] != 0x07230203u) { r.why = "not SPIR-V (or wrong endianness)"; return r; }

    struct Inst { size_t off; uint32_t op, wc; };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (wc == 0 || i + wc > in.size()) { r.why = "malformed instruction stream"; return r; }
        insts.push_back({i, op, wc});
        i += wc;
    }
    auto w = [&](const Inst& I, uint32_t k) { return in[I.off + k]; };

    // 1. Vertex entry points.
    std::vector<uint32_t> entry_fns, interface_ids;
    for (auto& I : insts) {
        if (I.op != OpEntryPoint || w(I, 1) != ExecModelVertex) continue;
        entry_fns.push_back(w(I, 2));
        uint32_t k = 3;  // skip the nul-terminated name literal
        for (; k < I.wc; ++k) {
            uint32_t word = in[I.off + k];
            if ((word & 0xff000000u) == 0 || (word & 0xff0000u) == 0 || (word & 0xff00u) == 0 || (word & 0xffu) == 0) { ++k; break; }
        }
        for (; k < I.wc; ++k) interface_ids.push_back(in[I.off + k]);
    }
    if (entry_fns.empty()) { r.why = "no vertex entry point"; return r; }

    // 2. Existing types, decorations, variables, module header state.
    uint32_t t_float = 0, t_vec4 = 0, t_bool = 0, t_int = 0, t_uint = 0, t_uvec2 = 0, glsl = 0;
    uint32_t pos_var = 0, pos_struct = 0, pos_member = 0, addressing = AddrLogical;
    bool has_psb_cap = false, has_psb_ext = false;
    std::vector<std::pair<uint32_t, uint32_t>> ptr_types;  // (ptr id, pointee) Output only
    std::vector<std::pair<uint32_t, uint32_t>> out_vars;   // (var id, ptr type)
    std::vector<uint32_t> builtin_pos_vars;
    for (auto& I : insts) {
        switch (I.op) {
        case OpCapability: if (w(I, 1) == CapPhysicalStorageBufferAddresses) has_psb_cap = true; break;
        case OpExtension: if (word_str(&in[I.off + 1], I.wc - 1) == "SPV_KHR_physical_storage_buffer") has_psb_ext = true; break;
        case OpExtInstImport: if (word_str(&in[I.off + 2], I.wc - 2) == "GLSL.std.450") glsl = w(I, 1); break;
        case OpMemoryModel: addressing = w(I, 1); break;
        case OpTypeFloat: if (w(I, 2) == 32 && I.wc == 3 && !t_float) t_float = w(I, 1); break;
        case OpTypeBool: t_bool = w(I, 1); break;
        case OpTypeInt:
            if (w(I, 2) == 32 && w(I, 3) == 1 && !t_int) t_int = w(I, 1);
            if (w(I, 2) == 32 && w(I, 3) == 0 && !t_uint) t_uint = w(I, 1);
            break;
        case OpTypePointer: if (w(I, 2) == StorageOutput) ptr_types.push_back({w(I, 1), w(I, 3)}); break;
        case OpVariable: if (w(I, 3) == StorageOutput) out_vars.push_back({w(I, 2), w(I, 1)}); break;
        case OpDecorate:
            if (I.wc >= 4 && w(I, 2) == DecBuiltIn && w(I, 3) == BuiltInPosition) builtin_pos_vars.push_back(w(I, 1));
            break;
        case OpMemberDecorate:
            if (I.wc >= 5 && w(I, 3) == DecBuiltIn && w(I, 4) == BuiltInPosition) { pos_struct = w(I, 1); pos_member = w(I, 2); }
            break;
        }
    }
    if (!t_float) { r.why = "no float32 type"; return r; }
    for (auto& I : insts) {
        if (I.op != OpTypeVector) continue;
        if (w(I, 2) == t_float && w(I, 3) == 4 && !t_vec4) t_vec4 = w(I, 1);
        if (t_uint && w(I, 2) == t_uint && w(I, 3) == 2 && !t_uvec2) t_uvec2 = w(I, 1);
    }
    if (!t_vec4) { r.why = "no vec4 type"; return r; }
    bool live = o.addr != 0;
    if (live && addressing != AddrLogical && addressing != AddrPhysicalStorageBuffer64) {
        r.why = "unsupported addressing model for live mode";
        return r;
    }

    auto in_interface = [&](uint32_t id) {
        for (auto x : interface_ids) if (x == id) return true;
        return false;
    };
    auto pointee = [&](uint32_t ptr) -> uint32_t {
        for (auto& p : ptr_types) if (p.first == ptr) return p.second;
        return 0;
    };
    for (auto v : builtin_pos_vars)
        for (auto& ov : out_vars) if (ov.first == v && in_interface(v)) { pos_var = v; pos_struct = 0; }
    if (!pos_var && pos_struct)
        for (auto& ov : out_vars)
            if (pointee(ov.second) == pos_struct && in_interface(ov.first)) { pos_var = ov.first; break; }
    if (!pos_var) { r.why = "vertex shader does not write gl_Position (or it is arrayed)"; return r; }

    // 3. New ids and global declarations.
    uint32_t bound = in[3];
    auto fresh = [&]() { return bound++; };
    auto emit = [](std::vector<uint32_t>& out, uint32_t op, std::initializer_list<uint32_t> ops) {
        out.push_back(uint32_t(ops.size() + 1) << 16 | op);
        out.insert(out.end(), ops);
    };
    auto emit_v = [](std::vector<uint32_t>& out, uint32_t op, const std::vector<uint32_t>& ops) {
        out.push_back(uint32_t(ops.size() + 1) << 16 | op);
        out.insert(out.end(), ops.begin(), ops.end());
    };
    std::vector<uint32_t> head_cap, head_ext, head_imp, globals;
    if (!glsl) {
        glsl = fresh();
        std::vector<uint32_t> ops{glsl};
        auto s = str_words("GLSL.std.450");
        ops.insert(ops.end(), s.begin(), s.end());
        emit_v(head_imp, OpExtInstImport, ops);
    }
    if (live && !has_psb_cap) emit(head_cap, OpCapability, {CapPhysicalStorageBufferAddresses});
    if (live && !has_psb_ext) emit_v(head_ext, OpExtension, str_words("SPV_KHR_physical_storage_buffer"));
    if (!t_bool && o.skip_w1) { t_bool = fresh(); emit(globals, OpTypeBool, {t_bool}); }
    uint32_t p_vec4 = 0;
    for (auto& p : ptr_types) if (p.second == t_vec4) { p_vec4 = p.first; break; }
    if (!p_vec4) { p_vec4 = fresh(); emit(globals, OpTypePointer, {p_vec4, StorageOutput, t_vec4}); }
    uint32_t c_member = 0;
    if (pos_struct) {
        if (!t_int) { t_int = fresh(); emit(globals, OpTypeInt, {t_int, 32, 1}); }
        c_member = fresh(); emit(globals, OpConstant, {t_int, c_member, pos_member});
    }
    uint32_t c_zero = fresh(); emit(globals, OpConstant, {t_float, c_zero, f2u(0.0f)});
    uint32_t c_one = fresh(); emit(globals, OpConstant, {t_float, c_one, f2u(1.0f)});
    uint32_t c_params = 0, c_addr = 0, p_psb = 0;
    if (live) {
        if (!t_uint) { t_uint = fresh(); emit(globals, OpTypeInt, {t_uint, 32, 0}); }
        if (!t_uvec2) { t_uvec2 = fresh(); emit(globals, OpTypeVector, {t_uvec2, t_uint, 2}); }
        uint32_t lo = fresh(), hi = fresh();
        emit(globals, OpConstant, {t_uint, lo, uint32_t(o.addr)});
        emit(globals, OpConstant, {t_uint, hi, uint32_t(o.addr >> 32)});
        c_addr = fresh(); emit(globals, OpConstantComposite, {t_uvec2, c_addr, lo, hi});
        p_psb = fresh(); emit(globals, OpTypePointer, {p_psb, StoragePhysicalStorageBuffer, t_vec4});
    } else {
        uint32_t c[4];
        for (int k = 0; k < 4; ++k) { c[k] = fresh(); emit(globals, OpConstant, {t_float, c[k], f2u(o.params[k])}); }
        c_params = fresh(); emit(globals, OpConstantComposite, {t_vec4, c_params, c[0], c[1], c[2], c[3]});
    }

    // 4. Rebuild the module, inserting each piece in its required section.
    std::vector<uint32_t> out(in.begin(), in.begin() + 5);
    out.reserve(in.size() + 128);
    bool cap_done = false, ext_done = false, globals_done = false, in_entry = false;
    int returns = 0;
    for (auto& I : insts) {
        if (!cap_done && I.op != OpCapability) { out.insert(out.end(), head_cap.begin(), head_cap.end()); cap_done = true; }
        if (!ext_done && I.op != OpCapability && I.op != OpExtension) { out.insert(out.end(), head_ext.begin(), head_ext.end()); ext_done = true; }
        if (I.op == OpMemoryModel) {
            out.insert(out.end(), head_imp.begin(), head_imp.end());
            out.push_back(in[I.off]);
            out.push_back(live ? AddrPhysicalStorageBuffer64 : w(I, 1));
            out.insert(out.end(), in.begin() + I.off + 2, in.begin() + I.off + I.wc);
            continue;
        }
        if (I.op == OpFunction && !globals_done) { out.insert(out.end(), globals.begin(), globals.end()); globals_done = true; }
        if (I.op == OpFunction) {
            in_entry = false;
            for (auto f : entry_fns) if (w(I, 2) == f) in_entry = true;
        }
        if (in_entry && I.op == OpReturn) {
            uint32_t ptr = pos_var;
            if (pos_struct) { ptr = fresh(); emit(out, OpAccessChain, {p_vec4, ptr, pos_var, c_member}); }
            uint32_t prm = c_params;
            if (live) {
                uint32_t pp = fresh();
                prm = fresh();
                emit(out, OpBitcast, {p_psb, pp, c_addr});
                emit(out, OpLoad, {t_vec4, prm, pp, MemAccessAligned, 16});
            }
            uint32_t v = fresh(), x = fresh(), wv = fresh(), a = fresh(), b = fresh(), wf = fresh(), inv = fresh();
            uint32_t t0 = fresh(), t1 = fresh(), t = fresh(), d = fresh(), e = fresh(), off = fresh(), x2 = fresh(), v2 = fresh();
            emit(out, OpLoad, {t_vec4, v, ptr});
            emit(out, OpCompositeExtract, {t_float, x, v, 0});
            emit(out, OpCompositeExtract, {t_float, wv, v, 3});
            emit(out, OpCompositeExtract, {t_float, a, prm, 0});
            emit(out, OpCompositeExtract, {t_float, b, prm, 1});
            emit(out, OpCompositeExtract, {t_float, wf, prm, 2});
            emit(out, OpCompositeExtract, {t_float, inv, prm, 3});
            emit(out, OpFSub, {t_float, t0, wf, wv});
            emit(out, OpFMul, {t_float, t1, t0, inv});
            emit(out, OpExtInst, {t_float, t, glsl, GLSLFClamp, t1, c_zero, c_one});
            emit(out, OpFSub, {t_float, d, b, a});
            emit(out, OpFMul, {t_float, e, d, t});
            emit(out, OpFAdd, {t_float, off, a, e});
            emit(out, OpFAdd, {t_float, x2, x, off});
            if (o.skip_w1) {
                uint32_t is1 = fresh(), xs = fresh();
                emit(out, OpFOrdEqual, {t_bool, is1, wv, c_one});
                emit(out, OpSelect, {t_float, xs, is1, x, x2});
                x2 = xs;
            }
            emit(out, OpCompositeInsert, {t_vec4, v2, x2, v, 0});
            emit(out, OpStore, {ptr, v2});
            ++returns;
        }
        if (I.op == OpFunctionEnd) in_entry = false;
        out.insert(out.end(), in.begin() + I.off, in.begin() + I.off + I.wc);
    }
    if (!returns) { r.why = "no OpReturn in vertex entry point"; return r; }
    out[3] = bound;
    in.swap(out);
    r.patched = true;
    return r;
}

}  // namespace spv
