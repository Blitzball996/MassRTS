#version 330 core
in vec3 v_color;
in vec3 v_normal;
in float v_mat;
in float v_fade;

out vec4 frag_color;

const vec3 BARK = vec3(0.33, 0.22, 0.12);
const vec3 BARK_DARK = vec3(0.22, 0.14, 0.08);

void main() {
    vec3 n = normalize(v_normal);
    // Simple directional light
    vec3 light_dir = normalize(vec3(0.4, 0.9, 0.3));
    float diff = max(dot(n, light_dir), 0.0);
    float ambient = 0.45;
    float light = ambient + diff * 0.7;

    int mat = int(v_mat + 0.5);
    vec3 base_color;
    if (mat == 0) {
        // Trunk / branches: bark, slight variation by facing
        base_color = mix(BARK_DARK, BARK, n.y * 0.5 + 0.5);
    } else if (mat == 1) {
        // Foliage: instance color with volume shading
        float shade = 0.80 + 0.20 * (n.y * 0.5 + 0.5);
        base_color = v_color * shade;
    } else {
        // Generic decor (rocks, banners): straight instance color
        base_color = v_color;
    }

    vec3 lit = base_color * light;
    frag_color = vec4(lit, v_fade);
}
