#version 330 core
layout(location=0) in vec2 a_pos;
// Per instance
layout(location=2) in vec3 a_inst_pos;
layout(location=3) in vec3 a_inst_color;
layout(location=4) in vec3 a_inst_dir;
layout(location=5) in float a_inst_size;
layout(location=6) in float a_inst_stretch;

uniform mat4 u_view;
uniform mat4 u_proj;

out vec3 v_color;
out vec2 v_quad;     // local quad coord in [-1,1], for sphere impostor
out float v_stretch; // >2 = arrow (flat), else sphere impostor

void main() {
    v_color = a_inst_color;
    v_quad = a_pos * 2.0;   // a_pos [-0.5,0.5] -> [-1,1]
    v_stretch = a_inst_stretch;

    vec3 cam_right = vec3(u_view[0][0], u_view[1][0], u_view[2][0]);
    vec3 cam_up    = vec3(u_view[0][1], u_view[1][1], u_view[2][1]);

    vec3 offset;
    if (a_inst_stretch > 2.0) {
        // Arrow: stretch along velocity, thin across.
        vec3 dir = normalize(a_inst_dir);
        vec3 right = normalize(cross(dir, cam_up));
        float w = a_inst_size;
        float h = a_inst_size * a_inst_stretch;
        offset = right * a_pos.x * w + dir * a_pos.y * h;
    } else {
        // Cannonball / nuke: camera-facing round billboard (sphere impostor).
        float r = a_inst_size;
        offset = cam_right * a_pos.x * r + cam_up * a_pos.y * r;
    }

    vec3 world_pos = a_inst_pos + offset;
    gl_Position = u_proj * u_view * vec4(world_pos, 1.0);
}
