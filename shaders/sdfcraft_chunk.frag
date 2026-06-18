#version 330 core
in vec3 v_color;
in vec3 v_normal;
in vec3 v_world;
in float v_mat;

uniform vec3  u_sun_dir;
uniform vec3  u_cam;
uniform float u_alpha;
uniform vec3  u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;

out vec4 frag;

// Material codes (keep in sync with blocks.h)
const int MAT_GENERIC=0, MAT_GRASS=1, MAT_DIRT=2, MAT_ROCK=3, MAT_SAND=4,
          MAT_SNOW=5, MAT_WOOD=6, MAT_LEAVES=7, MAT_ORE=8, MAT_WATER=9, MAT_GRAVEL=10;

// === Noise (verbatim from terrain.frag) ===
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x), mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
float fbm3(vec2 p) { return noise(p)*0.5+noise(p*2.0)*0.25+noise(p*4.0)*0.125+noise(p*8.0)*0.0625; }
float fbm5(vec2 p) { float v=0.0,a=0.5; for(int i=0;i<5;i++){v+=a*noise(p);p*=2.01;a*=0.5;} return v; }
float warped_fbm(vec2 p) {
    vec2 q = vec2(fbm3(p+vec2(1.2,3.4)), fbm3(p+vec2(5.6,7.8)));
    return fbm3(p + q * 2.0);
}

// Continuous terrain colouring (MassRTS terrain.frag style).
// Instead of hard per-block material zones (which cut grass/dirt along voxel
// edges and read as jaggies), we drive the whole surface from CONTINUOUS
// signals: surface slope and world height. Flat ground = lush grass, steep
// faces expose dirt then rock, exactly like the analytic RTS terrain. v_mat is
// only consulted for things that are genuinely a distinct material (wood,
// leaves, water, ore, sand, snow); everything earthy is the continuous blend.
vec3 terrain_surface(vec2 wpos, float slope, vec3 n) {
    // --- grass (default flat ground) ---
    vec3 grass_lush = vec3(0.18,0.38,0.08), grass_med = vec3(0.22,0.45,0.12);
    vec3 grass_dry  = vec3(0.40,0.42,0.15), grass_dark= vec3(0.10,0.25,0.05);
    float pattern = warped_fbm(wpos*0.004);
    float variety = noise(wpos*0.015);
    vec3 grass = mix(grass_lush, grass_med, pattern);
    grass = mix(grass, grass_dry,  smoothstep(0.55,0.75,variety)*0.5);
    grass = mix(grass, grass_dark, smoothstep(0.3,0.0,variety)*0.3);
    float flowers = noise(wpos*0.06)*noise(wpos*0.2);
    if (flowers > 0.55) {
        float hue = noise(wpos*0.3);
        vec3 fc = mix(vec3(0.7,0.6,0.1), vec3(0.5,0.2,0.5), hue);
        grass = mix(grass, fc, (flowers-0.55)*3.0*0.15);
    }
    grass += vec3(0.0, noise(wpos*2.0)*0.08, 0.0);

    // --- dirt (moderate slopes / shallow subsurface) ---
    vec3 dirt_a=vec3(0.34,0.29,0.24), dirt_b=vec3(0.26,0.23,0.20);
    float clods = warped_fbm(wpos*0.05 + v_world.y*0.1);
    vec3 dirt = mix(dirt_a, dirt_b, clods);
    float peb = noise(wpos*0.6 + vec2(v_world.y));
    dirt = mix(dirt, vec3(0.20,0.18,0.16), smoothstep(0.6,0.8,peb)*0.4);

    // --- rock (steep faces / deep subsurface) ---
    vec3 rock_a=vec3(0.42,0.42,0.43), rock_b=vec3(0.30,0.30,0.31), rock_c=vec3(0.50,0.50,0.51);
    float rough = warped_fbm(wpos*0.04);
    vec3 rock = mix(rock_a, rock_b, rough);
    float band = sin(v_world.y*0.55 + noise(wpos*0.05)*2.5)*0.5+0.5;
    rock = mix(rock, rock_c, smoothstep(0.55,0.85,band)*0.4);
    float vein = smoothstep(0.8,0.92, noise(wpos*0.3 + v_world.y*0.2));
    rock = mix(rock, vec3(0.58,0.58,0.60), vein*0.25);

    // Continuous slope blend: grass -> dirt -> rock. Soft smoothstep edges mean
    // the transition dissolves smoothly across the rounded MC surface instead of
    // snapping at block faces. Grass dominates anything near-flat.
    vec3 col = grass;
    col = mix(col, dirt, smoothstep(0.25, 0.55, slope));
    col = mix(col, rock, smoothstep(0.60, 0.85, slope));
    return col;
}

// PLACEHOLDER_PALETTE
vec3 pal_grass(vec2 wpos) {
    vec3 grass_lush=vec3(0.18,0.38,0.08), grass_med=vec3(0.22,0.45,0.12);
    vec3 grass_dry=vec3(0.40,0.42,0.15),  grass_dark=vec3(0.10,0.25,0.05);
    float pattern = warped_fbm(wpos*0.004);
    float variety = noise(wpos*0.015);
    vec3 b = mix(grass_lush, grass_med, pattern);
    b = mix(b, grass_dry, smoothstep(0.55,0.75,variety)*0.5);
    b = mix(b, grass_dark, smoothstep(0.3,0.0,variety)*0.3);
    float flowers = noise(wpos*0.06)*noise(wpos*0.2);
    if (flowers > 0.55) {
        float hue = noise(wpos*0.3);
        vec3 fc = mix(vec3(0.7,0.6,0.1), vec3(0.5,0.2,0.5), hue);
        b = mix(b, fc, (flowers-0.55)*3.0*0.15);
    }
    b += vec3(0.0, noise(wpos*2.0)*0.08, 0.0);
    return b;
}
vec3 pal_dirt(vec2 wpos, float y) {
    vec3 dirt_a=vec3(0.34,0.29,0.24), dirt_b=vec3(0.26,0.23,0.20);
    float clods = warped_fbm(wpos*0.05 + y*0.1);
    vec3 b = mix(dirt_a, dirt_b, clods);
    float peb = noise(wpos*0.6 + vec2(y));
    b = mix(b, vec3(0.20,0.18,0.16), smoothstep(0.6,0.8,peb)*0.4);
    return b;
}
vec3 pal_rock(vec2 wpos, float y) {
    vec3 rock_a=vec3(0.42,0.42,0.43), rock_b=vec3(0.30,0.30,0.31), rock_c=vec3(0.50,0.50,0.51);
    float rough = warped_fbm(wpos*0.04);
    vec3 b = mix(rock_a, rock_b, rough);
    float band = sin(y*0.55 + noise(wpos*0.05)*2.5)*0.5+0.5;
    b = mix(b, rock_c, smoothstep(0.55,0.85,band)*0.4);
    float vein = smoothstep(0.8,0.92, noise(wpos*0.3 + y*0.2));
    b = mix(b, vec3(0.58,0.58,0.60), vein*0.25);
    return b;
}
vec3 palette_int(int mat, vec2 wpos, float slope, float micro, float meso, vec3 n, vec3 vcol) {
    // Earthy terrain (grass/dirt/rock/gravel) is one CONTINUOUS slope-driven
    // surface — no per-block seams. Flat = grass, steep = dirt then rock.
    if (mat == MAT_GRASS || mat == MAT_DIRT || mat == MAT_ROCK)
        return terrain_surface(wpos, slope, n);
    if (mat == MAT_GRAVEL) {
        vec3 b = terrain_surface(wpos, max(slope, 0.5), n) * 1.05;
        float peb = noise(wpos*0.9);
        return mix(b, vec3(0.40,0.38,0.36), smoothstep(0.4,0.7,peb)*0.5);
    }
    if (mat == MAT_ORE) {
        // continuous rock base with the ore's albedo as sparse speckle
        vec3 rock = terrain_surface(wpos, max(slope, 0.7), n);
        float sp = noise(wpos*1.4 + v_world.y*1.1);
        return mix(rock, vcol*1.2, smoothstep(0.62,0.82,sp));
    }
    if (mat == MAT_SAND) {
        vec3 sand_a=vec3(0.62,0.55,0.38), sand_b=vec3(0.72,0.65,0.45);
        float sd = noise(wpos*0.08)*0.5 + noise(wpos*0.4)*0.3;
        vec3 b = mix(sand_a, sand_b, sd);
        float rip = sin(wpos.x*0.5 + wpos.y*0.3 + noise(wpos*0.02)*5.0)*0.5+0.5;
        return b * (0.9 + rip*0.1);
    }
    if (mat == MAT_SNOW) {
        vec3 b = mix(vec3(0.90,0.93,0.97), vec3(0.82,0.86,0.92), warped_fbm(wpos*0.02));
        b += vec3(noise(wpos*1.5)*0.04);
        return b;
    }
    if (mat == MAT_WOOD) {
        // bark/plank grain: tint vertex colour with directional grain rings
        float rings = sin(v_world.y*3.0 + noise(wpos*0.5)*2.0)*0.5+0.5;
        float grain = noise(wpos*8.0 + v_world.y*4.0);
        vec3 b = vcol * (0.82 + 0.22*rings);
        b = mix(b, b*0.78, smoothstep(0.6,0.85,grain)*0.5);
        return b;
    }
    if (mat == MAT_LEAVES) {
        // clustered foliage: break up the flat green with blotchy noise
        float clump = warped_fbm(wpos*0.6 + v_world.y*0.3);
        float spec  = noise(wpos*3.0 + v_world.y*2.0);
        vec3 b = mix(vcol*0.7, vcol*1.15, clump);
        b = mix(b, vec3(0.30,0.42,0.12), smoothstep(0.55,0.8,spec)*0.4);
        return b;
    }
    if (mat == MAT_ORE) {
        // grey rock with the ore's albedo as sparse speckle
        vec3 rock = pal_rock(wpos, v_world.y);
        float sp = noise(wpos*1.4 + v_world.y*1.1);
        return mix(rock, vcol*1.2, smoothstep(0.62,0.82,sp));
    }
    if (mat == MAT_WATER) return vcol;
    return vcol; // MAT_GENERIC
}

// Continuous-mat palette: when MC triangles span two material zones the GPU
// interpolates v_mat to a fractional value (e.g. grass=1 to dirt=2 -> 1.5).
// Blend the two neighbouring palettes by the fractional part to dissolve the
// hard grass/dirt seam into a smooth gradient.
vec3 palette(float mat, vec2 wpos, float slope, float micro, float meso, vec3 n, vec3 vcol) {
    int mat_lo = int(floor(mat));
    int mat_hi = mat_lo + 1;
    float t = fract(mat);  // blend factor
    vec3 col_lo = palette_int(mat_lo, wpos, slope, micro, meso, n, vcol);
    vec3 col_hi = palette_int(mat_hi, wpos, slope, micro, meso, n, vcol);
    return mix(col_lo, col_hi, smoothstep(0.0, 1.0, t));
}


void main() {
    vec3 n = normalize(v_normal);
    vec2 wpos = v_world.xz;
    float slope = 1.0 - abs(n.y);
    float micro = noise(wpos * 0.5);
    float meso  = noise(wpos * 0.08);

    vec3 base = palette(v_mat, wpos, slope, micro, meso, n, v_color);

    // === Global lighting (terrain.frag) ===
    float NdotL   = max(dot(n, normalize(u_sun_dir)), 0.0);
    float ambient = 0.35;
    float diffuse = NdotL * 0.55;
    float sky_amb = max(dot(n, vec3(0,1,0)), 0.0) * 0.10;
    vec3 color = base * (ambient + diffuse + sky_amb);

    // PLACEHOLDER_MAIN
    // shadow approximation from large-scale height noise (terrain.frag)
    float shadow_h = fbm5(wpos*0.003 + vec2(2.0));
    float shadow = smoothstep(0.3,0.5,shadow_h)*0.15;
    color *= (1.0 - shadow);

    // AO from concavity / world height
    float ao = smoothstep(-2.0, 8.0, v_world.y)*0.3 + 0.7;
    color *= ao;

    // distance fog (renderer-driven start/end + colour)
    float d = length(v_world - u_cam);
    float fog = clamp((d - u_fog_start) / max(u_fog_end - u_fog_start, 1.0), 0.0, 1.0);
    color = mix(color, u_fog_color, fog);

    frag = vec4(color, u_alpha);
}
