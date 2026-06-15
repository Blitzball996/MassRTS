#version 330 core
layout(location=0) in vec2 a_vertex;
layout(location=2) in vec3 a_inst_pos;
layout(location=3) in vec3 a_inst_color;
layout(location=4) in float a_inst_size;

uniform mat4 u_view;
uniform mat4 u_proj;

out vec3 v_color;
out vec2 v_uv;

void main() {
    // Billboard: face camera
    vec3 cam_right = vec3(u_view[0][0], u_view[1][0], u_view[2][0]);
    vec3 cam_up = vec3(u_view[0][1], u_view[1][1], u_view[2][1]);
    
    vec3 world_pos = a_inst_pos
        + cam_right * a_vertex.x * a_inst_size
        + cam_up * a_vertex.y * a_inst_size;
    
    gl_Position = u_proj * u_view * vec4(world_pos, 1.0);
    v_color = a_inst_color;
    v_uv = a_vertex + 0.5;
}
