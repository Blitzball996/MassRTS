#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=7) in vec2 a_part_pivot;

layout(location=2) in vec3 a_inst_pos;
layout(location=3) in vec3 a_inst_color;
layout(location=4) in float a_inst_scale;
layout(location=5) in float a_inst_rotation;
layout(location=6) in float a_inst_state;

uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_time;
uniform float u_model_scale; // manifest per-unit render scale (default 1.0)

out vec3 v_color;
out vec3 v_normal;
out vec3 v_world_pos;
out float v_part_id;
out vec3 v_local_pos;
out vec3 v_local_normal; // pre-rotation normal for face detection

vec3 rotate_x(vec3 p, float angle, float pivot_y) {
    p.y -= pivot_y;
    float c = cos(angle), s = sin(angle);
    vec3 r = vec3(p.x, p.y*c - p.z*s, p.y*s + p.z*c);
    r.y += pivot_y;
    return r;
}

void main() {
    int part = int(a_part_pivot.x + 0.5);
    float pivot_y = a_part_pivot.y;
    vec3 pos = a_pos;
    vec3 norm = a_normal;

    float raw_state = a_inst_state;
    float base_state = mod(raw_state, 4.0);
    float hit_flash = step(4.0, raw_state);

    float seed = a_inst_pos.x * 0.13 + a_inst_pos.z * 0.09;
    float anim_time = u_time * 3.5 + seed;
    float swing = 0.0, arm_swing = 0.0;

    float dead_tilt = 0.0;
    if (base_state > 2.5) {
        dead_tilt = 1.5708;
    } else if (base_state > 0.5 && base_state < 1.5) {
        swing = sin(anim_time) * 0.5;
        arm_swing = -swing * 0.7;
    } else if (base_state > 1.5) {
        arm_swing = sin(anim_time * 2.5) * 0.8;
        swing = sin(anim_time) * 0.15;
    }

    if (hit_flash > 0.5) pos.y += abs(sin(u_time * 12.0)) * 2.0;

    // Part animations
    if (part == 2) { pos = rotate_x(pos, arm_swing, pivot_y); norm = rotate_x(norm, arm_swing, 0.0); }
    else if (part == 3 || part == 6) { pos = rotate_x(pos, -arm_swing, pivot_y); norm = rotate_x(norm, -arm_swing, 0.0); }
    else if (part == 4 || part == 10) { pos = rotate_x(pos, swing, pivot_y); norm = rotate_x(norm, swing, 0.0); }
    else if (part == 5) { pos = rotate_x(pos, -swing, pivot_y); norm = rotate_x(norm, -swing, 0.0); }

    // Save local-space data BEFORE any world transform
    v_local_pos = pos;
    v_local_normal = norm; // This is the key fix - local normal for face detection

    float pixel_scale = (a_inst_scale / 32.0) * u_model_scale;
    pos *= pixel_scale;

    // Dead tilt
    if (dead_tilt > 0.0) {
        float ct = cos(dead_tilt), st = sin(dead_tilt);
        pos = vec3(pos.x, pos.y*ct - pos.z*st, pos.y*st + pos.z*ct);
        norm = vec3(norm.x, norm.y*ct - norm.z*st, norm.y*st + norm.z*ct);
    }

    // Y-axis rotation for facing direction
    float angle = -a_inst_rotation;
    float c = cos(angle), s = sin(angle);
    vec3 rotated = vec3(pos.x*c - pos.z*s, pos.y, pos.x*s + pos.z*c);
    vec3 rot_norm = vec3(norm.x*c - norm.z*s, norm.y, norm.x*s + norm.z*c);

    vec3 world_pos = rotated + a_inst_pos;
    gl_Position = u_proj * u_view * vec4(world_pos, 1.0);

    vec3 out_color = a_inst_color;
    if (hit_flash > 0.5) out_color = vec3(0.9, a_inst_color.g, a_inst_color.b * 0.1);
    v_color = out_color;
    v_normal = rot_norm;
    v_world_pos = world_pos;
    v_part_id = a_part_pivot.x;
}
