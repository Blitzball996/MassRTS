#version 330 core
in vec3 v_color;
in vec2 v_quad;     // [-1,1] across the billboard
in float v_stretch; // >2 = arrow, else sphere/glow-core
out vec4 frag_color;

void main() {
    float r2 = dot(v_quad, v_quad);
    if (r2 > 1.0) discard;            // outside unit circle -> nothing

    if (v_stretch > 2.0) {
        // Arrow: thin shaft with a pointed head and fletching at the tail.
        // v_quad.y goes -1 (tail) to +1 (tip) along the flight axis;
        // v_quad.x is across the width [-1,1].
        float shaft_t = v_quad.y * 0.5 + 0.5; // 0=tail, 1=tip
        float ax = abs(v_quad.x);

        // Width profile along the length: a slim shaft, a triangular arrowhead
        // near the tip, and a wider fletching flare at the very tail.
        float head_mask  = smoothstep(0.72, 0.80, shaft_t);     // front ~25%
        float fletch_mask = smoothstep(0.20, 0.10, shaft_t);    // rear ~15%

        // Half-width allowed at this position (in quad-x units).
        float shaft_hw = 0.18;
        // Arrowhead tapers from a wide base to a point at the tip.
        float head_hw = mix(0.55, 0.0, smoothstep(0.80, 1.0, shaft_t));
        // Fletching flares out toward the tail.
        float fletch_hw = mix(shaft_hw, 0.5, fletch_mask);

        float half_w = shaft_hw;
        half_w = mix(half_w, head_hw, head_mask);
        half_w = max(half_w, fletch_mask * fletch_hw);

        // Inside the silhouette? soft edge for AA.
        float edge = 1.0 - smoothstep(half_w - 0.06, half_w + 0.02, ax);
        if (edge < 0.01) discard;

        vec3 shaft_color  = vec3(0.20, 0.13, 0.06);   // dark wood
        vec3 head_color   = vec3(0.78, 0.80, 0.85);   // steel arrowhead
        vec3 fletch_color = vec3(0.55, 0.18, 0.14);   // red feathers

        vec3 col = shaft_color;
        col = mix(col, fletch_color, fletch_mask);
        col = mix(col, head_color, head_mask);

        // A little length-wise shading for roundness on the shaft.
        col *= 0.75 + 0.25 * (1.0 - ax / max(half_w, 0.001));

        frag_color = vec4(col, edge);
        return;
    }

    float r = sqrt(r2);

    // Emissive glow core: when the instance color carries HDR energy (>1.0,
    // i.e. cannon/nuke shells and their trail), render a hot radial core that
    // blooms instead of a lit-from-outside ball. A white-hot center fades to
    // the shell color, then to a soft transparent halo.
    float emissive = max(v_color.r, max(v_color.g, v_color.b));
    if (emissive > 1.0) {
        float core = 1.0 - smoothstep(0.0, 0.35, r); // tight hot center
        float halo = 1.0 - smoothstep(0.2, 1.0, r);  // wider falloff
        vec3 white_hot = vec3(emissive);
        vec3 col = mix(v_color, white_hot, core * 0.7) * (halo + core * 1.5);
        float alpha = max(halo, core);
        if (alpha < 0.01) discard;
        frag_color = vec4(col, alpha);
        return;
    }

    // Non-emissive sphere impostor (fallback): shade like a real 3D ball.
    vec3 n = vec3(v_quad.x, v_quad.y, sqrt(max(0.0, 1.0 - r2)));
    vec3 L = normalize(vec3(0.4, 0.7, 0.6));
    float diff = max(dot(n, L), 0.0);
    float ambient = 0.30;
    vec3 V = vec3(0.0, 0.0, 1.0);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(n, H), 0.0), 32.0) * 0.6;
    float rim = pow(1.0 - n.z, 2.0) * 0.25;
    vec3 col = v_color * (ambient + diff * 0.9) + vec3(spec) + v_color * rim;
    float edge = 1.0 - smoothstep(0.92, 1.0, r);
    frag_color = vec4(col, edge);
}

