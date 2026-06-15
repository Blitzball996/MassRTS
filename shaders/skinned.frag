#version 330 core
// Fragment shader for skinned models. Simple diffuse + faction tint.
in vec3 v_color;
in vec3 v_normal;
in vec3 v_world_pos;
in vec2 v_uv;

uniform vec3 u_light_dir;     // normalized, pointing from surface to light
uniform sampler2D u_albedo;   // optional texture
uniform int u_use_texture;    // 0 = use v_color, 1 = sample u_albedo

out vec4 frag;

void main() {
    vec3 base = (u_use_texture == 1) ? texture(u_albedo, v_uv).rgb * v_color : v_color;
    vec3 N = normalize(v_normal);
    float diff = max(dot(N, normalize(u_light_dir)), 0.0);
    vec3 lit = base * (0.35 + 0.65 * diff);   // ambient + diffuse
    frag = vec4(lit, 1.0);
}
