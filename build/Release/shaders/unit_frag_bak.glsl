#version 330 core
in vec3 v_color;
in vec3 v_normal;
in vec3 v_world_pos;
out vec4 frag_color;

void main() {
    // Directional sunlight
    vec3 light_dir = normalize(vec3(0.3, 0.8, 0.4));
    vec3 n = normalize(v_normal);
    float diffuse = max(dot(n, light_dir), 0.0);
    float ambient = 0.35;
    
    vec3 color = v_color * (ambient + diffuse * 0.65);
    
    // Subtle rim light for visibility
    vec3 view_dir = normalize(vec3(0.2, 0.8, 0.3));
    float rim = pow(1.0 - max(dot(n, view_dir), 0.0), 3.0) * 0.2;
    color += vec3(rim);
    
    // Minecraft-style: slightly flat shading with visible edges
    // Darken bottom faces
    if (n.y < -0.5) color *= 0.7;
    
    // Distance fog
    float dist = length(v_world_pos.xz) / 1500.0;
    vec3 fog = vec3(0.45, 0.55, 0.6);
    color = mix(color, fog, clamp(dist * dist, 0.0, 0.6));
    
    frag_color = vec4(color, 1.0);
}
