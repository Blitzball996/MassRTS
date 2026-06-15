#version 330 core
in vec3 v_color;
in vec2 v_uv;
in float v_selected;
out vec4 frag_color;

void main() {
    vec2 p = v_uv * 2.0 - 1.0;

    // MC-style boxy humanoid silhouette (wider than before)
    float body_w = 0.42;
    float in_body = step(abs(p.x), body_w) * step(-0.55, p.y) * step(p.y, 0.45);
    // Head (square, MC style)
    float in_head = step(abs(p.x), 0.28) * step(0.45, p.y) * step(p.y, 0.9);
    // Legs
    float in_leg_l = step(abs(p.x + 0.15), 0.12) * step(-1.0, p.y) * step(p.y, -0.55);
    float in_leg_r = step(abs(p.x - 0.15), 0.12) * step(-1.0, p.y) * step(p.y, -0.55);

    float shape = max(max(in_body, in_head), max(in_leg_l, in_leg_r));
    if (shape < 0.5) discard;

    bool is_red = v_color.r < 0.33;
    vec3 color;

    if (p.y > 0.45) {
        // Head — skin colored with hair on top
        if (p.y > 0.75) {
            color = is_red ? vec3(0.22, 0.13, 0.06) : vec3(0.50, 0.38, 0.26); // hair/bald
        } else {
            color = vec3(0.62, 0.46, 0.32); // skin
            // Eyes (tiny dark dots)
            if (abs(p.x - 0.08) < 0.04 && p.y > 0.55 && p.y < 0.65) color = vec3(0.1);
            if (abs(p.x + 0.08) < 0.04 && p.y > 0.55 && p.y < 0.65) color = vec3(0.1);
        }
    } else if (p.y > -0.55) {
        // Torso
        if (is_red) color = vec3(0.0, 0.42, 0.42);
        else color = vec3(0.42, 0.27, 0.12);
    } else {
        // Legs
        if (is_red) color = vec3(0.14, 0.10, 0.36);
        else color = vec3(0.32, 0.19, 0.09);
    }

    // MC-style shading (darker at bottom)
    float shade = 0.7 + 0.3 * (p.y * 0.5 + 0.5);
    color *= shade;

    // Subtle fog
    vec3 fog = vec3(0.55, 0.62, 0.68);
    color = mix(color, fog, 0.15);

    if (v_selected > 0.5) {
        color = mix(color, vec3(0.2, 0.8, 0.2), 0.3);
    }

    frag_color = vec4(color, 1.0);
}
