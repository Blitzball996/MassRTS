#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec3 a_color;
layout(location=3) in float a_mat;

uniform mat4 u_view;
uniform mat4 u_proj;

out vec3 v_color;
out vec3 v_normal;
out vec3 v_world;
out float v_mat;

void main() {
    v_color = a_color;
    v_normal = a_normal;
    v_world = a_pos;
    v_mat = a_mat;
    gl_Position = u_proj * u_view * vec4(a_pos, 1.0);
}
