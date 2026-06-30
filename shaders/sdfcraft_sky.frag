#version 330 core
// ============================================================================
// SDFCraft sky dome — bright, clean gradient atmosphere with a soft sun disk,
// horizon haze and lightly-shaded drifting clouds. Tuned to feel close to the
// MassRTS_GPU overworld sky (sun glow + cloud lighting + gentle tonemap) while
// staying a cheap single-pass dome (no atmospheric raymarch needed here).
// ============================================================================
in vec3 v_dir;
out vec4 frag;

uniform vec3  u_sun_dir;   // normalized, pointing TOWARD the sun
uniform float u_time;      // seconds, for cloud drift
uniform int   u_hdr_out;   // 1 = output linear HDR (PostFX composites); 0 = tonemap inline

// --- value noise --------------------------------------------------------------
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i),          hash(i+vec2(1,0)), f.x),
               mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)), f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) { v += a*noise(p); p *= 2.03; a *= 0.5; }
    return v;
}

void main() {
    vec3 dir = normalize(v_dir);
    float el  = clamp(dir.y, -1.0, 1.0);   // -1 down, 0 horizon, 1 up
    vec3 sun  = normalize(u_sun_dir);
    float sun_h = clamp(sun.y * 0.5 + 0.5, 0.0, 1.0);   // 0 night-ish, 1 noon

    // --- sky gradient: deep zenith blue -> pale horizon, warmed at low sun ----
    vec3 zenith  = vec3(0.20, 0.44, 0.86);
    vec3 mid     = vec3(0.42, 0.66, 0.95);
    vec3 horizon = vec3(0.78, 0.88, 0.98);
    float t = pow(clamp(el, 0.0, 1.0), 0.55);
    vec3 sky = mix(horizon, mid, smoothstep(0.0, 0.45, el));
    sky = mix(sky, zenith, smoothstep(0.30, 1.0, t));

    // below the horizon line, fade to a soft ground haze so the dome never
    // shows a hard band where it meets distant terrain fog.
    vec3 ground_haze = vec3(0.66, 0.72, 0.74);
    sky = mix(sky, ground_haze, smoothstep(0.02, -0.25, el));

    // warm the whole sky toward sunset colours when the sun is low
    float golden = smoothstep(0.35, 0.02, sun.y);
    vec3 warm = vec3(1.0, 0.62, 0.36);
    sky = mix(sky, sky*0.6 + warm*0.6, golden * smoothstep(-0.1, 0.5, el) * 0.5);

    // --- sun: warm glow halo + crisp disk -----------------------------------
    float sd = max(dot(dir, sun), 0.0);
    float glow = pow(sd, 8.0) * 0.25 + pow(sd, 256.0) * 0.6;
    vec3 sun_col = mix(vec3(1.0, 0.78, 0.5), vec3(1.0, 0.96, 0.86), sun_h);
    sky += sun_col * glow;
    float disk = smoothstep(0.9988, 0.9994, sd);
    sky += sun_col * disk * 3.0;

    // --- clouds: project onto a high plane, fbm coverage, cheap sun shading --
    if (el > 0.0) {
        float plane = 1.0;
        float dist = plane / max(el, 0.04);              // ray->cloud plane
        vec2 uv = dir.xz * dist * 1.4 + u_time * 0.010;  // slow drift
        float d  = fbm(uv);
        float ds = fbm(uv + sun.xz * 0.25);              // sun-offset sample
        float cover = smoothstep(0.50, 0.74, d);
        float shade = clamp((d - ds) * 2.5, 0.0, 1.0);   // self-shadow
        // fade clouds out toward the horizon so the plane edge is hidden
        float hfade = smoothstep(0.04, 0.35, el);
        cover *= hfade;
        vec3 lit  = mix(vec3(1.0,0.95,0.88), vec3(1.0), sun_h);
        vec3 dark = mix(vec3(0.50,0.52,0.60), vec3(0.62,0.66,0.74), sun_h);
        vec3 ccol = mix(lit, dark, shade);
        sky = mix(sky, ccol, cover * 0.9);
    }

    // --- day/night: sink the whole dome toward a dark night-blue as the sun
    // drops below the horizon, so the sky doesn't stay bright-blue at midnight
    // (which made night read like an overcast day once the HDR gamma fix landed).
    // The sun disk/glow above were added before this, so they correctly fade out
    // with the sun too. A small floor keeps a faint moonlit sky rather than pure
    // black.
    float dayf = smoothstep(-0.15, 0.20, sun.y);
    vec3 night_sky = vec3(0.015, 0.03, 0.075);
    sky = mix(night_sky, sky, mix(0.06, 1.0, dayf));

    // With PostFX (u_hdr_out=1) the dome emits LINEAR HDR: the gradient values
    // are authored display-referred, so we hand them over roughly as-is and let
    // the composite's single ACES+gamma map the whole frame consistently — and
    // the sun disk/glow (already >1.0) blooms. Without PostFX we apply the old
    // soft-shoulder tonemap + gamma here so the dome looks right on the backbuffer.
    if (u_hdr_out == 1) {
        frag = vec4(sky, 1.0);
    } else {
        sky = sky / (sky + vec3(0.18));         // soft shoulder
        sky *= 1.10;
        sky = pow(clamp(sky, 0.0, 1.0), vec3(0.92));
        frag = vec4(sky, 1.0);
    }
}
