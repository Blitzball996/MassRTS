#version 330 core
layout(location=0) in vec2 a_pos;
// Per instance
layout(location=2) in vec3 a_inst_pos;
layout(location=3) in vec3 a_inst_color;
layout(location=4) in vec3 a_inst_dir;
layout(location=5) in float a_inst_size;
layout(location=6) in float a_inst_stretch;

uniform mat4 u_view;
uniform mat4 u_proj;

out vec3 v_color;
out vec2 v_quad;     // local quad coord in [-1,1], for sphere impostor
out float v_stretch; // >2 = arrow (flat), else sphere impostor

void main() {
    v_color = a_inst_color;
    v_quad = a_pos * 2.0;   // a_pos [-0.5,0.5] -> [-1,1]
    v_stretch = a_inst_stretch;

    vec3 cam_right = vec3(u_view[0][0], u_view[1][0], u_view[2][0]);
    vec3 cam_up    = vec3(u_view[0][1], u_view[1][1], u_view[2][1]);

    vec3 offset;
    if (a_inst_stretch > 2.0) {
        // Arrow: cylindrical billboard aligned to flight direction.
        // The arrow's length-axis follows velocity; the width-axis faces the camera
        // so the arrow is always readable but points where it's going.
        vec3 flight_dir = normalize(a_inst_dir);
        vec3 cam_forward = -vec3(u_view[0][2], u_view[1][2], u_view[2][2]);
        
        // Right vector perpendicular to flight direction, pointing toward camera
        vec3 right = cross(flight_dir, cam_forward);
        float rl = length(right);
        if (rl < 0.01) {
            // Degenerate case: arrow flies directly at/away from camera.
            // Fall back to any perpendicular (use cam_up as reference).
            right = normalize(cross(flight_dir, cam_up));
        } else {
            right = right / rl;
        }
        
        // Motion stretch: faster arrows get visually elongated (motion blur proxy).
        float vel_mag = length(a_inst_dir);
        float stretch_mult = 1.0 + clamp(vel_mag / 100.0, 0.0, 1.5); // cap at 2.5x
        
        float w = a_inst_size * 0.5;  // quad width houses the arrowhead; frag keeps the shaft thin
        float h = a_inst_size * a_inst_stretch * stretch_mult;
        offset = right * a_pos.x * w + flight_dir * a_pos.y * h;
    } else {
        // Cannonball / nuke: camera-facing round billboard (sphere impostor).
        float r = a_inst_size;
        offset = cam_right * a_pos.x * r + cam_up * a_pos.y * r;
    }

    vec3 world_pos = a_inst_pos + offset;
    gl_Position = u_proj * u_view * vec4(world_pos, 1.0);
}
