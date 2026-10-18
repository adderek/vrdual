#version 450
// --bda variant of cube.vert: vertices come from a buffer device address in the
// push constants instead of vertex input (how DXVK 3 / modern engines fetch data).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430) readonly buffer Verts { float v[]; };
layout(push_constant) uniform PC { mat4 mvp; vec4 color; Verts verts; } pc;
layout(location = 0) out vec3 v_color;
void main() {
    int b = gl_VertexIndex * 6;
    vec3 in_pos = vec3(pc.verts.v[b], pc.verts.v[b + 1], pc.verts.v[b + 2]);
    vec3 in_nrm = vec3(pc.verts.v[b + 3], pc.verts.v[b + 4], pc.verts.v[b + 5]);
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    float l = 0.35 + 0.65 * max(dot(normalize(in_nrm), normalize(vec3(0.4, 0.8, 0.5))), 0.0);
    v_color = pc.color.rgb * l;
}
