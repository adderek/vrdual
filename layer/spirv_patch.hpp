// Minimal SPIR-V rewriter: shifts clip-space X of the last pre-raster stage's
// output (vertex, tessellation evaluation or geometry shader).
//
// Before every OpReturn of each Vertex / TessellationEvaluation entry point
// (Geometry: before every OpEmitVertex / OpEmitStreamVertex) we append:
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
// writing host-visible memory — no pipeline rebuild, app stays unaware. It also
// reads a second vec4 h at addr+16 (head offset, vrdual_ctl.hpp EyeParams):
//     (x, y, w)_t = (v.x + h.x, v.y + h.y, v.w + h.z)
//     (x, y, w)'  = M * (x, y, w)_t,   x' += off,   z' = v.z * w' / v.w
// (z' = v.z if v.w == 0 or w' == v.w)
// M = rows at addr+48/64/80, the head rotation (identity without one).
// (z scaled with w keeps every vertex's depth z/w: no far-plane clipping,
// depth order kept; same w == 1 skip). w == 1 vertices (HUD) get x' = v.x + h.w
// instead: a constant disparity that puts the HUD at hud_depth.
//
// Shader rules (docs/tasks/shader-rules.md): `hud` gives every vertex the
// w == 1 treatment, `hide` stores (0,0,0,0) (nothing drawn). Live mode also
// reads a uvec4 (lo, hi, mode, 0) at addr+32 and, if lo <= key <= hi (key =
// high 32 bits of the shader's hash), applies mode 1 hide | 2 noshift | 3 hud
// at runtime: the hunt for a shader's key.
//
// Gate: the result is selected against the original by a specialization
// constant (SpecId kGateSpecId, default true). A pipeline with TES or GS sets
// it false for the stages before the last one: their output is not clip space
// yet (the TES or GS would shift it a second time). The layer does that in
// vkCreateGraphicsPipelines, since vkCreateShaderModule knows no pipeline.
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
    OpCompositeInsert = 82, OpBitcast = 124, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136,
    OpCompositeConstruct = 80, OpConstantTrue = 41, OpLogicalOr = 166, OpLogicalAnd = 167, OpIEqual = 170,
    OpUGreaterThanEqual = 174, OpULessThanEqual = 178,
    OpSelect = 169, OpFOrdEqual = 180, OpReturn = 253, OpEmitVertex = 218, OpEmitStreamVertex = 220,
    OpSpecConstantTrue = 48, OpDecorateId = 332, OpDecorateString = 5632, OpMemberDecorateString = 5633,
    OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75,
};
enum : uint32_t {
    ExecModelVertex = 0, ExecModelTessEval = 2, ExecModelGeometry = 3, StorageOutput = 3, DecSpecId = 1, StoragePhysicalStorageBuffer = 5349, DecBuiltIn = 11,
    BuiltInPosition = 0, AddrLogical = 0, AddrPhysicalStorageBuffer64 = 5348,
    CapPhysicalStorageBufferAddresses = 5347, MemAccessAligned = 2, GLSLFClamp = 43,
};

struct PatchOptions {
    float params[4] = {0, 0, 0, 0};  // baked params (used when addr == 0)
    uint64_t addr = 0;               // live: device address of a vec4 (16-byte aligned)
    bool skip_w1 = true;
    bool hud = false;   // rule: every vertex like w == 1 (HUD at hud_depth)
    bool w1_hud = true; // w == 1 vertices get the hud_depth shift (false: unshifted, hud_depth_scope = rules)
    bool hide = false;  // rule: draw nothing
    uint32_t key = 0;   // live hunt: high 32 bits of the shader hash
};

constexpr uint32_t kGateSpecId = 0x76726431;  // 'vrd1': spec constant gating the patch
inline const char* const kNoEntry = "no vertex, tessellation evaluation or geometry entry point";

// Shader key: FNV-1a 64 over the SPIR-V bytes as the app passed them.
inline uint64_t shader_hash(const uint32_t* code, size_t words) {
    uint64_t h = 0xcbf29ce484222325ull;
    const unsigned char* b = (const unsigned char*)code;
    for (size_t i = 0; i < words * 4; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

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

// Fewest words an instruction the patcher reads must have (its fixed operands),
// so a malformed module never reads past an instruction or the buffer.
inline uint32_t min_wc(uint32_t op) {
    switch (op) {
    case OpCapability: case OpExtension: case OpTypeBool: return 2;
    case OpExtInstImport: case OpMemoryModel: case OpTypeFloat: case OpDecorate: return 3;
    case OpEntryPoint: case OpTypeInt: case OpTypePointer: case OpVariable: case OpMemberDecorate: case OpTypeVector: return 4;
    case OpFunction: return 5;
    default: return 1;
    }
}

inline PatchResult patch_vertex(std::vector<uint32_t>& in, const PatchOptions& o) {
    PatchResult r;
    if (in.size() < 5 || in[0] != 0x07230203u) { r.why = "not SPIR-V (or wrong endianness)"; return r; }

    struct Inst { size_t off; uint32_t op, wc; };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (wc == 0 || i + wc > in.size() || wc < min_wc(op)) { r.why = "malformed instruction stream"; return r; }
        insts.push_back({i, op, wc});
        i += wc;
    }
    auto w = [&](const Inst& I, uint32_t k) { return in[I.off + k]; };

    // 1. Vertex / tessellation evaluation / geometry entry points.
    std::vector<uint32_t> entry_fns, interface_ids;
    bool geom = false;
    for (auto& I : insts) {
        if (I.op != OpEntryPoint) continue;
        uint32_t model = w(I, 1);
        if (model != ExecModelVertex && model != ExecModelTessEval && model != ExecModelGeometry) continue;
        if (model == ExecModelGeometry) geom = true;
        else entry_fns.push_back(w(I, 2));
        uint32_t k = 3;  // skip the nul-terminated name literal
        for (; k < I.wc; ++k) {
            uint32_t word = in[I.off + k];
            if ((word & 0xff000000u) == 0 || (word & 0xff0000u) == 0 || (word & 0xff00u) == 0 || (word & 0xffu) == 0) { ++k; break; }
        }
        for (; k < I.wc; ++k) interface_ids.push_back(in[I.off + k]);
    }
    if (entry_fns.empty() && !geom) { r.why = kNoEntry; return r; }

    // 2. Existing types, decorations, variables, module header state.
    uint32_t t_float = 0, t_vec4 = 0, t_bool = 0, t_int = 0, t_uint = 0, t_uvec2 = 0, t_uvec4 = 0, glsl = 0;
    uint32_t pos_var = 0, pos_struct = 0, pos_member = 0, addressing = AddrLogical;
    std::vector<std::pair<uint32_t, uint32_t>> pos_structs;  // (struct, member): output and input blocks (TES/GS gl_in)
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
            if (I.wc >= 5 && w(I, 3) == DecBuiltIn && w(I, 4) == BuiltInPosition) pos_structs.push_back({w(I, 1), w(I, 2)});
            break;
        }
    }
    if (!t_float) { r.why = "no float32 type"; return r; }
    for (auto& I : insts) {
        if (I.op != OpTypeVector) continue;
        if (w(I, 2) == t_float && w(I, 3) == 4 && !t_vec4) t_vec4 = w(I, 1);
        if (t_uint && w(I, 2) == t_uint && w(I, 3) == 2 && !t_uvec2) t_uvec2 = w(I, 1);
        if (t_uint && w(I, 2) == t_uint && w(I, 3) == 4 && !t_uvec4) t_uvec4 = w(I, 1);
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
        for (auto& ov : out_vars) if (ov.first == v && in_interface(v)) pos_var = v;
    for (auto& ps : pos_structs)
        for (auto& ov : out_vars)
            if (!pos_var && pointee(ov.second) == ps.first && in_interface(ov.first)) { pos_var = ov.first; pos_struct = ps.first; pos_member = ps.second; }
    if (!pos_var) { r.why = "shader does not write gl_Position (or it is arrayed)"; return r; }

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
    std::vector<uint32_t> head_cap, head_ext, head_imp, annot, globals;
    if (!glsl) {
        glsl = fresh();
        std::vector<uint32_t> ops{glsl};
        auto s = str_words("GLSL.std.450");
        ops.insert(ops.end(), s.begin(), s.end());
        emit_v(head_imp, OpExtInstImport, ops);
    }
    if (live && !has_psb_cap) emit(head_cap, OpCapability, {CapPhysicalStorageBufferAddresses});
    if (live && !has_psb_ext) emit_v(head_ext, OpExtension, str_words("SPV_KHR_physical_storage_buffer"));
    bool hudpath = o.skip_w1 || o.hud;
    if (!t_bool) { t_bool = fresh(); emit(globals, OpTypeBool, {t_bool}); }
    uint32_t c_gate = fresh();
    emit(globals, OpSpecConstantTrue, {t_bool, c_gate});
    emit(annot, OpDecorate, {c_gate, DecSpecId, kGateSpecId});
    uint32_t c_true = 0;
    if (o.hud) { c_true = fresh(); emit(globals, OpConstantTrue, {t_bool, c_true}); }
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
    uint32_t c_zero4 = fresh(); emit(globals, OpConstantComposite, {t_vec4, c_zero4, c_zero, c_zero, c_zero, c_zero});
    uint32_t c_params = 0, c_addr = 0, c_addr2 = 0, p_psb = 0, c_addr3 = 0, p_psb4 = 0, c_key = 0, c_m[4] = {}, c_rot[3] = {};
    if (live) {
        if (!t_uint) { t_uint = fresh(); emit(globals, OpTypeInt, {t_uint, 32, 0}); }
        if (!t_uvec2) { t_uvec2 = fresh(); emit(globals, OpTypeVector, {t_uvec2, t_uint, 2}); }
        uint32_t lo = fresh(), hi = fresh();
        emit(globals, OpConstant, {t_uint, lo, uint32_t(o.addr)});
        emit(globals, OpConstant, {t_uint, hi, uint32_t(o.addr >> 32)});
        c_addr = fresh(); emit(globals, OpConstantComposite, {t_uvec2, c_addr, lo, hi});
        uint32_t lo2 = fresh(), hi2 = fresh();
        emit(globals, OpConstant, {t_uint, lo2, uint32_t(o.addr + 16)});
        emit(globals, OpConstant, {t_uint, hi2, uint32_t((o.addr + 16) >> 32)});
        c_addr2 = fresh(); emit(globals, OpConstantComposite, {t_uvec2, c_addr2, lo2, hi2});
        p_psb = fresh(); emit(globals, OpTypePointer, {p_psb, StoragePhysicalStorageBuffer, t_vec4});
        if (!t_uvec4) { t_uvec4 = fresh(); emit(globals, OpTypeVector, {t_uvec4, t_uint, 4}); }
        uint32_t lo3 = fresh(), hi3 = fresh();
        emit(globals, OpConstant, {t_uint, lo3, uint32_t(o.addr + 32)});
        emit(globals, OpConstant, {t_uint, hi3, uint32_t((o.addr + 32) >> 32)});
        c_addr3 = fresh(); emit(globals, OpConstantComposite, {t_uvec2, c_addr3, lo3, hi3});
        p_psb4 = fresh(); emit(globals, OpTypePointer, {p_psb4, StoragePhysicalStorageBuffer, t_uvec4});
        c_key = fresh(); emit(globals, OpConstant, {t_uint, c_key, o.key});
        for (int k = 0; k < 3; ++k) {  // rotation rows
            uint64_t a = o.addr + 48 + 16 * k;
            uint32_t rl = fresh(), rh = fresh();
            emit(globals, OpConstant, {t_uint, rl, uint32_t(a)});
            emit(globals, OpConstant, {t_uint, rh, uint32_t(a >> 32)});
            c_rot[k] = fresh(); emit(globals, OpConstantComposite, {t_uvec2, c_rot[k], rl, rh});
        }
        for (uint32_t k = 1; k < 4; ++k) { c_m[k] = fresh(); emit(globals, OpConstant, {t_uint, c_m[k], k}); }
    } else {
        uint32_t c[4];
        for (int k = 0; k < 4; ++k) { c[k] = fresh(); emit(globals, OpConstant, {t_float, c[k], f2u(o.params[k])}); }
        c_params = fresh(); emit(globals, OpConstantComposite, {t_vec4, c_params, c[0], c[1], c[2], c[3]});
    }

    // 4. Rebuild the module, inserting each piece in its required section.
    std::vector<uint32_t> out(in.begin(), in.begin() + 5);
    out.reserve(in.size() + 128);
    bool cap_done = false, ext_done = false, annot_done = false, globals_done = false, in_entry = false;
    int returns = 0;
    auto sel4 = [&](uint32_t cond, uint32_t a, uint32_t b) {  // vector OpSelect with a scalar condition needs SPIR-V 1.4
        uint32_t c[4];
        for (uint32_t k = 0; k < 4; ++k) {
            uint32_t ca = fresh(), cb = fresh();
            c[k] = fresh();
            emit(out, OpCompositeExtract, {t_float, ca, a, k});
            emit(out, OpCompositeExtract, {t_float, cb, b, k});
            emit(out, OpSelect, {t_float, c[k], cond, ca, cb});
        }
        uint32_t r4 = fresh();
        emit(out, OpCompositeConstruct, {t_vec4, r4, c[0], c[1], c[2], c[3]});
        return r4;
    };
    for (auto& I : insts) {
        bool is_annot = I.op == OpDecorate || I.op == OpMemberDecorate || I.op == OpDecorateId || I.op == OpDecorateString ||
                        I.op == OpMemberDecorateString || I.op == OpDecorationGroup || I.op == OpGroupDecorate || I.op == OpGroupMemberDecorate;
        if (!annot_done && is_annot) { out.insert(out.end(), annot.begin(), annot.end()); annot_done = true; }
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
        if ((in_entry && I.op == OpReturn) || (geom && (I.op == OpEmitVertex || I.op == OpEmitStreamVertex))) {
            uint32_t ptr = pos_var;
            if (pos_struct) { ptr = fresh(); emit(out, OpAccessChain, {p_vec4, ptr, pos_var, c_member}); }
            if (o.hide) {
                uint32_t v0 = fresh();
                emit(out, OpLoad, {t_vec4, v0, ptr});
                emit(out, OpStore, {ptr, sel4(c_gate, c_zero4, v0)});
                ++returns;
                out.insert(out.end(), in.begin() + I.off, in.begin() + I.off + I.wc);
                continue;
            }
            uint32_t prm = c_params, hp = 0;
            if (live) {
                uint32_t pp = fresh(), pp2 = fresh();
                prm = fresh();
                hp = fresh();
                emit(out, OpBitcast, {p_psb, pp, c_addr});
                emit(out, OpLoad, {t_vec4, prm, pp, MemAccessAligned, 16});
                emit(out, OpBitcast, {p_psb, pp2, c_addr2});
                emit(out, OpLoad, {t_vec4, hp, pp2, MemAccessAligned, 16});
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
            uint32_t y = 0, y2 = 0, w2 = 0, z = 0, z2 = 0;
            if (!live) emit(out, OpFAdd, {t_float, x2, x, off});
            if (live) {
                uint32_t hx = fresh(), hy = fresh(), hw = fresh(), xt = fresh(), yt = fresh(), wt = fresh(), zr = fresh();
                y = fresh(); y2 = fresh(); w2 = fresh(); z = fresh(); z2 = fresh();
                emit(out, OpCompositeExtract, {t_float, y, v, 1});
                emit(out, OpCompositeExtract, {t_float, z, v, 2});
                emit(out, OpCompositeExtract, {t_float, hx, hp, 0});
                emit(out, OpCompositeExtract, {t_float, hy, hp, 1});
                emit(out, OpCompositeExtract, {t_float, hw, hp, 2});
                emit(out, OpFAdd, {t_float, xt, x, hx});
                emit(out, OpFAdd, {t_float, yt, y, hy});
                emit(out, OpFAdd, {t_float, wt, wv, hw});
                // rotation: row k . (xt, yt, wt); identity rows give the input exactly
                uint32_t rr[3];
                for (int k = 0; k < 3; ++k) {
                    uint32_t rp = fresh(), row = fresh(), m0 = fresh(), m1 = fresh(), m2 = fresh();
                    uint32_t p0 = fresh(), p1 = fresh(), p2 = fresh(), s0 = fresh();
                    emit(out, OpBitcast, {p_psb, rp, c_rot[k]});
                    emit(out, OpLoad, {t_vec4, row, rp, MemAccessAligned, 16});
                    emit(out, OpCompositeExtract, {t_float, m0, row, 0});
                    emit(out, OpCompositeExtract, {t_float, m1, row, 1});
                    emit(out, OpCompositeExtract, {t_float, m2, row, 2});
                    emit(out, OpFMul, {t_float, p0, m0, xt});
                    emit(out, OpFMul, {t_float, p1, m1, yt});
                    emit(out, OpFMul, {t_float, p2, m2, wt});
                    emit(out, OpFAdd, {t_float, s0, p0, p1});
                    rr[k] = fresh();
                    emit(out, OpFAdd, {t_float, rr[k], s0, p2});
                }
                emit(out, OpFAdd, {t_float, x2, rr[0], off});
                y2 = rr[1];
                w2 = rr[2];
                // w == 0 (a triangle crossing the camera plane) would give
                // 0/0; an unchanged w keeps z bit-exact (depth prepass
                // shaders without the patch, rule noshift, must still match).
                uint32_t zm = fresh(), w0 = fresh(), h0 = fresh(), keep = fresh();
                emit(out, OpFDiv, {t_float, zr, w2, wv});
                emit(out, OpFMul, {t_float, zm, z, zr});
                emit(out, OpFOrdEqual, {t_bool, w0, wv, c_zero});
                emit(out, OpFOrdEqual, {t_bool, h0, w2, wv});
                emit(out, OpLogicalOr, {t_bool, keep, w0, h0});
                emit(out, OpSelect, {t_float, z2, keep, z, zm});
            }
            uint32_t xh = x;  // x of the HUD treatment
            if (live) {  // HUD at hud_depth: constant shift h.w
                uint32_t hh = fresh();
                xh = fresh();
                emit(out, OpCompositeExtract, {t_float, hh, hp, 3});
                emit(out, OpFAdd, {t_float, xh, x, hh});
            }
            if (hudpath) {
                uint32_t is1 = c_true, xs = fresh();
                if (!o.hud) { is1 = fresh(); emit(out, OpFOrdEqual, {t_bool, is1, wv, c_one}); }
                emit(out, OpSelect, {t_float, xs, is1, o.w1_hud || o.hud ? xh : x, x2});
                x2 = xs;
                if (live) {
                    uint32_t ys = fresh(), ws = fresh(), zs = fresh();
                    emit(out, OpSelect, {t_float, ys, is1, y, y2});
                    emit(out, OpSelect, {t_float, ws, is1, wv, w2});
                    emit(out, OpSelect, {t_float, zs, is1, z, z2});
                    y2 = ys;
                    w2 = ws;
                    z2 = zs;
                }
            }
            emit(out, OpCompositeInsert, {t_vec4, v2, x2, v, 0});
            if (live) {
                uint32_t v3 = fresh(), v4 = fresh(), v5 = fresh();
                emit(out, OpCompositeInsert, {t_vec4, v3, y2, v2, 1});
                emit(out, OpCompositeInsert, {t_vec4, v4, z2, v3, 2});
                emit(out, OpCompositeInsert, {t_vec4, v5, w2, v4, 3});
                v2 = v5;
                // Hunt: res = in && mode 1 ? 0 : in && mode 2 ? v : in && mode 3 ? hud(v) : v2.
                // Per-component selects: a vector OpSelect with a scalar
                // condition needs SPIR-V 1.4.
                uint32_t hq = fresh(), hv = fresh(), lo = fresh(), hi = fresh(), md = fresh(), ge = fresh(), le = fresh(), inr = fresh();
                emit(out, OpBitcast, {p_psb4, hq, c_addr3});
                emit(out, OpLoad, {t_uvec4, hv, hq, MemAccessAligned, 16});
                emit(out, OpCompositeExtract, {t_uint, lo, hv, 0});
                emit(out, OpCompositeExtract, {t_uint, hi, hv, 1});
                emit(out, OpCompositeExtract, {t_uint, md, hv, 2});
                emit(out, OpUGreaterThanEqual, {t_bool, ge, c_key, lo});
                emit(out, OpULessThanEqual, {t_bool, le, c_key, hi});
                emit(out, OpLogicalAnd, {t_bool, inr, ge, le});
                uint32_t vh = fresh();
                emit(out, OpCompositeInsert, {t_vec4, vh, xh, v, 0});
                uint32_t res = v2;
                const uint32_t alt[4] = {0, c_zero4, v, vh};
                for (uint32_t k = 3; k >= 1; --k) {
                    uint32_t is = fresh(), cnd = fresh();
                    emit(out, OpIEqual, {t_bool, is, md, c_m[k]});
                    emit(out, OpLogicalAnd, {t_bool, cnd, inr, is});
                    res = sel4(cnd, alt[k], res);
                }
                v2 = res;
            }
            emit(out, OpStore, {ptr, sel4(c_gate, v2, v)});
            ++returns;
        }
        if (I.op == OpFunctionEnd) in_entry = false;
        out.insert(out.end(), in.begin() + I.off, in.begin() + I.off + I.wc);
    }
    if (!returns) { r.why = geom ? "no OpEmitVertex in geometry shader" : "no OpReturn in entry point"; return r; }
    if (!annot_done) { r.why = "no decorations"; return r; }  // unreachable: gl_Position is decorated
    out[3] = bound;
    in.swap(out);
    r.patched = true;
    return r;
}

}  // namespace spv
