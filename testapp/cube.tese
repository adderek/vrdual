#version 450
// --tess: position interpolated from the vertex shader's gl_Position (already
// clip space), so a shift in both stages would show as a doubled disparity.
layout(triangles, equal_spacing, ccw) in;
layout(location = 0) in vec3 c[];
layout(location = 0) out vec3 v_color;
void main() {
    vec3 b = gl_TessCoord;
    gl_Position = b.x * gl_in[0].gl_Position + b.y * gl_in[1].gl_Position + b.z * gl_in[2].gl_Position;
    v_color = b.x * c[0] + b.y * c[1] + b.z * c[2];
}
