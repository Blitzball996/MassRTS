#version 330 core
in vec3 v_color;
in vec2 v_uv;
in float v_selected;
out vec4 frag_color;

void main() {
    // Simple humanoid silhouette shape
    vec2 p = v_uv * 2.0 - 1.0; // -1 to 1
    
    // Body shape: wider at shoulders, narrow at waist
    float body_width = 0.4 - abs(p.y - 0.2) * 0.15;
    float in_body = step(abs(p.x), body_width) * step(-0.8, p.y) * step(p.y, 0.6);
    
    // Head
    float head = 1.0 - smoothstep(0.15, 0.2, length(p - vec2(0, 0.75)));
    
    float alpha = max(in_body, head);
    if (alpha < 0.1) discard;

    vec3 color = v_color;
    
    // Darker edges
    float edge = abs(p.x) / body_width;
    color *= 1.0 - edge * 0.3;

    // Selection highlight
    if (v_selected > 0.5) {
        color += vec3(0.0, 0.2, 0.05);
    }

    frag_color = vec4(color, alpha * 0.9);
}
