#version 330 core
// Final composite: combine HDR scene + bloom, apply exposure, ACES filmic tone
// mapping (HDR -> LDR), then gamma correction. This is the "head swap" that lets
// emissive >1.0 values actually glow instead of clipping flat to white.
in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;   // HDR scene color
uniform sampler2D u_bloom;   // blurred bright-pass
uniform float u_exposure;    // exposure multiplier (e.g. 1.0)
uniform float u_bloom_strength; // additive bloom amount (e.g. 0.6)

// ACES filmic approximation (Narkowicz 2015)
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Ordered dither: breaks up the horizontal/arc color BANDING that appears when a
// smooth HDR gradient (sky, lighting falloff) is quantized into an 8-bit
// framebuffer after tonemapping. Adds ~1/255 of triangular noise per pixel so
// the eye reads a smooth gradient instead of stair-step stripes.
float dither(vec2 uv) {
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec3 hdr = texture(u_scene, v_uv).rgb;
    vec3 bloom = texture(u_bloom, v_uv).rgb;
    hdr += bloom * u_bloom_strength;
    hdr *= u_exposure;
    vec3 mapped = aces(hdr);
    mapped = pow(mapped, vec3(1.0 / 2.2)); // gamma

    // === Color grade (LUT-style, done analytically) ===
    // 1) Contrast S-curve around mid-gray.
    mapped = clamp((mapped - 0.5) * 1.10 + 0.5, 0.0, 1.0);
    // 2) Gentle split-tone: cool shadows, very slightly warm highlights. Kept
    //    near-neutral so the whole frame doesn't turn yellow/orange.
    float luma = dot(mapped, vec3(0.299, 0.587, 0.114));
    vec3 shadow_tint = vec3(0.98, 1.00, 1.04);   // faint cool
    vec3 light_tint  = vec3(1.02, 1.00, 0.98);   // faint warm
    mapped *= mix(shadow_tint, light_tint, smoothstep(0.2, 0.8, luma));
    // 3) Vibrance.
    float sat = mix(1.12, 1.04, smoothstep(0.0, 0.6, luma));
    mapped = clamp(mix(vec3(luma), mapped, sat), 0.0, 1.0);
    // 4) Subtle vignette.
    vec2 vc = v_uv - 0.5;
    float vig = 1.0 - dot(vc, vc) * 0.45;
    mapped *= vig;

    // Triangular dither (two samples -> TPDF) to kill banding.
    float dd = dither(v_uv) - dither(v_uv + 0.5);
    mapped += dd * (1.0 / 255.0);

    frag_color = vec4(mapped, 1.0);
}
