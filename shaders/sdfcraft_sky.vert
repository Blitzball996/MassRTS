#version 330 core
// Stylized sky dome (Genshin-style gradient + volumetric clouds)
layout(location=0) in vec3 a_pos;
uniform mat4 u_view;
uniform mat4 u_proj;
out vec3 v_dir;

void main() {
    v_dir = a_pos;
    // Remove translation from view matrix (keep rotation only)
    mat4 view_no_trans = u_view;
    view_no_trans[3] = vec4(0, 0, 0, 1);
    vec4 pos = u_proj * view_no_trans * vec4(a_pos, 1.0);
    gl_Position = pos.xyww; // force far plane
}
