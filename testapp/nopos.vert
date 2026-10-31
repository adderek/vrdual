#version 450
// --nopos-vs: a vertex shader without gl_Position (feeds tessellation, like
// HairWorks'): the layer leaves it unpatched; created, never drawn.
layout(location = 0) in vec3 in_pos;
layout(location = 0) out vec3 v_pos;
void main() { v_pos = in_pos; }
