#version 450
// --tess: passthrough control stage (one triangle per patch, no subdivision)
layout(vertices = 3) out;
layout(location = 0) in vec3 c_in[];
layout(location = 0) out vec3 c_out[];
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    c_out[gl_InvocationID] = c_in[gl_InvocationID];
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = 1.0;
        gl_TessLevelInner[0] = 1.0;
    }
}
