#version 450
// --geom: passthrough geometry stage (emits the incoming triangle).
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec3 c[];
layout(location = 0) out vec3 v_color;
void main() {
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        v_color = c[i];
        EmitVertex();
    }
    EndPrimitive();
}
