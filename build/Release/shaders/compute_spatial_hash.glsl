#version 430 core
// GPU Spatial Hash - builds a grid-based spatial index for neighbor queries
// Supports up to 1M units with O(1) cell lookup

layout(local_size_x = 256) in;

struct UnitData {
    vec2 position;
    vec2 velocity;
    float rotation;
    float health;
    float damage;
    float range;
    float speed;
    uint faction;    // 0=Red, 1=Blue
    uint type;
    uint state;      // 0=Idle,1=Moving,2=Attacking,3=Dead,4=Retreating,5=Ragdoll
    uint target;
    float cooldown;
    float _pad1;
    float _pad2;
};

// Spatial hash parameters
const float CELL_SIZE = 20.0;
const int GRID_DIM = 150;          // 3000/20 = 150
const int TOTAL_CELLS = 22500;     // 150*150
const int MAX_PER_CELL = 64;

layout(std430, binding = 0) buffer UnitBuffer {
    UnitData units[];
};

// Cell counts: how many units in each cell
layout(std430, binding = 4) buffer CellCounts {
    uint cell_counts[22500];
};

// Cell entries: unit indices stored per cell
// Layout: cell_entries[cell_id * MAX_PER_CELL + local_idx] = unit_index
layout(std430, binding = 5) buffer CellEntries {
    uint cell_entries[1440000]; // 22500 * 64
};

uniform uint u_count;
uniform uint u_phase; // 0 = clear, 1 = insert

int get_cell_id(vec2 pos) {
    int cx = clamp(int((pos.x + 1500.0) / CELL_SIZE), 0, GRID_DIM - 1);
    int cz = clamp(int((pos.y + 1500.0) / CELL_SIZE), 0, GRID_DIM - 1);
    return cz * GRID_DIM + cx;
}

void main() {
    uint idx = gl_GlobalInvocationID.x;

    if (u_phase == 0u) {
        // Phase 0: Clear cell counts
        if (idx < 22500u) {
            cell_counts[idx] = 0u;
        }
        return;
    }

    // Phase 1: Insert units into grid
    if (idx >= u_count) return;

    UnitData u = units[idx];
    if (u.state >= 3u) return; // skip dead/ragdoll

    int cell = get_cell_id(u.position);
    uint slot = atomicAdd(cell_counts[cell], 1u);
    if (slot < MAX_PER_CELL) {
        cell_entries[cell * MAX_PER_CELL + slot] = idx;
    }
}
