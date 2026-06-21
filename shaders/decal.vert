#version 330 core
layout(location=0) in vec2 a_quad;       // [-0.5,0.5] in local XZ
layout(location=1) in vec3 a_pos;         // world center (y = ground)
layout(location=2) in vec3 a_color;
layout(location=3) in float a_radius;
layout(location=4) in float a_rotation;
layout(location=5) in float a_age;
layout(location=6) in float a_life;
layout(location=7) in float a_kind;       // 0 blood, 1 corpse
layout(location=8) in float a_seed;

uniform mat4 u_view;
uniform mat4 u_proj;

out vec2 v_uv;       // [-1,1] local
out vec3 v_color;
out float v_fade;
out float v_kind;
out float v_seed;

void main() {
    // Ground-aligned quad: lay the local XZ plane flat on the terrain, rotated
    // by yaw. No camera-facing — these are decals painted on the ground.
    float c = cos(a_rotation), s = sin(a_rotation);
    vec2 r = vec2(a_quad.x * c - a_quad.y * s, a_quad.x * s + a_quad.y * c);
    vec3 world = a_pos + vec3(r.x * a_radius * 2.0, 0.0, r.y * a_radius * 2.0);

    v_uv = a_quad * 2.0;
    v_color = a_color;
    v_kind = a_kind;
    v_seed = a_seed;

    // Fade only in the last 20% of lifetime; blood also darkens (dries) over time.
    float t = clamp(a_age / max(a_life, 0.001), 0.0, 1.0);
    v_fade = 1.0 - smoothstep(0.8, 1.0, t);

    gl_Position = u_proj * u_view * vec4(world, 1.0);
}
