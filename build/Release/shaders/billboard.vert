#version 330 core
layout(location=0) in vec2 a_vertex;
layout(location=2) in vec3 a_inst_pos;
layout(location=3) in vec3 a_inst_color;
layout(location=4) in float a_inst_scale;
layout(location=5) in float a_inst_rotation;
layout(location=6) in float a_inst_selected;
layout(location=10) in float a_inst_fade;

uniform mat4 u_view;
uniform mat4 u_proj;

out vec3 v_color;
out vec2 v_uv;
out float v_selected;
out float v_fade;

void main() {
    // SPHERICAL (view-space) billboard: the card is built directly in view
    // space, so it ALWAYS fully faces the camera and can never collapse into a
    // vertical sliver ("cylinder") -- which is exactly what happens to an axial
    // (world-up) card when viewed from a steep top-down RTS angle (it is seen
    // edge-on). The humanoid silhouette stays readable from every camera pitch.
    float height = a_inst_scale * 0.85;
    float width  = height * 0.55;

    // World anchor at the unit's mid-height, transformed to view space.
    vec3 center_world = a_inst_pos + vec3(0.0, height * 0.5, 0.0);
    vec4 center_view = u_view * vec4(center_world, 1.0);

    // Offset within the view plane keeps the quad camera-facing at all times.
    center_view.xy += vec2(a_vertex.x * width, a_vertex.y * height);

    gl_Position = u_proj * center_view;
    v_color = a_inst_color;
    v_uv = a_vertex * 0.5 + 0.5;
    v_selected = a_inst_selected;
    v_fade = (a_inst_fade <= 0.0) ? 1.0 : a_inst_fade;
}
                                                                                                                                                                                                     