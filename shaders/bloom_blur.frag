#version 330 core
// Separable Gaussian blur. Run once horizontal, once vertical per mip level.
// 9-tap with precomputed weights; u_direction selects axis, u_texel = 1/size.
in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_tex;
uniform vec2 u_direction; // (1,0) horizontal or (0,1) vertical
uniform vec2 u_texel;     // 1.0 / texture size

void main() {
    float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 result = texture(u_tex, v_uv).rgb * w[0];
    for (int i = 1; i < 5; i++) {
        vec2 off = u_direction * u_texel * float(i);
        result += texture(u_tex, v_uv + off).rgb * w[i];
        result += texture(u_tex, v_uv - off).rgb * w[i];
    }
    frag_color = vec4(result, 1.0);
}
