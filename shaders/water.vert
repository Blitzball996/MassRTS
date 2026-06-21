#version 330 core
layout(location=0) in vec2 a_xz;       // world XZ
layout(location=1) in float a_surfaceY; // bed + water
layout(location=2) in float a_depth;    // water column height
layout(location=3) in vec2 a_vel;       // horizontal velocity

uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_time;

out vec3 v_world;
out float v_depth;
out vec2 v_vel;
out float v_dry;

void main() {
    v_depth = a_depth;
    v_vel = a_vel;
    // Dry cells (no water): flag so the fragment can discard them. We also sink
    // them slightly to avoid a thin sheen sitting on dry ground.
    v_dry = (a_depth < 0.04) ? 1.0 : 0.0;

    float y = a_surfaceY;
    // Small gerstner-ish ripple on the visible surface (amplitude scaled down in
    // very shallow water so puddles stay calm).
    float amp = min(a_depth, 1.0) * 0.35;
    y += sin(a_xz.x * 0.05 + u_time * 1.7) * amp * 0.4;
    y += cos(a_xz.y * 0.06 - u_time * 1.3) * amp * 0.4;

    v_world = vec3(a_xz.x, y, a_xz.y);
    gl_Position = u_proj * u_view * vec4(v_world, 1.0);
}
