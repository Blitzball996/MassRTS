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

// === Triplanar sampling weights ===
// Projecting all detail onto the XZ plane stretches it into streaks on steep
// faces and vertical walls — that's the "unnatural faceted polygon" look. With
// a real 3D surface (SDF / Marching Cubes) we must blend three planar samples
// (YZ, XZ, XY) weighted by the surface normal so cliffs and dug-out walls stay
// just as crisp as flat ground. `tri_w` returns the per-axis blend weights.
vec3 tri_w(vec3 n) {
    vec3 w = pow(abs(n), vec3(4.0));     // sharpen so the dominant axis wins
    return w / max(w.x + w.y + w.z, 1e-5);
}
// Triplanar 2D-noise: sample the same fbm on the three world planes and blend.
float tri_noise(vec3 p, vec3 w, float freq) {
    float nx = noise(p.yz * freq);
    float ny = noise(p.xz * freq);
    float nz = noise(p.xy * freq);
    return nx * w.x + ny * w.y + nz * w.z;
}
float tri_warped(vec3 p, vec3 w, float freq) {
    float nx = warped_fbm(p.yz * freq);
    float ny = warped_fbm(p.xz * freq);
    float nz = warped_fbm(p.xy * freq);
    return nx * w.x + ny * w.y + nz * w.z;
}

// Continuous terrain colouring (MassRTS terrain.frag style).
// Instead of hard per-block material zones (which cut grass/dirt along voxel
// edges and read as jaggies), we drive the whole surface from CONTINUOUS
// signals: surface slope and world height. Flat ground = lush grass, steep
// faces expose dirt then rock, exactly like the analytic RTS terrain. v_mat is
// only consulted for things that are genuinely a distinct material (wood,
// leaves, water, ore, sand, snow); everything earthy is the continuous blend.
vec3 terrain_surface(vec2 wpos, float slope, vec3 n, int surfMat) {
    vec3 P = v_world;          // full 3D position for triplanar
    vec3 W = tri_w(n);         // triplanar blend weights
    // --- ONE uniform grass colour for the whole natural surface ---
    // User request: the terrain is just grass, a single flat colour. No
    // slope/height/biome blending (that was the source of the contour stripes).
    // Only a barely-there large-scale tonal wobble so it isn't a dead flat fill.
    vec3 grass = vec3(0.27,0.50,0.15);
    grass *= 0.97 + tri_noise(P, W, 0.4) * 0.05;

    // Dug-out / explicitly non-grass blocks still read correctly: the mesher
    // tags each vertex with the block material, so a carved pit floor shows
    // dirt/stone instead of grass. Natural ground is always grass.
    vec3 col = grass;
    if (surfMat == MAT_ROCK) {
        vec3 rock = vec3(0.40,0.40,0.43);
        rock *= 0.95 + tri_noise(P, W, 0.8) * 0.07;
        col = rock;
    } else if (surfMat == MAT_DIRT) {
        vec3 dirt = vec3(0.32,0.27,0.22);
        dirt *= 0.94 + tri_noise(P, W, 1.2) * 0.10;
        col = dirt;
    }
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
        return terrain_surface(wpos, slope, n, mat);
    if (mat == MAT_GRAVEL) {
        vec3 b = terrain_surface(wpos, max(slope, 0.5), n, MAT_ROCK) * 1.05;
        float peb = noise(wpos*0.9);
        return mix(b, vec3(0.40,0.38,0.36), smoothstep(0.4,0.7,peb)*0.5);
    }
    if (mat == MAT_ORE) {
        // continuous rock base with the ore's albedo as sparse speckle
        vec3 rock = terrain_surface(wpos, max(slope, 0.7), n, MAT_ROCK);
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
        vec3 b = vcol * (0.85 + 0.20*rings);
        b = mix(b, b*0.74, smoothstep(0.6,0.85,grain)*0.5);
        return b;
    }
    if (mat == MAT_LEAVES) {
        // Lush canopy (MassRTS forest-floor greens). Leaves now render solid in
        // the opaque pass, so we build a rich, varied green instead of a flat
        // washed-out tint: blotchy clumps + a few sun-touched highlights + a
        // little dark interior so the crown reads as 3D foliage, not a green box.
        vec3 leaf_lush = vec3(0.12,0.34,0.07);
        vec3 leaf_mid  = vec3(0.18,0.44,0.11);
        vec3 leaf_dark = vec3(0.06,0.18,0.04);
        vec3 leaf_sun  = vec3(0.34,0.55,0.16);
        float clump = warped_fbm(wpos*0.5 + v_world.y*0.25);
        float spec  = noise(wpos*2.4 + v_world.y*1.7);
        float deep  = noise(wpos*0.9 - v_world.y*0.6);
        vec3 b = mix(leaf_lush, leaf_mid, clump);
        b = mix(b, leaf_dark, smoothstep(0.55,0.15,deep)*0.45);  // shaded pockets
        b = mix(b, leaf_sun,  smoothstep(0.62,0.85,spec)*0.40);  // sun-kissed tips
        // bias toward the per-block vertex colour so top faces stay brighter
        b = mix(b, vcol, 0.20);
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
    // Low-frequency micro/meso variation drives the palette's large biome
    // regions (no thin stripes).
    float micro = noise(wpos * 0.08);
    float meso  = noise(wpos * 0.015);

    // Resolve the continuous terrain surface colour (grass/dirt/rock blend +
    // wood/leaves/water/etc). This MUST run — the previous edit referenced an
    // undefined `base` and declared micro/meso twice, so the shader failed to
    // compile and terrain fell back to garbage/unlit colour.
    vec3 base = palette(v_mat, wpos, slope, micro, meso, n, v_color);

    // === Global lighting — bright, unified daylight ===
    float NdotL   = max(dot(n, normalize(u_sun_dir)), 0.0);
    float ambient = 0.55;                                  // bright fill
    float diffuse = NdotL * 0.70;                          // sun term
    float sky_amb = max(dot(n, vec3(0,1,0)), 0.0) * 0.18;  // sky bounce on tops
    // warm sun tint (golden hour feel) + cool sky bounce
    vec3 sun_color = vec3(1.06, 1.02, 0.95);
    vec3 sky_color = vec3(0.85, 0.92, 1.10);
    vec3 color = base * ambient
               + base * diffuse * sun_color
               + base * sky_amb * sky_color;

    // shadow approximation from large-scale height noise (terrain.frag)
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

    // Subtle height-based cool shift (MassRTS terrain.frag): terrain above sea
    // level picks up a faint cool tint so distant high ground reads as airy
    // rather than flat. Subsurface stays neutral.
    float above = clamp(v_world.y / 80.0, 0.0, 1.0);
    color = mix(color, color * vec3(0.92, 0.96, 1.04), above * 0.15);

    frag = vec4(color, u_alpha);
}
