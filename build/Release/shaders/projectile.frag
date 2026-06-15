#version 330 core
in vec3 v_color;
in vec2 v_uv;
out vec4 frag_color;

void main() {
    // Soft round shape for cannonballs, elongated for arrows
    vec2 centered = v_uv - 0.5;
    float dist = length(centered);
    float alpha = 1.0 - smoothstep(0.3, 0.5, dist);
    if (alpha < 0.01) discard;

    // Slight glow effect
    vec3 col = v_color * (1.0 + 0.3 * (1.0 - dist * 2.0));
    frag_color = vec4(col, alpha);
}
