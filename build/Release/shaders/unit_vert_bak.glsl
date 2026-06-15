#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=7) in vec2 a_part_pivot; // x=part_id, y=pivot_y

// Per-instance
layout(location=2) in vec3 a_inst_pos;
layout(location=3) in vec3 a_inst_color;
layout(location=4) in float a_inst_scale;
layout(location=5) in float a_inst_rotation;
layout(location=6) in float a_inst_state; // 0=idle, 1=walk, 2=attack

uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_time;

out vec3 v_color;
out vec3 v_normal;
out vec3 v_world_pos;

// Rotate point around Y axis at pivot
vec3 rotate_y(vec3 p, float angle, float pivot_y) {
    p.y -= pivot_y;
    float c = cos(angle), s = sin(angle);
    vec3 r = vec3(p.x*c - p.z*s, p.y, p.x*s + p.z*c);
    r.y += pivot_y;
    return r;
}

// Rotate around X axis at pivot (for limb swing)
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
    
    // Animation based on state and unique entity seed
    float seed = a_inst_pos.x * 0.1 + a_inst_pos.z * 0.07;
    float anim_time = u_time * 4.0 + seed;
    float state = a_inst_state;
    
    float swing = 0.0;
    float arm_swing = 0.0;
    
    if (state > 0.5 && state < 1.5) {
        // Walking: legs and arms swing
        swing = sin(anim_time) * 0.6;
        arm_swing = -swing;
    } else if (state > 1.5) {
        // Attacking: arms thrust forward
        arm_swing = sin(anim_time * 2.0) * 0.8;
        swing = sin(anim_time) * 0.2;
    } else {
        // Idle: subtle breathing
        swing = sin(anim_time * 0.5) * 0.02;
    }
    
    // Apply part animations
    if (part == 1) {
        // Head: slight bob
        pos.y += sin(anim_time * 0.7) * 0.01;
    } else if (part == 2) {
        // Left arm
        pos = rotate_x(pos, arm_swing, pivot_y);
        norm = rotate_x(norm, arm_swing, 0.0);
    } else if (part == 3) {
        // Right arm
        pos = rotate_x(pos, -arm_swing, pivot_y);
        norm = rotate_x(norm, -arm_swing, 0.0);
    } else if (part == 4) {
        // Left leg
        pos = rotate_x(pos, swing, pivot_y);
        norm = rotate_x(norm, swing, 0.0);
    } else if (part == 5) {
        // Right leg
        pos = rotate_x(pos, -swing, pivot_y);
        norm = rotate_x(norm, -swing, 0.0);
    } else if (part == 6) {
        // Weapon: follows right arm
        pos = rotate_x(pos, -arm_swing * 1.2, pivot_y);
        norm = rotate_x(norm, -arm_swing * 1.2, 0.0);
    }
    
    // Scale
    pos *= a_inst_scale;
    
    // Rotate whole model by facing direction (around Y)
    float c = cos(a_inst_rotation), s = sin(a_inst_rotation);
    vec3 rotated = vec3(pos.x*c - pos.z*s, pos.y, pos.x*s + pos.z*c);
    vec3 rot_norm = vec3(norm.x*c - norm.z*s, norm.y, norm.x*s + norm.z*c);
    
    vec3 world_pos = rotated + a_inst_pos;
    
    gl_Position = u_proj * u_view * vec4(world_pos, 1.0);
    v_color = a_inst_color;
    v_normal = rot_norm;
    v_world_pos = world_pos;
}
