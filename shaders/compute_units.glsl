#version 430 core
// GPU-side instance data generation from combat UnitData
// Reads the same SSBO written by combat/movement shaders
// Produces per-type instance buffers + indirect draw commands

layout(local_size_x = 256) in;

// Must match combat UnitData struct layout exactly
struct UnitData {
    vec2 position;
    vec2 velocity;
    float rotation;
    float health;
    float damage;
    float range;
    float speed;
    uint faction;
    uint type;       // 0=infantry, 1=cavalry, 2=archer, 3+=other
    uint state;      // 0=Idle, 1=Moving, 2=Attacking, 3=Dead, 4=Retreating, 5=Ragdoll
    uint target;
    float cooldown;
    float _pad1;
    float _pad2;
};

// Instance output for rendering (40 bytes, matches InstanceData on CPU)
struct InstanceOut {
    vec3 position;   // world XYZ (with terrain height)
    vec3 color;
    float scale;
    float rotation;
    float state;     // encoded animation state
    float _pad;
};

layout(std430, binding = 0) buffer UnitBuffer {
    UnitData units[];
};

layout(std430, binding = 1) buffer InstanceBuffer {
    InstanceOut instances[];
};

// Indirect draw command: { count, instanceCount, first, baseInstance }
layout(std430, binding = 2) buffer IndirectCmds {
    uvec4 draw_cmds[4]; // [0]=infantry, [1]=cavalry, [2]=archer, [3]=billboard
};

// Atomic counters for instance counts per type
layout(std430, binding = 3) buffer AtomicCounters {
    uint counters[4];
};

// Per-unit color + scale (uploaded once when units are created)
// .rgb = color, .w = scale
layout(std430, binding = 8) buffer ColorBuffer {
    vec4 unit_colors[];
};

layout(binding = 0) uniform sampler2D u_heightmap;

uniform uint u_count;
uniform vec3 u_cam_pos;
uniform float u_lod_dist;
uniform float u_terrain_size;
uniform vec4 u_frustum[6];
uniform float u_time;

float sample_height(vec2 pos) {
    vec2 uv = pos / u_terrain_size + 0.5;
    return texture(u_heightmap, uv).r;
}

bool frustum_cull(vec3 p, float radius) {
    for (int i = 0; i < 6; i++) {
        if (dot(u_frustum[i].xyz, p) + u_frustum[i].w < -radius)
            return true;
    }
    return false;
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= u_count) return;

    UnitData u = units[idx];

    // Skip dead/ragdoll units
    if (u.state >= 3u) return;

    // Sample terrain height
    float y = sample_height(u.position);
    vec3 world_pos = vec3(u.position.x, y, u.position.y);

    // Distance to camera
    float dist = length(world_pos - u_cam_pos);
    if (dist > 2500.0) return;

    // Frustum cull
    float unit_scale = unit_colors[idx].w;
    if (frustum_cull(world_pos, unit_scale * 3.0)) return;

    // Build instance data
    InstanceOut inst;
    inst.position = world_pos;
    inst.color = unit_colors[idx].rgb;
    inst.scale = unit_scale;
    inst.rotation = u.rotation;

    // Encode animation state
    float anim_state = 0.0;
    if (u.state == 1u || u.state == 4u) anim_state = 1.0; // moving/retreating
    else if (u.state == 2u) anim_state = 2.0;             // attacking
    if (u.cooldown > 0.8) anim_state += 4.0;              // hit flash
    inst.state = anim_state;

    // --- LOD crossfade band ---------------------------------------------
    // Instead of a hard mesh<->billboard switch at u_lod_dist (which pops),
    // blend across a transition band [lo, hi] around it. Inside the band a
    // unit is emitted into BOTH buckets, each with a fade weight in inst._pad
    // (0..1 alpha). The shaders multiply final alpha by this fade.
    const float BAND = 0.18;                  // +/-18% transition width
    float lo = u_lod_dist * (1.0 - BAND);
    float hi = u_lod_dist * (1.0 + BAND);

    // mesh weight: 1 near, ramps to 0 across the band; billboard is the inverse
    float mesh_w = 1.0 - clamp((dist - lo) / max(hi - lo, 1.0), 0.0, 1.0);
    float bb_w   = 1.0 - mesh_w;

    uint meshBucket = min(u.type, 2u);

    // Emit MESH instance if it contributes (near side + band).
    if (mesh_w > 0.001) {
        inst._pad = mesh_w;
        uint slot = atomicAdd(counters[meshBucket], 1u);
        if (slot < 80000u) {
            instances[meshBucket * 80000u + slot] = inst;
            draw_cmds[meshBucket].y = min(counters[meshBucket], 80000u);
        }
    }

    // Emit BILLBOARD instance if it contributes (far side + band).
    if (bb_w > 0.001) {
        inst._pad = bb_w;
        uint slot = atomicAdd(counters[3], 1u);
        if (slot < 80000u) {
            instances[3u * 80000u + slot] = inst;
            draw_cmds[3].y = min(counters[3], 80000u);
        }
    }
}
