#version 330 core
in vec3 v_color;
in vec2 v_quad;     // [-1,1] across the billboard
in float v_stretch; // >2 = arrow, else sphere
out vec4 frag_color;

void main() {
    float r2 = dot(v_quad, v_quad);
    if (r2 > 1.0) discard;            // outside unit circle -> nothing

    if (v_stretch > 2.0) {
        // Arrow: simple soft elongated shaft.
        float a = 1.0 - smoothstep(0.6, 1.0, abs(v_quad.x));
        if (a < 0.01) discard;
        frag_color = vec4(v_color * 1.1, a);
        return;
    }

    // Sphere impostor: reconstruct a hemisphere normal from the quad coord
    // so the cannonball/nuke shades like a real 3D ball.
    vec3 n = vec3(v_quad.x, v_quad.y, sqrt(max(0.0, 1.0 - r2)));
    vec3 L = normalize(vec3(0.4, 0.7, 0.6));   // key light
    float diff = max(dot(n, L), 0.0);
    float ambient = 0.30;
    // Specular highlight for a metallic/stony ball look.
    vec3 V = vec3(0.0, 0.0, 1.0);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 32.0) * 0.6;
    // Rim term so the silhouette reads against bright terrain.
    float rim = pow(1.0 - n.z, 2.0) * 0.25;

    vec3 col = v_color * (ambient + diff * 0.9) + vec3(spec) + v_color * rim;
    // Soft anti-aliased edge.
    float edge = 1.0 - smoothstep(0.92, 1.0, sqrt(r2));
    frag_color = vec4(col, edge);
}
