#version 330 core
in vec3 v_normal;
in vec3 v_world_pos;
in vec2 v_uv;
in float v_biome;
in float v_height_norm;
out vec4 frag_color;

uniform float u_time;

// Noise
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x), mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
float fbm3(vec2 p) { return noise(p)*0.5+noise(p*2.0)*0.25+noise(p*4.0)*0.125+noise(p*8.0)*0.0625; }
float fbm5(vec2 p) {
    float v=0.0,a=0.5;
    for(int i=0;i<5;i++){v+=a*noise(p);p*=2.01;a*=0.5;}
    return v;
}

// Domain warping for organic look
float warped_fbm(vec2 p) {
    vec2 q = vec2(fbm3(p+vec2(1.2,3.4)), fbm3(p+vec2(5.6,7.8)));
    return fbm3(p + q * 2.0);
}

void main() {
    vec3 n = normalize(v_normal);
    int biome = int(v_biome + 0.5);
    float slope = 1.0 - n.y;
    vec2 wpos = v_world_pos.xz;

    // Detail at multiple scales
    float micro = noise(wpos * 0.5);
    float meso = noise(wpos * 0.08);
    float macro = warped_fbm(wpos * 0.003);

    vec3 base;

    if (biome == 5) {
        // === RIVER / WATER ===
        float depth = max(0.0, -v_world_pos.y + 2.0) * 0.15;
        vec3 shallow = vec3(0.15, 0.42, 0.45);
        vec3 deep = vec3(0.04, 0.12, 0.18);
        base = mix(shallow, deep, clamp(depth, 0.0, 1.0));
        // Caustics/ripple
        float ripple = noise(wpos * 0.15 + vec2(u_time * 0.4, u_time * 0.3));
        float ripple2 = noise(wpos * 0.3 - vec2(u_time * 0.2, u_time * 0.5));
        base += vec3(0.08, 0.12, 0.15) * ripple * ripple2;
        // Specular highlight
        vec3 view_dir = normalize(vec3(0.3, 0.9, 0.2));
        vec3 light = normalize(vec3(0.4, 0.85, 0.3));
        vec3 half_v = normalize(light + view_dir);
        float spec = pow(max(dot(n + vec3(0, ripple*0.1, 0), half_v), 0.0), 64.0);
        base += vec3(0.5, 0.5, 0.4) * spec * 0.6;
        // Foam at edges
        float edge_foam = smoothstep(0.0, 0.5, v_world_pos.y + 0.5);
        base = mix(base + vec3(0.3, 0.3, 0.25) * noise(wpos * 2.0 + u_time), base, edge_foam);

        // Final lighting for water (less shadow)
        vec3 light_dir = normalize(vec3(0.4, 0.85, 0.3));
        float NdotL = max(dot(n, light_dir), 0.0);
        base *= (0.6 + NdotL * 0.4);
        frag_color = vec4(base, 0.88);
        return;
    }

    if (biome == 1) {
        // === MOUNTAIN ===
        vec3 rock_a = vec3(0.32, 0.28, 0.22);
        vec3 rock_b = vec3(0.48, 0.44, 0.38);
        vec3 rock_c = vec3(0.38, 0.36, 0.30);
        float rock_detail = warped_fbm(wpos * 0.01);
        base = mix(rock_a, rock_b, rock_detail);
        base = mix(base, rock_c, micro * 0.3);
        // Cracks/strata
        float strata = noise(vec2(v_world_pos.y * 0.3, wpos.x * 0.01));
        base = mix(base, base * 0.7, strata * slope * 2.0);
        // Snow
        float snow_line = 0.6 + noise(wpos * 0.005) * 0.1;
        float snow = smoothstep(snow_line, snow_line + 0.1, v_height_norm) * smoothstep(0.3, 0.0, slope);
        base = mix(base, vec3(0.92, 0.94, 0.97), snow);
        // Moss on north faces
        float moss = smoothstep(-0.3, -0.6, n.z) * (1.0-slope) * 0.4;
        base = mix(base, vec3(0.15, 0.28, 0.08), moss);
    }
    else if (biome == 2) {
        // === SWAMP ===
        vec3 mud_dark = vec3(0.12, 0.10, 0.05);
        vec3 mud_light = vec3(0.22, 0.18, 0.08);
        vec3 swamp_green = vec3(0.08, 0.18, 0.04);
        float mud_pattern = warped_fbm(wpos * 0.008);
        base = mix(mud_dark, mud_light, mud_pattern);
        // Algae patches
        float algae = smoothstep(0.5, 0.7, noise(wpos * 0.03 + vec2(3.1, 7.2)));
        base = mix(base, swamp_green, algae * 0.6);
        // Standing water puddles (animated)
        float puddle = smoothstep(0.55, 0.65, noise(wpos * 0.02 + vec2(1.5, 2.8)));
        vec3 murky_water = vec3(0.06, 0.12, 0.08);
        murky_water += vec3(0.02) * sin(u_time + wpos.x * 0.1);
        base = mix(base, murky_water, puddle * 0.8);
        // Decomposing matter
        float rot = noise(wpos * 0.15) * noise(wpos * 0.4);
        base = mix(base, vec3(0.15, 0.08, 0.02), rot * 0.2);
    }
    else if (biome == 3) {
        // === FOREST FLOOR ===
        vec3 forest_a = vec3(0.05, 0.14, 0.03);
        vec3 forest_b = vec3(0.12, 0.25, 0.06);
        vec3 forest_c = vec3(0.08, 0.18, 0.04);
        float pattern = warped_fbm(wpos * 0.006);
        base = mix(forest_a, mix(forest_b, forest_c, meso), pattern);
        // Fallen leaves
        float leaves = noise(wpos * 0.3) * noise(wpos * 0.7 + vec2(4.2, 1.8));
        vec3 leaf_color = mix(vec3(0.28, 0.15, 0.03), vec3(0.35, 0.25, 0.05), noise(wpos * 0.5));
        base = mix(base, leaf_color, leaves * 0.35);
        // Dappled light (canopy shadow)
        float canopy = noise(wpos * 0.025) * 0.4 + 0.6;
        base *= canopy;
        // Root patterns
        float roots = smoothstep(0.7, 0.8, noise(wpos * 0.12));
        base = mix(base, vec3(0.18, 0.10, 0.04), roots * 0.3);
    }
    else if (biome == 4) {
        // === TRENCH ===
        vec3 dirt_a = vec3(0.20, 0.14, 0.07);
        vec3 dirt_b = vec3(0.28, 0.20, 0.10);
        base = mix(dirt_a, dirt_b, micro);
        // Wooden supports
        float plank_x = fract(v_world_pos.x * 0.12);
        float plank_z = fract(v_world_pos.z * 0.12);
        float planks = step(0.9, plank_x) + step(0.9, plank_z);
        base = mix(base, vec3(0.32, 0.22, 0.10), min(planks, 1.0) * 0.4);
        // Muddy bottom
        float bottom = smoothstep(5.0, 2.0, v_world_pos.y);
        base = mix(base, vec3(0.10, 0.08, 0.04), bottom * 0.5);
        // Sandbag texture on walls
        float sandbag = step(0.6, noise(wpos * 0.2)) * slope;
        base = mix(base, vec3(0.35, 0.30, 0.18), sandbag * 0.4);
    }
    else if (biome == 6) {
        // === SAND (river banks) ===
        vec3 sand_a = vec3(0.62, 0.55, 0.38);
        vec3 sand_b = vec3(0.72, 0.65, 0.45);
        float sand_detail = noise(wpos * 0.08) * 0.5 + noise(wpos * 0.4) * 0.3;
        base = mix(sand_a, sand_b, sand_detail);
        // Ripple marks
        float ripples = sin(wpos.x * 0.5 + wpos.y * 0.3 + noise(wpos * 0.02) * 5.0) * 0.5 + 0.5;
        base *= 0.9 + ripples * 0.1;
        // Wet sand near water
        float wet = smoothstep(4.0, 2.0, v_world_pos.y);
        base = mix(base, base * 0.6, wet);
    }
    else {
        // === GRASS (default) ===
        vec3 grass_lush = vec3(0.18, 0.38, 0.08);
        vec3 grass_med = vec3(0.22, 0.45, 0.12);
        vec3 grass_dry = vec3(0.40, 0.42, 0.15);
        vec3 grass_dark = vec3(0.10, 0.25, 0.05);

        float pattern = warped_fbm(wpos * 0.004);
        float variety = noise(wpos * 0.015);
        base = mix(grass_lush, grass_med, pattern);
        base = mix(base, grass_dry, smoothstep(0.55, 0.75, variety) * 0.5);
        base = mix(base, grass_dark, smoothstep(0.3, 0.0, variety) * 0.3);

        // Wildflower patches
        float flowers = noise(wpos * 0.06) * noise(wpos * 0.2);
        if (flowers > 0.55) {
            float hue = noise(wpos * 0.3);
            vec3 flower_col = mix(vec3(0.7, 0.6, 0.1), vec3(0.5, 0.2, 0.5), hue);
            base = mix(base, flower_col, (flowers - 0.55) * 3.0 * 0.15);
        }

        // Micro grass blades (high-freq pattern)
        float blades = noise(wpos * 2.0) * 0.08;
        base += vec3(0.0, blades, 0.0);

        // Dirt patches on slopes
        base = mix(base, vec3(0.25, 0.18, 0.08), smoothstep(0.2, 0.5, slope) * 0.6);
    }

    // === Global lighting ===
    vec3 sun_dir = normalize(vec3(0.4, 0.85, 0.3));
    float NdotL = max(dot(n, sun_dir), 0.0);
    float ambient = 0.35;
    float diffuse = NdotL * 0.55;

    // Indirect bounce (sky)
    float sky_ambient = max(dot(n, vec3(0,1,0)), 0.0) * 0.1;

    vec3 color = base * (ambient + diffuse + sky_ambient);

    // Shadow approximation from height
    float shadow_height = fbm5(wpos * 0.003 + vec2(2.0));
    float shadow = smoothstep(0.3, 0.5, shadow_height) * 0.15;
    color *= (1.0 - shadow);

    // Ambient occlusion from concavity
    float ao = smoothstep(-2.0, 8.0, v_world_pos.y) * 0.3 + 0.7;
    color *= ao;

    // Atmospheric fog (exponential)
    float dist = length(v_world_pos.xz);
    float fog_amount = 1.0 - exp(-dist * dist * 0.0000003);
    vec3 fog_color = mix(vec3(0.55, 0.62, 0.72), vec3(0.45, 0.50, 0.58), v_world_pos.y / 60.0);
    color = mix(color, fog_color, clamp(fog_amount, 0.0, 0.5));

    // Height-based color shift (higher = cooler tones)
    color = mix(color, color * vec3(0.9, 0.95, 1.05), v_height_norm * 0.2);

    frag_color = vec4(color, 1.0);
}
