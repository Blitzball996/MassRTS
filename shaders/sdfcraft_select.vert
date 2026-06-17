#version 330 core
layout(location=0) in vec3 a_pos;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec3 u_block;   // integer block origin
void main() {
    // a_pos is a unit-cube corner in [0,1]; slight inflate to avoid z-fighting
    vec3 p = u_block + a_pos * 1.002 - vec3(0.001);
    gl_Position = u_proj * u_view * vec4(p, 1.0);
}
