#version 330 core
in vec3 v_world;
in float v_depth;
in vec2 v_vel;
in float v_dry;
out vec4 frag_color;

uniform vec3 u_cam_pos;
uniform float u_time;

// Animated normal from layered scrolling ripples at several scales. Multi-scale
// detail breaks up the coarse grid so the surface reads as continuous water
// rather than faceted triangles.
vec3 water_normal(vec2 p, vec2 flow) {
    vec2 f = flow * 0.5;
    vec2 g = vec2(0.0);
    // large swell
    g += vec2(sin(p.x*0.020 + u_time*1.1 + f.x), cos(p.y*0.022 - u_time*0.9 + f.y)) * 0.10;
    // medium ripples
    g += vec2(sin(p.y*0.070 - u_time*1.8), cos(p.x*0.065 + u_time*1.5)) * 0.06;
    // fine chop
    g += vec2(sin(p.x*0.150 + u_time*2.6), cos(p.y*0.160 - u_time*2.3)) * 0.03;
    return normalize(vec3(-g.x, 1.0, -g.y));
}

void main() {
    // Break up the grid-aligned shoreline: instead of a hard per-cell wet/dry
    // cut (which makes axis-aligned "right-angle" edges), erode the edge with
    // noise so the waterline meanders naturally between grid cells.
    float edge_noise = sin(v_world.x*0.18)*cos(v_world.z*0.21)*0.5 + 0.5;
    edge_noise = mix(edge_noise,
        fract(sin(dot(floor(v_world.xz*0.6), vec2(27.1,61.7)))*43758.5), 0.5);
    float effective_depth = v_depth - edge_noise * 0.12;
    if (v_dry > 0.5 || effective_depth < 0.0) discard; // no water here

    vec3 N = water_normal(v_world.xz, v_vel);
    vec3 V = normalize(u_cam_pos - v_world);

    // Fresnel: glancing angles reflect the sky, steep angles show the water body.
    float fres = pow(1.0 - max(dot(N, V), 0.0), 5.0);
    fres = 0.03 + 0.97 * fres;

    // Depth gradient: shallow = bright teal-green, deep = dark blue. Smooth curve
    // (not a hard step) so it never looks like a flat plastic sheet.
    float d = clamp(v_depth / 14.0, 0.0, 1.0);
    d = sqrt(d); // bias toward showing depth variation early
    vec3 shallow = vec3(0.18, 0.55, 0.55);
    vec3 mid     = vec3(0.05, 0.30, 0.45);
    vec3 deep    = vec3(0.01, 0.06, 0.18);
    vec3 body = mix(mix(shallow, mid, smoothstep(0.0, 0.5, d)),
                    deep, smoothstep(0.5, 1.0, d));

    // Sky reflection (hemisphere gradient + horizon warmth).
    vec3 R = reflect(-V, N);
    vec3 sky = mix(vec3(0.62, 0.72, 0.85), vec3(0.85, 0.91, 0.97), clamp(R.y, 0.0, 1.0));

    vec3 col = mix(body, sky, fres);

    // Sharp sun glint = "波光粼粼". Two specular lobes (tight + broad) on the
    // rippled normal so the highlight shimmers across the surface.
    vec3 L = normalize(vec3(0.4, 0.82, 0.35));
    vec3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    float glint = pow(NdotH, 200.0) * 1.6 + pow(NdotH, 32.0) * 0.35;
    col += vec3(1.2, 1.12, 0.95) * glint; // HDR so bloom catches it

    // Foam: fast flow OR very thin water (shoreline / overspill). Broken up with
    // noise so it isn't a hard white band at the water's edge.
    float speed = length(v_vel);
    float flow_foam = smoothstep(2.5, 8.0, speed);
    float shore_foam = smoothstep(0.7, 0.05, v_depth);
    float foam = max(flow_foam, shore_foam);
    float n = 0.5 + 0.5*sin(v_world.x*0.3 + u_time*2.0)
                  * cos(v_world.z*0.27 - u_time*1.6);
    n = mix(n, fract(sin(dot(floor(v_world.xz*0.4), vec2(12.99,78.23)))*43758.5), 0.4);
    foam *= 0.55 + 0.45*clamp(n, 0.0, 1.0);
    col = mix(col, vec3(0.92, 0.95, 0.97), clamp(foam, 0.0, 0.85));

    // Alpha: shallow water transparent, deep opaque; foam opaque.
    float alpha = mix(0.40, 0.94, d);
    alpha = max(alpha, clamp(foam, 0.0, 0.9));

    frag_color = vec4(col, alpha);
}
