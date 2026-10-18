#version 450
layout(push_constant) uniform PC { mat4 mvp; vec4 color; } pc;
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_nrm;
layout(location = 0) out vec3 v_color;
void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    float l = 0.35 + 0.65 * max(dot(normalize(in_nrm), normalize(vec3(0.4, 0.8, 0.5))), 0.0);
    v_color = pc.color.rgb * l;
}
