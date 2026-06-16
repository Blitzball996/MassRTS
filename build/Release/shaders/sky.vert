#version 330 core
// Fullscreen sky pass. Emits a screen-filling triangle and reconstructs a
// world-space view ray per pixel (used by the fragment shader for atmospheric
// scattering + clouds). No vertex buffer needed: gl_VertexID drives it.
out vec3 v_ray;          // world-space view direction
uniform mat4 u_inv_view; // inverse view  (camera -> world)
uniform mat4 u_inv_proj; // inverse proj  (clip   -> view)

void main() {
    // Big single triangle covering the screen.
    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                  (gl_VertexID == 2) ? 3.0 : -1.0);
    gl_Position = vec4(p, 1.0, 1.0); // z = far plane -> drawn behind everything

    // Reconstruct the view ray: unproject the clip-space point to view space,
    // then rotate into world space. Translation is irrelevant for a direction.
    vec4 view_h = u_inv_proj * vec4(p, 1.0, 1.0);
    vec3 view_dir = view_h.xyz / view_h.w;
    v_ray = mat3(u_inv_view) * view_dir;
}
