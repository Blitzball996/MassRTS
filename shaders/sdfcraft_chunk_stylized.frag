#version 330 core
// SDFCraft - Stylized/Toon shader (Genshin Impact / 原神 style)
// Bright, saturated colors with cel-shaded lighting
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

// Material codes
const int MAT_GENERIC=0, MAT_GRASS=1, MAT_DIRT=2, MAT_ROCK=3, MAT_SAND=4,
          MAT_SNOW=5, MAT_WATER=6, MAT_WOOD=7, MAT_LEAVES=8, MAT_GRAVEL=9, MAT_ORE=10;

// Simple noise for texture variation
float hash(vec2 p) {
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i), b = hash(i+vec2(1,0)), c = hash(i+vec2(0,1)), d = hash(i+vec2(1,1));
    return mix(mix(a,b,f.x), mix(c,d,f.x), f.y);
}

// Multi-octave noise
float fbm(vec2 p) {
    float val = 0.0, amp = 0.5;
    for (int i = 0; i < 3; i++) {
        val += amp * noise(p);
        p *= 2.0;
        amp *= 0.5;
    }
    return val;
}

// === Stylized material colors (bright and saturated) ===
vec3 get_base_color(int mat, vec3 wpos) {
    float depth = -wpos.y; // below sea level
    vec2 uv = wpos.xz * 0.1;
    float n = fbm(uv);
    
    if (mat == MAT_GRASS) {
        // Vibrant green grass (Genshin-style)
        vec3 grass_bright = vec3(0.45, 0.75, 0.35);
        vec3 grass_dark = vec3(0.35, 0.65, 0.25);
        return mix(grass_dark, grass_bright, n);
    }
    
    if (mat == MAT_DIRT) {
        // Warm brown earth
        vec3 dirt_a = vec3(0.55, 0.40, 0.25);
        vec3 dirt_b = vec3(0.45, 0.32, 0.20);
        
        // Underground gets darker and more layered
        if (depth > 5.0) {
            float layer = sin(wpos.y * 0.3 + n * 2.0) * 0.5 + 0.5;
            dirt_a = mix(vec3(0.35, 0.28, 0.20), vec3(0.42, 0.35, 0.25), layer);
            dirt_b = dirt_a * 0.85;
        }
        
        return mix(dirt_b, dirt_a, n);
    }
    
    if (mat == MAT_ROCK) {
        // Cool gray stone (lighter than realistic)
        vec3 rock_a = vec3(0.60, 0.62, 0.65);
        vec3 rock_b = vec3(0.50, 0.52, 0.55);
        
        // Deep underground: darker sedimentary layers
        if (depth > 30.0) {
            float strata = sin(wpos.y * 0.2) * 0.5 + 0.5;
            rock_a = mix(vec3(0.35, 0.36, 0.38), vec3(0.45, 0.46, 0.48), strata);
            rock_b = rock_a * 0.9;
        }
        
        return mix(rock_b, rock_a, n);
    }
    
    if (mat == MAT_SAND) {
        vec3 sand_a = vec3(0.90, 0.82, 0.60);
        vec3 sand_b = vec3(0.82, 0.75, 0.55);
        return mix(sand_b, sand_a, n);
    }
    
    if (mat == MAT_SNOW) {
        vec3 snow = vec3(0.95, 0.96, 0.98);
        return snow * (0.95 + n * 0.05);
    }
    
    if (mat == MAT_GRAVEL) {
        vec3 gravel = vec3(0.50, 0.48, 0.46);
        return gravel * (0.90 + n * 0.10);
    }
    
    if (mat == MAT_ORE) {
        // Sparkling ore veins
        vec3 rock = vec3(0.40, 0.42, 0.45);
        float sparkle = noise(uv * 5.0);
        return mix(rock, v_color * 1.3, smoothstep(0.6, 0.85, sparkle));
    }
    
    if (mat == MAT_WOOD) {
        vec3 wood = vec3(0.52, 0.38, 0.25);
        float grain = sin(wpos.y * 4.0 + n * 3.0) * 0.5 + 0.5;
        return wood * (0.85 + grain * 0.15);
    }
    
    if (mat == MAT_LEAVES) {
        vec3 leaf_a = vec3(0.35, 0.70, 0.30);
        vec3 leaf_b = vec3(0.40, 0.80, 0.35);
        return mix(leaf_a, leaf_b, fbm(uv * 2.0));
    }
    
    if (mat == MAT_WATER) {
        return vec3(0.20, 0.50, 0.80); // bright blue water
    }
    
    return v_color; // fallback
}

void main() {
    vec3 n = normalize(v_normal);
    vec3 to_sun = normalize(u_sun_dir);
    vec3 to_cam = normalize(u_cam - v_world);
    
    int mat = int(floor(v_mat));
    vec3 base_color = get_base_color(mat, v_world);
    
    // === Cel-shaded lighting (toon/anime style) ===
    float NdotL = dot(n, to_sun);
    
    // Multi-tone shading (3 levels: shadow, mid, light)
    float light_level;
    if (NdotL > 0.6) {
        light_level = 1.0; // bright
    } else if (NdotL > 0.0) {
        light_level = 0.7; // mid-tone
    } else {
        light_level = 0.45; // shadow
    }
    
    // Sky light (hemisphere ambient - bright!)
    float sky_factor = n.y * 0.5 + 0.5;
    vec3 sky_amb = mix(vec3(0.50, 0.48, 0.55), vec3(0.70, 0.80, 1.00), sky_factor);
    
    // Combine
    vec3 color = base_color * (light_level * 0.75 + 0.25) * (vec3(1.0, 0.98, 0.92) * 0.8 + sky_amb * 0.2);
    
    // Specular highlights (water, snow, ore)
    if (mat == MAT_WATER || mat == MAT_SNOW || mat == MAT_ORE) {
        vec3 half_vec = normalize(to_sun + to_cam);
        float spec = pow(max(0.0, dot(n, half_vec)), mat == MAT_WATER ? 32.0 : 16.0);
        float strength = (mat == MAT_WATER) ? 0.6 : 0.3;
        color += vec3(1.0, 1.0, 0.95) * spec * strength * step(0.0, NdotL);
    }
    
    // Rim light (edge glow for depth)
    float rim = pow(1.0 - max(0.0, dot(n, to_cam)), 2.5);
    color += sky_amb * rim * 0.25;
    
    // === Atmospheric fog (aerial perspective - distance fades to sky blue) ===
    float dist = length(v_world - u_cam);
    float fog_factor = smoothstep(u_fog_start, u_fog_end, dist);
    vec3 fog_sky = vec3(0.70, 0.85, 1.00); // bright sky blue
    color = mix(color, fog_sky, fog_factor);
    
    // Boost saturation (Genshin-style vibrant)
    vec3 luma_coeff = vec3(0.299, 0.587, 0.114);
    float luma = dot(color, luma_coeff);
    color = mix(vec3(luma), color, 1.25); // +25% saturation
    
    // Slightly boost brightness
    color = pow(color, vec3(0.95)) * 1.08;
    
    frag = vec4(color, u_alpha);
}
