#version 330 core
in vec3 v_normal;
in vec3 v_world_pos;
in vec2 v_uv;
out vec4 frag_color;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i), hash(i+vec2(1,0)), f.x),
               mix(hash(i+vec2(0,1)), hash(i+vec2(1,1)), f.x), f.y);
}

void main() {
    // Grass color with procedural variation
    float n1 = noise(v_world_pos.xz * 0.01);
    float n2 = noise(v_world_pos.xz * 0.05);
    vec3 grass_dark = vec3(0.12, 0.28, 0.08);
    vec3 grass_light = vec3(0.22, 0.48, 0.14);
    vec3 base = mix(grass_dark, grass_light, n1 * 0.7 + n2 * 0.3);

    // Height-based coloring: higher = rocky
    float height_factor = clamp(v_world_pos.y / 40.0, 0.0, 1.0);
    vec3 rock = vec3(0.35, 0.3, 0.25);
    base = mix(base, rock, height_factor * 0.6);

    // Simple directional lighting
    vec3 light_dir = normalize(vec3(0.4, 0.8, 0.3));
    float diffuse = max(dot(normalize(v_normal), light_dir), 0.0);
    float ambient = 0.35;
    vec3 color = base * (ambient + diffuse * 0.65);

    // Subtle grid
    vec2 grid = abs(fract(v_world_pos.xz / 100.0) - 0.5);
    float line = 1.0 - smoothstep(0.0, 0.015, min(grid.x, grid.y));
    color += vec3(0.05) * line;

    // Distance fog
    float dist = length(v_world_pos.xz) / 1500.0;
    vec3 fog = vec3(0.4, 0.5, 0.55);
    color = mix(color, fog, clamp(dist * dist, 0.0, 0.6));

    frag_color = vec4(color, 1.0);
}
