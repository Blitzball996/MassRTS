#version 330 core
layout(location=0) in vec2 a_vertex;
layout(location=2) in vec3 a_inst_pos;
layout(location=3) in vec3 a_inst_color;
layout(location=4) in float a_inst_scale;
layout(location=5) in float a_inst_rotation;
layout(location=6) in float a_inst_selected;

uniform mat4 u_view;
uniform mat4 u_proj;

out vec3 v_color;
out vec2 v_uv;
out float v_selected;

void main() {
    vec3 cam_right = vec3(u_view[0][0], u_view[1][0], u_view[2][0]);
    vec3 cam_up = vec3(0, 1, 0);

    // Billboard size: match mesh at LOD crossover
    // MC character ~1.8 blocks tall, scale factor
    float height = a_inst_scale * 0.85;
    float width = height * 0.55; // wider — MC characters are boxy not thin sticks

    vec3 center = a_inst_pos + vec3(0, height * 0.5, 0);

    vec3 pos = center
             + cam_right * (a_vertex.x * width)
             + cam_up * (a_vertex.y * height);

    gl_Position = u_proj * u_view * vec4(pos, 1.0);
    v_color = a_inst_color;
    v_uv = a_vertex * 0.5 + 0.5;
    v_selected = a_inst_selected;
}
