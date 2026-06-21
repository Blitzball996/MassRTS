#version 330 core
// Particle fragment shader: lifecycle curves (color/alpha fade), soft particles
// (depth-fade), and procedural fire-to-smoke animation (placeholder for flipbook
// texture atlas when CC0 assets are added). This is the "quality change" that
// makes explosions/smoke look like actual VFX instead of colored bubbles.
in vec3 v_color;
in vec2 v_uv;
in float v_age;
in vec4 v_clip_pos;
out vec4 frag_color;

uniform sampler2D u_depth_texture; // scene depth for soft particles
uniform float u_near;
uniform float u_far;

// Procedural noise for fire/smoke texture (until we have a real flipbook atlas)
float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1,0)), f.x),
               mix(hash(i + vec2(0,1)), hash(i + vec2(1,1)), f.x), f.y);
}

float linearize_depth(float d) {
    return (2.0 * u_near) / (u_far + u_near - d * (u_far - u_near));
}

void main() {
    // Radial distance from center (for shape)
    vec2 centered = v_uv - 0.5;
    float dist = length(centered) * 2.0;
    
    // Procedural smoke/fire texture (animated by age)
    vec2 tex_coord = v_uv * 2.0 + v_age * 0.5;
    float turb = noise(tex_coord * 4.0 + v_age * 2.0) * 0.6
               + noise(tex_coord * 8.0 - v_age * 3.0) * 0.3;
    float shape = 1.0 - smoothstep(0.3, 1.0, dist + turb * 0.3);
    if (shape < 0.02) discard;
    
    // Lifecycle color curve: bright yellow/orange (fire core) -> dull gray (smoke)
    vec3 fire_core = vec3(1.5, 0.9, 0.3);   // HDR emissive (>1.0, bloom will glow)
    vec3 fire_mid = vec3(1.0, 0.5, 0.1);
    vec3 smoke_dark = vec3(0.25, 0.25, 0.28);
    vec3 color;
    if (v_age < 0.3) {
        color = mix(fire_core, fire_mid, v_age / 0.3);
    } else {
        color = mix(fire_mid, smoke_dark, (v_age - 0.3) / 0.7);
    }
    color = mix(color, v_color, 0.3); // tint by emitter color
    
    // Lifecycle alpha curve: fade in quickly, hold, fade out
    float alpha = 1.0;
    if (v_age < 0.1) alpha = v_age / 0.1;
    else if (v_age > 0.7) alpha = (1.0 - v_age) / 0.3;
    alpha *= shape;
    
    // Soft particles: depth-fade against scene geometry so smoke doesn't hard-cut
    // into terrain/units (the single biggest visual upgrade for particle quality).
    vec2 screen_uv = (v_clip_pos.xy / v_clip_pos.w) * 0.5 + 0.5;
    float scene_depth_ndc = texture(u_depth_texture, screen_uv).r;
    float scene_depth_linear = linearize_depth(scene_depth_ndc);
    float particle_depth_linear = linearize_depth(gl_FragCoord.z);
    float depth_diff = (scene_depth_linear - particle_depth_linear) * u_far;
    float soft = smoothstep(0.0, 2.0, depth_diff); // fade over ~2 units
    alpha *= soft;
    
    if (alpha < 0.02) discard;
    frag_color = vec4(color, alpha * 0.9);
}
