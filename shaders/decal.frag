#version 330 core
in vec2 v_uv;      // [-1,1] local quad coords
in vec3 v_color;
in float v_fade;
in float v_kind;   // 0 blood, 1 corpse
in float v_seed;
out vec4 frag_color;

// Cheap value noise for organic edges.
float hash(vec2 p){ p=fract(p*vec2(123.34,456.21)); p+=dot(p,p+45.32); return fract(p.x*p.y); }
float noise(vec2 p){
    vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),
               mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}

void main() {
    float r = length(v_uv);

    if (v_kind < 0.5) {
        // === BLOOD SPLAT ===
        // Irregular blob: radius modulated by angular noise so edges are ragged,
        // with a few satellite spatters. Dense overlapping splats blend (alpha)
        // into pools and rivers.
        float ang = atan(v_uv.y, v_uv.x);
        float edge = 0.62 + 0.30 * noise(vec2(ang * 2.0, v_seed * 10.0))
                          + 0.12 * noise(v_uv * 3.0 + v_seed * 20.0);
        float blob = 1.0 - smoothstep(edge - 0.18, edge, r);
        // Spatter speckles outside the main blob.
        float spk = noise(v_uv * 6.0 + v_seed * 5.0);
        float spatter = smoothstep(0.82, 0.9, spk) * (1.0 - smoothstep(0.9, 1.0, r));
        float a = max(blob, spatter * 0.6);
        if (a < 0.02) discard;

        // Real blood is dark crimson, NOT pure red. Deep maroon center -> dried
        // brown edge. v_color from the system already carries a dark-red base.
        vec3 fresh = vec3(0.32, 0.015, 0.02);  // dark wet crimson
        vec3 dried = vec3(0.16, 0.05, 0.035);  // oxidized brown
        // center is wettest/darkest, rim drier/browner
        float rim = smoothstep(0.0, edge, r);
        vec3 col = mix(fresh, dried, rim);

        // Drying over lifetime: v_fade ~1 fresh, ->0 old. Older blood browns and
        // dulls so old battlefields read differently from fresh kills.
        float wetness = clamp(v_fade, 0.0, 1.0);
        col = mix(dried * 0.7, col, wetness);

        // Wet sheen: a soft specular lobe gives fresh blood a glossy highlight
        // (this is what separates "wet blood" from "red paint"). A faux light
        // direction in decal space; strongest near the center while wet.
        vec3 N = normalize(vec3((noise(v_uv*4.0+v_seed)-0.5)*0.4,
                                (noise(v_uv*4.0+v_seed+7.0)-0.5)*0.4, 1.0));
        float sheen = pow(max(N.z, 0.0), 24.0) * (1.0 - rim) * wetness;
        col += vec3(0.5, 0.18, 0.15) * sheen * 0.8;

        frag_color = vec4(col, a * (0.45 + 0.5*wetness));
        return;
    }

    // === CORPSE BODY (flat humanoid silhouette, shaded for volume) ===
    vec2 uv = v_uv;
    // torso capsule along Y, narrow in X
    float torso = 1.0 - smoothstep(0.0, 1.0,
        length(vec2(uv.x * 2.4, max(abs(uv.y) - 0.35, 0.0) * 1.6)));
    // head near +Y
    float head = 1.0 - smoothstep(0.0, 1.0, length((uv - vec2(0.0, 0.62)) * 3.4));
    // splayed arms (two short diagonals)
    float arm1 = 1.0 - smoothstep(0.0, 1.0, length((uv - vec2(0.32, 0.05)) * vec2(3.0, 5.0)));
    float arm2 = 1.0 - smoothstep(0.0, 1.0, length((uv - vec2(-0.32, 0.05)) * vec2(3.0, 5.0)));
    float body = max(max(torso, head), max(arm1, arm2) * 0.8);

    // Contact shadow: a soft dark ring/halo UNDER the body so it reads as lying
    // ON the ground, not floating. Drawn wherever the body itself isn't.
    float shadow_field = max(torso, head) ;
    float contact = smoothstep(0.0, 0.5, shadow_field) * (1.0 - smoothstep(0.04, 0.25, body));

    if (body < 0.04 && contact < 0.02) discard;

    // Fake volume: treat the silhouette distance as a rounded height so the
    // center is "raised" (lit) and edges fall into shadow. This is what stops
    // it reading as a flat white card.
    float bulge = smoothstep(0.04, 0.6, body);
    float lit = 0.45 + 0.55 * bulge;       // center brighter, edges darker
    // Desaturate + darken the unit color: corpses are lifeless, not vivid.
    float luma = dot(v_color, vec3(0.299, 0.587, 0.114));
    vec3 corpse_col = mix(v_color, vec3(luma), 0.45) * 0.5;
    float grime = 0.8 + 0.2 * noise(uv * 5.0 + v_seed * 8.0);
    vec3 col = corpse_col * lit * grime;
    // a little dried blood pooling at the body
    col = mix(col, vec3(0.14, 0.04, 0.03), (1.0 - bulge) * 0.3);

    float body_a = smoothstep(0.04, 0.2, body);
    // Composite contact shadow under the body.
    vec3 out_col = mix(vec3(0.0), col, body_a);
    float out_a = max(body_a, contact * 0.4) * v_fade;
    frag_color = vec4(out_col, out_a);
}
