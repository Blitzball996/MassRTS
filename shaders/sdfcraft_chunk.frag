#version 330 core
in vec3 v_color;
in vec3 v_normal;
in vec3 v_world;

uniform vec3  u_sun_dir;    // normalized, points toward the sun
uniform vec3  u_cam;
uniform float u_alpha;      // 1.0 opaque pass, <1.0 transparent pass
uniform vec3  u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;

out vec4 frag;

void main() {
    // Per-vertex color already carries directional face shade from the mesher;
    // add a soft sun term + ambient so lighting reads cleanly without textures.
    float ndl = max(dot(normalize(v_normal), u_sun_dir), 0.0);
    float light = 0.45 + 0.55 * ndl;
    vec3 col = v_color * light;

    // distance fog toward the sky color (hides chunk pop-in at the load radius)
    float d = length(v_world - u_cam);
    float fog = clamp((d - u_fog_start) / max(u_fog_end - u_fog_start, 1.0), 0.0, 1.0);
    col = mix(col, u_fog_color, fog);

    frag = vec4(col, u_alpha);
}
