#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=7) in vec2 a_part;   // x = material id (0 bark, 1 leaf, 2 generic)

layout(location=2) in vec3 a_inst_pos;
layout(location=3) in vec3 a_inst_color;
layout(location=4) in float a_inst_scale;
layout(location=5) in float a_inst_rotation;
layout(location=10) in float a_inst_fade;

uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_time;

out vec3 v_color;
out vec3 v_normal;
out float v_mat;       // material id passthrough
out float v_fade;

void main() {
    vec3 pos = a_pos;
    vec3 norm = a_normal;

    // Wind sway: stronger higher up the tree (canopy), near-zero at the base.
    float sway_amount = clamp(pos.y * 0.4, 0.0, 1.0);
    float seed = a_inst_pos.x * 0.11 + a_inst_pos.z * 0.07;
    float wind = sin(u_time * 1.3 + seed) * 0.05 * sway_amount;
    pos.x += wind;
    pos.z += wind * 0.6;

    v_mat = a_part.x;

    // Uniform scale (decor mesh authored ~1-2 units tall; match unit scale convention)
    float s = a_inst_scale * 4.0;
    pos *= s;

    // Y-axis rotation
    float angle = a_inst_rotation;
    float c = cos(angle), si = sin(angle);
    vec3 rotated = vec3(pos.x*c - pos.z*si, pos.y, pos.x*si + pos.z*c);
    vec3 rot_norm = vec3(norm.x*c - norm.z*si, norm.y, norm.x*si + norm.z*c);

    vec3 world_pos = rotated + a_inst_pos;
    gl_Position = u_proj * u_view * vec4(world_pos, 1.0);

    v_color = a_inst_color;
    v_normal = rot_norm;
    v_fade = (a_inst_fade <= 0.0) ? 1.0 : a_inst_fade;
}
