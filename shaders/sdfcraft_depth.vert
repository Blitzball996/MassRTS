#version 330 core
// SDFCraft — shadow depth pass: render chunk geometry from the sun's POV.
// Reuses the same 10-float vertex layout as the main chunk shader; only the
// world position matters here (depth-only), the other attribs are ignored.
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec3 a_color;
layout(location=3) in float a_mat;

uniform mat4 u_light_vp;   // light-space view*projection

void main() {
    gl_Position = u_light_vp * vec4(a_pos, 1.0);
}
