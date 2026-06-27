#version 330 core
// SDFCraft — Realistic PBR-like terrain shader with triplanar texture mapping
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

// Texture samplers (loaded by chunk_renderer)
uniform sampler2D u_tex_grass;
uniform sampler2D u_tex_dirt;
uniform sampler2D u_tex_rock;
uniform sampler2D u_tex_rock2;
uniform sampler2D u_tex_sand;
uniform sampler2D u_tex_snow;
uniform sampler2D u_tex_gravel;
uniform sampler2D u_tex_mossy_rock;
uniform sampler2D u_tex_wood;
uniform sampler2D u_tex_bark;

out vec4 frag;

// Material codes (keep in sync with blocks.h)
const int MAT_GENERIC=0, MAT_GRASS=1, MAT_DIRT=2, MAT_ROCK=3, MAT_SAND=4,
          MAT_SNOW=5, MAT_WOOD=6, MAT_LEAVES=7, MAT_ORE=8, MAT_WATER=9, MAT_GRAVEL=10;

// === Triplanar mapping ===
vec3 tri_w(vec3 n) {
    vec3 w = pow(abs(n), vec3(4.0));
    return w / max(w.x + w.y + w.z, 1e-5);
}

vec3 triplanar(sampler2D tex, vec3 pos, vec3 w, float scale) {
    vec3 cx = texture(tex, pos.yz * scale).rgb;
    vec3 cy = texture(tex, pos.xz * scale).rgb;
    vec3 cz = texture(tex, pos.xy * scale).rgb;
    return cx * w.x + cy * w.y + cz * w.z;
}

// Two-scale triplanar: blend a detail scale with a large scale for richness
vec3 triplanar_detail(sampler2D tex, vec3 pos, vec3 w, float scale) {
    vec3 base = triplanar(tex, pos, w, scale);
    vec3 detail = triplanar(tex, pos, w, scale * 4.0);
    return base * 0.7 + detail * 0.3; // blend large + fine detail
}

// === Simple noise for variation (integer hash, no banding) ===
float hash(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p)) + 0x10000);
    uint  n = (q.x * 1597334673u) ^ (q.y * 3812015801u);
    n = (n ^ (n >> 15)) * 2246822519u;
    n = (n ^ (n >> 13));
    return float(n & 0x00FFFFFFu) / float(0x01000000u);
}
float noise(vec2 p) {
    vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x), mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}

// === Earthy terrain: grass -> dirt -> rock blended by DEPTH below surface ===
// `depth` is the per-vertex distance below the natural ground (0 at the lawn,
// growing as you dig). It is a smooth geometric value the GPU interpolates
// cleanly, so the grass/dirt/rock transitions are gradient-soft with no garish
// cross-material banding and no triangle-edge sawtooth. A little world-space
// noise jitters the thresholds so the boundaries read as organic, not as razor
// contour lines. Slope additionally exposes dirt/rock on steep cliff faces.
vec3 earthy_color(float depth, vec3 pos, vec3 n, float slope, vec3 w) {
    float ts = 0.25;
    vec3 grass = triplanar_detail(u_tex_grass, pos, w, ts);
    vec3 dirt  = triplanar_detail(u_tex_dirt,  pos, w, ts);
    vec3 rock  = triplanar_detail(u_tex_rock,  pos, w, ts * 0.8);
    vec3 mossy = triplanar_detail(u_tex_mossy_rock, pos, w, ts * 0.6);
    rock = mix(rock, mossy, noise(pos.xz * 0.04) * 0.35);
    vec3 deep  = triplanar_detail(u_tex_rock2, pos, w, ts * 0.5);

    // organic jitter so the grass/dirt line isn't a clean geometric contour
    float j  = (noise(pos.xz * 0.5) - 0.5) * 0.7;
    float j2 = (noise(pos.xz * 0.25 + 13.0) - 0.5) * 1.5;

    // depth-driven layer fractions (smooth)
    float toDirt = smoothstep(0.35 + j, 1.7 + j, depth);
    float toRock = smoothstep(3.0 + j2, 7.5 + j2, depth);
    // steep faces expose earth even at the surface (cliffs / pit walls)
    toDirt = max(toDirt, smoothstep(0.55, 0.80, slope));
    toRock = max(toRock, smoothstep(0.80, 0.96, slope));

    vec3 col = mix(grass, dirt, toDirt);
    col = mix(col, rock, toRock);
    // deepest strata shift to the darker rock2 texture
    col = mix(col, deep, smoothstep(10.0, 26.0, depth) * 0.6);
    return col;
}

// === Per-material colour (non-terrain materials) ===
vec3 get_material_color(int mat, vec3 pos, vec3 n, vec3 w) {
    float tex_scale = 0.25;

    if (mat == MAT_SAND) {
        return triplanar_detail(u_tex_sand, pos, w, tex_scale);
    }
    if (mat == MAT_SNOW) {
        return triplanar_detail(u_tex_snow, pos, w, tex_scale);
    }
    if (mat == MAT_GRAVEL) {
        return triplanar_detail(u_tex_gravel, pos, w, tex_scale);
    }
    if (mat == MAT_WOOD) {
        vec3 wood = triplanar_detail(u_tex_wood, pos, w, tex_scale);
        vec3 bark = triplanar_detail(u_tex_bark, pos, w, tex_scale);
        // vertical faces show bark, top shows wood grain
        float bark_blend = smoothstep(0.3, 0.7, 1.0 - abs(n.y));
        return mix(wood, bark, bark_blend);
    }
    if (mat == MAT_LEAVES) {
        // Rich green canopy
        vec3 leaf = vec3(0.12, 0.34, 0.08);
        vec3 leaf_b = vec3(0.20, 0.50, 0.14);
        float vary = noise(pos.xz * 0.5 + pos.y * 0.3);
        return mix(leaf, leaf_b, vary);
    }
    if (mat == MAT_ORE) {
        vec3 rock = triplanar_detail(u_tex_rock, pos, w, tex_scale);
        float sparkle = noise(pos.xz * 2.0 + pos.y * 1.5);
        return mix(rock, v_color * 1.5, smoothstep(0.6, 0.85, sparkle));
    }
    if (mat == MAT_WATER) {
        return vec3(0.08, 0.25, 0.45);
    }
    // Safety fallback for any unhandled / earthy code that reached the special
    // path (e.g. an old 200+ROCK snap): use a real rock texture, NOT flat
    // v_color, so it can never render as a grey untextured "sheet".
    if (mat == MAT_GENERIC || mat == MAT_GRASS || mat == MAT_DIRT || mat == MAT_ROCK)
        return triplanar_detail(u_tex_rock, pos, w, tex_scale * 0.8);
    return v_color;
}

void main() {
    vec3 n = normalize(v_normal);
    vec3 pos = v_world;
    float slope = 1.0 - abs(n.y);
    vec3 w = tri_w(n); // triplanar weights

    // Use actual normal for lighting — no smoothing to avoid seam artifacts
    vec3 light_n = n;

    // === Material colour ===
    // v_mat packs two things (set by the mesher): values < 100 are an EARTHY
    // DEPTH (grass/dirt/rock blended smoothly by how far below the surface we
    // are); values >= 200 are a snapped special material code (200 + MAT_*).
    int mat;
    vec3 base;
    if (v_mat >= 100.0) {
        mat = int(floor(v_mat - 200.0 + 0.5));   // special: ore/wood/leaves/water/sand/snow/gravel
        base = get_material_color(mat, pos, n, w);
    } else {
        mat = MAT_DIRT;                          // earthy sentinel (no specular)
        base = earthy_color(v_mat, pos, n, slope, w);   // v_mat == depth below surface
    }

    // === PBR-like lighting ===
    vec3 to_sun = normalize(u_sun_dir);
    vec3 to_cam = normalize(u_cam - pos);
    vec3 half_vec = normalize(to_sun + to_cam);

    // Hemisphere ambient: sky blue from above, warm bounce from below. The
    // lower hemisphere is kept fairly bright so down-facing pit walls and the
    // ring just outside a dug hole are softly lit, not dead black.
    float sky_factor = light_n.y * 0.5 + 0.5;
    vec3 sky_amb = mix(vec3(0.42, 0.40, 0.38), vec3(0.55, 0.65, 0.85), sky_factor);
    float ambient_strength = 0.6; // brighter fill so caves/pits stay readable
    // Lambertian diffuse with a high shadow floor so nothing crushes to black.
    float NdotL = max(dot(light_n, to_sun), 0.0);
    float diffuse_val = NdotL * 0.55 + 0.25; // shadow floor raised from 0.15
    vec3 sun_color = vec3(1.1, 1.0, 0.9);

    // Blinn-Phong specular — ONLY water is shiny. Rock/dirt/ore are rough,
    // matte earth; giving them a specular lobe made dug pit walls look like wet
    // polished plastic ("反光太假"). Terrain stays purely diffuse.
    float NdotH = max(dot(light_n, half_vec), 0.0);
    float spec_power = 64.0;
    float spec_strength = 0.0;
    if (mat == MAT_WATER) {
        spec_strength = 0.6;
        spec_power = 64.0;
    }
    float spec = pow(NdotH, spec_power) * spec_strength * step(0.0, NdotL);

    // Combine lighting
    vec3 color = base * ambient_strength * sky_amb
               + base * diffuse_val * sun_color
               + vec3(1.0, 0.98, 0.92) * spec;

    // Rim light REMOVED. `sky_amb*rim` lit the silhouette of every thin edge a
    // glowing blue-grey. On the thin grass shelves a dig leaves between carve
    // spheres, that glow framed them like a bright bordered card — exactly the
    // "weird polygon" floating in a hole. Without the rim the shelf just reads as
    // ordinary grass/dirt terrain.

    // NOTE: the old "cavity AO" (color *= 1 - max(0,-n.y)*0.12) darkened every
    // down-facing face. On a fresh dig the overhang/rim around the hole faces
    // partly downward, so it turned pitch-black — the "黑一圈" ring. Removed; the
    // hemisphere ambient already gives a gentle, even darkening to concavities.

    // === Atmospheric fog (distance-based, cool blue) ===
    float d = length(pos - u_cam);
    float fog = clamp((d - u_fog_start) / max(u_fog_end - u_fog_start, 1.0), 0.0, 1.0);
    fog = fog * fog; // quadratic falloff for softer horizon
    vec3 fog_sky = mix(u_fog_color, vec3(0.65, 0.78, 0.95), 0.3); // slightly bluer
    color = mix(color, fog_sky, fog);

    // Slight exposure / tone mapping for HDR-like feel
    color = color / (color + vec3(1.0)) * 1.15; // Reinhard tonemap + exposure boost

    frag = vec4(color, u_alpha);
}
