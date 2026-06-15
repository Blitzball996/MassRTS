#version 330 core
in vec2 v_uv;
out vec4 frag_color;

void main() {
    float bx = min(v_uv.x, 1.0 - v_uv.x);
    float by = min(v_uv.y, 1.0 - v_uv.y);
    float border = step(min(bx, by), 0.02);
    
    vec3 fill_color = vec3(0.2, 0.8, 0.3);
    float alpha = mix(0.1, 0.8, border);
    frag_color = vec4(fill_color, alpha);
}
