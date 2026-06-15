#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;
layout(location=3) in float a_biome;
layout(location=4) in float a_height_norm;

uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_time;

out vec3 v_normal;
out vec3 v_world_pos;
out vec2 v_uv;
out float v_biome;
out float v_height_norm;

void main() {
    vec3 pos = a_pos;
    // Water surface animation
    if (int(a_biome + 0.5) == 5) {
        pos.y += sin(pos.x * 0.1 + u_time * 1.5) * 0.3 + cos(pos.z * 0.15 + u_time) * 0.2;
    }
    v_world_pos = pos;
    v_normal = a_normal;
    v_uv = a_uv;
    v_biome = a_biome;
    v_height_norm = a_height_norm;
    gl_Position = u_proj * u_view * vec4(pos, 1.0);
}
