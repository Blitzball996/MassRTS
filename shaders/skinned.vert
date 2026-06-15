#version 330 core
// ============================================================================
// GPU SKINNING vertex shader for the asset pipeline's skinned models.
// Bone matrices are sampled from an animation texture (no CPU skinning), so
// 50k animated units cost ~nothing on the CPU.
//
// Vertex attribs (from SkinnedModel VAO): 0=pos 1=norm 2=uv 3=bone_ids 4=weights
// Instance attribs (from instance VBO):   5=pos 6=color 7=scale 8=rot 9=state
// ============================================================================
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;
layout(location=3) in vec4 a_bone_ids;
layout(location=4) in vec4 a_bone_weights;

layout(location=5) in vec3  a_inst_pos;
layout(location=6) in vec3  a_inst_color;
layout(location=7) in float a_inst_scale;
layout(location=8) in float a_inst_rotation;
layout(location=9) in float a_inst_state;

uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_time;
uniform float u_model_scale;
uniform sampler2D u_anim_tex;   // RGBA32F: width = bones*4, height = total frames
uniform int  u_bone_count;
uniform int  u_anim_width;
uniform int  u_anim_height;
// Active clip selected by CPU per draw (start row, frame count, fps)
uniform int  u_clip_start;
uniform int  u_clip_frames;
uniform float u_clip_fps;
uniform float u_anim_phase;     // per-draw time offset into the clip (seconds)

out vec3 v_color;
out vec3 v_normal;
out vec3 v_world_pos;
out vec2 v_uv;

// Fetch bone matrix `bone` at frame row `frame` from the animation texture.
mat4 fetch_bone(int bone, int frame) {
    // each bone occupies 4 horizontal texels (one matrix row each)
    float fy = (float(frame) + 0.5) / float(u_anim_height);
    int base = bone * 4;
    vec4 r0 = texture(u_anim_tex, vec2((float(base)+0.5)/float(u_anim_width), fy));
    vec4 r1 = texture(u_anim_tex, vec2((float(base+1)+0.5)/float(u_anim_width), fy));
    vec4 r2 = texture(u_anim_tex, vec2((float(base+2)+0.5)/float(u_anim_width), fy));
    vec4 r3 = texture(u_anim_tex, vec2((float(base+3)+0.5)/float(u_anim_width), fy));
    // texture rows are matrix rows; transpose to column-major mat4
    return mat4(
        vec4(r0.x, r1.x, r2.x, r3.x),
        vec4(r0.y, r1.y, r2.y, r3.y),
        vec4(r0.z, r1.z, r2.z, r3.z),
        vec4(r0.w, r1.w, r2.w, r3.w)
    );
}

mat3 rot_y(float a) {
    float c = cos(a), s = sin(a);
    return mat3(c,0,-s, 0,1,0, s,0,c);
}

void main() {
    vec3 skinned_pos = a_pos;
    vec3 skinned_norm = a_normal;

    if (u_bone_count > 0 && u_clip_frames > 0) {
        // current frame within the clip (looping), per-instance phase de-syncs units
        float seed = fract(sin(a_inst_pos.x*0.13 + a_inst_pos.z*0.71) * 43758.5453);
        float t = u_time + u_anim_phase + seed * 2.0;
        int local = int(mod(t * u_clip_fps, float(u_clip_frames)));
        int frame = u_clip_start + local;

        mat4 skin =
            fetch_bone(int(a_bone_ids.x), frame) * a_bone_weights.x +
            fetch_bone(int(a_bone_ids.y), frame) * a_bone_weights.y +
            fetch_bone(int(a_bone_ids.z), frame) * a_bone_weights.z +
            fetch_bone(int(a_bone_ids.w), frame) * a_bone_weights.w;

        skinned_pos = (skin * vec4(a_pos, 1.0)).xyz;
        skinned_norm = mat3(skin) * a_normal;
    }

    // instance transform: scale -> rotate(Y) -> translate
    mat3 R = rot_y(a_inst_rotation);
    vec3 world = R * (skinned_pos * a_inst_scale * u_model_scale) + a_inst_pos;

    v_color = a_inst_color;
    v_normal = normalize(R * skinned_norm);
    v_world_pos = world;
    v_uv = a_uv;
    gl_Position = u_proj * u_view * vec4(world, 1.0);
}
