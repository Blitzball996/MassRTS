#version 330 core
// ============================================================================
// Procedural sky: physically-based atmospheric scattering (Rayleigh + Mie),
// a soft sun disk, and raymarched volumetric clouds. This is an original
// implementation of the standard public single-scattering model (the same
// family used by Bruneton/Preetham-style skies), tuned for an RTS overworld.
// ============================================================================
in vec3 v_ray;
out vec4 frag_color;

uniform vec3  u_sun_dir;     // normalized, pointing TOWARD the sun
uniform float u_time;        // seconds, for cloud animation
uniform vec3  u_cam_pos;     // world camera position (for cloud parallax)

// --- Atmosphere constants (Earth-like, scaled) ------------------------------
const float PI = 3.14159265359;
const float ATMO_R   = 6420e3;   // atmosphere top radius
const float PLANET_R = 6360e3;   // planet radius
const vec3  RAY_BETA = vec3(5.5e-6, 13.0e-6, 22.4e-6); // Rayleigh scatter (RGB)
const vec3  MIE_BETA = vec3(21e-6);                     // Mie scatter
const float RAY_H = 8000.0;      // Rayleigh scale height
const float MIE_H = 1200.0;      // Mie scale height
const float MIE_G = 0.758;       // Mie anisotropy (forward scattering)
const float SUN_I = 22.0;        // sun intensity

const int  STEPS = 16;           // primary scatter samples
const int  LSTEPS = 8;           // light (optical depth) samples

// Ray-sphere intersection (returns near/far t along ray from origin).
vec2 ray_sphere(vec3 o, vec3 d, float r) {
    float b = dot(o, d);
    float c = dot(o, o) - r * r;
    float h = b * b - c;
    if (h < 0.0) return vec2(1e9, -1e9);
    h = sqrt(h);
    return vec2(-b - h, -b + h);
}

// Single-scattering integral along the view ray.
vec3 atmosphere(vec3 ray, vec3 sun_dir) {
    // Camera sits a little above the planet surface.
    vec3 orig = vec3(0.0, PLANET_R + 1500.0, 0.0);
    vec2 t = ray_sphere(orig, ray, ATMO_R);
    if (t.x > t.y) return vec3(0.0);
    t.y = min(t.y, ray_sphere(orig, ray, PLANET_R).x > 0.0
                   ? ray_sphere(orig, ray, PLANET_R).x : t.y);
    float seg = (t.y - t.x) / float(STEPS);
    float tcur = t.x;

    vec3 sum_r = vec3(0.0);
    vec3 sum_m = vec3(0.0);
    float od_r = 0.0, od_m = 0.0; // accumulated optical depth

    float mu = dot(ray, sun_dir);
    // Rayleigh + Mie (Henyey-Greenstein) phase functions.
    float phase_r = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
    float g = MIE_G;
    float phase_m = 3.0 / (8.0 * PI) *
        ((1.0 - g * g) * (1.0 + mu * mu)) /
        ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));

    for (int i = 0; i < STEPS; i++) {
        vec3 sp = orig + ray * (tcur + seg * 0.5);
        float h = length(sp) - PLANET_R;
        float hr = exp(-h / RAY_H) * seg;
        float hm = exp(-h / MIE_H) * seg;
        od_r += hr; od_m += hm;

        // Optical depth toward the sun (light ray).
        vec2 tl = ray_sphere(sp, sun_dir, ATMO_R);
        float lseg = tl.y / float(LSTEPS);
        float lr = 0.0, lm = 0.0, tl_cur = 0.0;
        bool lit = true;
        for (int j = 0; j < LSTEPS; j++) {
            vec3 lp = sp + sun_dir * (tl_cur + lseg * 0.5);
            float lh = length(lp) - PLANET_R;
            if (lh < 0.0) { lit = false; break; }
            lr += exp(-lh / RAY_H) * lseg;
            lm += exp(-lh / MIE_H) * lseg;
            tl_cur += lseg;
        }
        if (lit) {
            vec3 tau = RAY_BETA * (od_r + lr) + MIE_BETA * 1.1 * (od_m + lm);
            vec3 att = exp(-tau);
            sum_r += att * hr;
            sum_m += att * hm;
        }
        tcur += seg;
    }
    return SUN_I * (sum_r * RAY_BETA * phase_r + sum_m * MIE_BETA * phase_m);
}

// --- Volumetric clouds (lightweight 2.5D layer) -----------------------------
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i), b = hash(i + vec2(1, 0));
    float c = hash(i + vec2(0, 1)), d = hash(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) { v += a * noise(p); p *= 2.03; a *= 0.5; }
    return v;
}

// Project the view ray onto a high cloud plane and sample animated fbm.
float clouds(vec3 ray, vec3 sun_dir, out float shade) {
    shade = 0.0;
    if (ray.y < 0.02) return 0.0;            // below horizon -> no clouds
    float plane_h = 2600.0;
    float t = plane_h / ray.y;               // distance to the cloud plane
    vec2 uv = (u_cam_pos.xz + ray.xz * t) * 0.00035;
    uv += u_time * 0.006;                    // slow drift
    float d = fbm(uv);
    float cover = smoothstep(0.50, 0.78, d); // coverage threshold
    // Cheap self-shadow: compare against a sun-offset sample.
    float ds = fbm(uv + sun_dir.xz * 0.04);
    shade = clamp((d - ds) * 3.0, 0.0, 1.0);
    // Fade clouds out toward the horizon so the plane edge is hidden.
    float horizon = smoothstep(0.02, 0.30, ray.y);
    return cover * horizon;
}

void main() {
    vec3 ray = normalize(v_ray);
    vec3 sun = normalize(u_sun_dir);

    // Atmosphere.
    vec3 col = atmosphere(ray, sun);

    // Sun disk + glow.
    float sd = dot(ray, sun);
    float disk = smoothstep(0.9995, 0.9998, sd);
    float glow = pow(max(sd, 0.0), 256.0) * 0.3 + pow(max(sd, 0.0), 8.0) * 0.05;
    col += vec3(1.0, 0.95, 0.85) * (disk * 30.0 + glow);

    // Clouds: lit top, shaded base, composited over the sky.
    float shade;
    float c = clouds(ray, sun, shade);
    if (c > 0.0) {
        float sun_h = clamp(sun.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 lit  = mix(vec3(1.0), vec3(1.0, 0.9, 0.78), 1.0 - sun_h);
        vec3 dark = mix(vec3(0.35, 0.38, 0.45), vec3(0.2, 0.18, 0.25), 1.0 - sun_h);
        vec3 ccol = mix(lit, dark, shade);
        col = mix(col, ccol, c * 0.92);
    }

    // Tonemap (ACES-ish) + gamma.
    col = (col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14);
    col = pow(clamp(col, 0.0, 1.0), vec3(1.0 / 2.2));
    frag_color = vec4(col, 1.0);
}
