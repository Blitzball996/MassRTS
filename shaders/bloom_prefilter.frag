#version 330 core
// Bright-pass prefilter: extract pixels above a luminance threshold so only the
// genuinely "hot" parts of the HDR scene (fire cores, projectile glow, sun
// glints) feed the bloom blur. Soft knee avoids a hard cutoff that flickers.
in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;   // HDR scene color
uniform float u_threshold;   // luminance threshold (e.g. 1.0)
uniform float u_knee;        // soft knee width (e.g. 0.5)

void main() {
    vec3 c = texture(u_scene, v_uv).rgb;
    float br = max(c.r, max(c.g, c.b));
    // Soft-knee curve (Karis / COD style)
    float knee = u_knee;
    float soft = br - u_threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float contrib = max(soft, br - u_threshold) / max(br, 1e-4);
    frag_color = vec4(c * contrib, 1.0);
}
