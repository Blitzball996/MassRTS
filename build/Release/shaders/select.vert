#version 330 core
layout(location = 0) in vec2 a_vertex;
uniform vec4 u_box;
out vec2 v_uv;

void main() {
    vec2 pos = mix(u_box.xy, u_box.zw, a_vertex * 0.5 + 0.5);
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv = a_vertex * 0.5 + 0.5;
}
