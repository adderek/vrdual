// HLSL-style vertex shader (DXVK/VKD3D shape): SV_Position is a plain Output
// variable, not a gl_PerVertex block member, and there are two return paths.
struct VSOut { float4 pos : SV_Position; float3 c : COLOR; };
cbuffer CB : register(b0) { float4x4 mvp; };
VSOut main(float3 p : POSITION) {
    VSOut o;
    o.pos = mul(mvp, float4(p, 1));
    o.c = p;
    if (p.x > 0) return o;
    o.c = 1;
    return o;
}
