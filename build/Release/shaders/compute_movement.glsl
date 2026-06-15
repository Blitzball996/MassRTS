#version 430 core
// GPU Movement - position integration + separation force
// Matches CPU movement_system.h behavior:
//   pos += vel * dt
//   separation push for overlapping units

layout(local_size_x = 256) in;

struct UnitData {
    vec2 position;
    vec2 velocity;
    float rotation;
    float health;
    float damage;
    float range;
    float speed;
    uint faction;
    uint type;
    uint state;
    uint target;
    float cooldown;
    float max_health;
    float _pad2;
};

struct MoveCmd {
    float tx, ty;
    uint has;
    float pad;
};

const float CELL_SIZE = 20.0;
const int GRID_DIM = 150;
const int MAX_PER_CELL = 64;
const float WORLD_BOUND = 1450.0;

layout(std430, binding = 0) buffer UnitBuffer {
    UnitData units[];
};

layout(std430, binding = 4) buffer CellCounts {
    uint cell_counts[];
};

layout(std430, binding = 5) buffer CellEntries {
    uint cell_entries[];
};

layout(std430, binding = 7) buffer MoveCmds {
    MoveCmd move_cmds[];
};

uniform uint u_count;
uniform float u_dt;
uniform uint u_frame;

ivec2 get_cell(vec2 pos) {
    return ivec2(
        clamp(int((pos.x + 1500.0) / CELL_SIZE), 0, GRID_DIM - 1),
        clamp(int((pos.y + 1500.0) / CELL_SIZE), 0, GRID_DIM - 1)
    );
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= u_count) return;

    UnitData u = units[idx];
    if (u.state == 3u || u.state == 5u) return; // skip Dead/Ragdoll only; Retreating(4) still moves

    // === Player move commands (highest priority) ===
    MoveCmd cmd = move_cmds[idx];
    if (cmd.has == 1u) {
        vec2 target = vec2(cmd.tx, cmd.ty);
        vec2 diff = target - u.position;
        float dist = length(diff);
        if (dist < 5.0) {
            // Arrived - just clear command, combat AI will take over
            move_cmds[idx].has = 0u;
        } else {
            // Override velocity to move toward player-commanded target
            units[idx].velocity = (diff / dist) * u.speed;
            units[idx].rotation = atan(diff.x / dist, diff.y / dist);
            units[idx].state = 1u;
        }
    }

    // === Position integration (with terrain speed mult) ===
    float terrain_mult = u._pad2; // uploaded by CPU
    if (terrain_mult < 0.1) terrain_mult = 0.7; // safety
    vec2 vel = units[idx].velocity * terrain_mult;
    vec2 new_pos = u.position + vel * u_dt;
    new_pos = clamp(new_pos, vec2(-WORLD_BOUND), vec2(WORLD_BOUND));
    units[idx].position = new_pos;

    // === Separation (staggered 1/8 per frame) ===
    uint step = 8u;
    uint phase = u_frame % step;
    if ((idx % step) != phase) return;
    if (u.state == 2u) return;

    ivec2 my_cell = get_cell(new_pos);
    vec2 push = vec2(0.0);
    int neighbors = 0;

    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            int cx = my_cell.x + dx;
            int cz = my_cell.y + dz;
            if (cx < 0 || cx >= GRID_DIM || cz < 0 || cz >= GRID_DIM) continue;
            int cell_id = cz * GRID_DIM + cx;
            uint count = min(cell_counts[cell_id], uint(MAX_PER_CELL));
            for (uint i = 0u; i < count; i++) {
                uint other = cell_entries[cell_id * MAX_PER_CELL + i];
                if (other == idx || other >= u_count) continue;
                vec2 diff = new_pos - units[other].position;
                float d = length(diff);
                if (d < 3.5 && d > 0.01) {
                    push += (diff / d) * (3.5 - d);
                    neighbors++;
                }
            }
        }
    }

    if (neighbors > 0) {
        push /= float(neighbors);
        units[idx].position += push * u_dt * 6.0;
        units[idx].position = clamp(units[idx].position, vec2(-WORLD_BOUND), vec2(WORLD_BOUND));
    }
}
