#version 330 core
in vec3 v_color;
in vec2 v_uv;
out vec4 frag_color;

void main() {
    float dist = length(v_uv - 0.5) * 2.0;
    float alpha = 1.0 - smoothstep(0.5, 1.0, dist);
    if (alpha < 0.05) discard;
    
    // Hot center, fade to color at edges
    vec3 hot = vec3(1.0, 0.9, 0.5);
    vec3 color = mix(hot, v_color, dist);
    
    frag_color = vec4(color, alpha * 0.8);
}
