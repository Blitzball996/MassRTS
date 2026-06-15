#version 430 core
// GPU Combat AI - DECISION ONLY (no damage dealing)
// Sets: velocity, state, target, rotation
// CPU handles: actual damage, projectiles, effects, death

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
    uint type;       // 0=Infantry,1=Cavalry,2=Archer,3=Bomber,4=Artillery,5=Shield,6=Samurai,7=Militia
    uint state;
    uint target;
    float cooldown;
    float max_health;
    float _pad2;
};

const float CELL_SIZE = 20.0;
const int GRID_DIM = 150;
const int MAX_PER_CELL = 64;

layout(std430, binding = 0) buffer UnitBuffer {
    UnitData units[];
};

layout(std430, binding = 4) buffer CellCounts {
    uint cell_counts[];
};

layout(std430, binding = 5) buffer CellEntries {
    uint cell_entries[];
};

uniform uint u_count;
uniform float u_dt;
uniform vec2 u_faction_center[2];
uniform uint u_faction_alive[2];

ivec2 get_cell(vec2 pos) {
    return ivec2(
        clamp(int((pos.x + 1500.0) / CELL_SIZE), 0, GRID_DIM - 1),
        clamp(int((pos.y + 1500.0) / CELL_SIZE), 0, GRID_DIM - 1)
    );
}

uint find_nearest_enemy(uint self_idx, vec2 pos, float search_range, uint my_faction) {
    uint best = 0xFFFFFFFFu;
    float best_dist2 = search_range * search_range;
    int search_r = min(int(ceil(search_range / CELL_SIZE)), 7);
    ivec2 my_cell = get_cell(pos);

    for (int dz = -search_r; dz <= search_r; dz++) {
        for (int dx = -search_r; dx <= search_r; dx++) {
            int cx = my_cell.x + dx;
            int cz = my_cell.y + dz;
            if (cx < 0 || cx >= GRID_DIM || cz < 0 || cz >= GRID_DIM) continue;
            int cell_id = cz * GRID_DIM + cx;
            uint count = min(cell_counts[cell_id], uint(MAX_PER_CELL));
            for (uint i = 0u; i < count; i++) {
                uint other = cell_entries[cell_id * MAX_PER_CELL + i];
                if (other == self_idx || other >= u_count) continue;
                UnitData enemy = units[other];
                if (enemy.faction == my_faction) continue;
                if (enemy.state >= 3u) continue;
                vec2 diff = enemy.position - pos;
                float d2 = dot(diff, diff);
                if (d2 < best_dist2) {
                    best_dist2 = d2;
                    best = other;
                }
            }
        }
    }
    return best;
}

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= u_count) return;

    UnitData u = units[idx];
    // Dead(3)/Ragdoll(5) are CPU-handled. Retreating(4) MUST run below,
    // otherwise the retreat+heal logic is unreachable and the unit freezes
    // in place forever (looks like a stuck idle corpse).
    if (u.state == 3u || u.state == 5u) return;

    // Cooldown tick
    units[idx].cooldown = max(u.cooldown - u_dt, 0.0);

    // === Retreat check ===
    float retreat_thresh = (u.type == 5u) ? 0.1 : 0.2;
    if (u.health < u.max_health * retreat_thresh && u.state != 4u) {
        units[idx].state = 4u;
        units[idx].target = 0xFFFFFFFFu;
        vec2 to_safety = u_faction_center[u.faction] - u.position;
        float dist = length(to_safety);
        if (dist > 10.0) {
            vec2 dir = to_safety / dist;
            units[idx].velocity = dir * u.speed * 1.3;
            units[idx].rotation = atan(dir.x, dir.y);
        }
        return;
    }

    // === Retreating: move to safety + heal ===
    if (u.state == 4u) {
        vec2 to_safety = u_faction_center[u.faction] - u.position;
        float dist = length(to_safety);
        if (dist > 10.0) {
            vec2 dir = to_safety / dist;
            units[idx].velocity = dir * u.speed * 1.3;
            units[idx].rotation = atan(dir.x, dir.y);
        }
        float new_hp = min(u.health + u_dt * 5.0, u.max_health * 0.5);
        units[idx].health = new_hp;
        if (new_hp >= u.max_health * 0.5) {
            units[idx].state = 0u;
            units[idx].target = 0xFFFFFFFFu;
            units[idx].velocity = vec2(0.0);
        }
        return;
    }

    // === Validate target ===
    uint tgt = u.target;
    if (tgt != 0xFFFFFFFFu) {
        if (tgt >= u_count || units[tgt].state >= 3u) {
            tgt = 0xFFFFFFFFu;
            units[idx].target = 0xFFFFFFFFu;
            units[idx].state = 0u;
        }
    }

    // === Find new target ===
    if (tgt == 0xFFFFFFFFu) {
        float search = (u.type == 4u) ? 600.0 : 500.0;
        tgt = find_nearest_enemy(idx, u.position, search, u.faction);
        units[idx].target = tgt;
        if (tgt != 0xFFFFFFFFu) {
            vec2 diff = units[tgt].position - u.position;
            float dist = length(diff);
            if (dist <= u.range) {
                units[idx].state = 2u;
                units[idx].velocity = vec2(0.0);
            } else {
                units[idx].state = 1u;
                vec2 dir = diff / (dist + 0.001);
                units[idx].velocity = dir * u.speed;
                units[idx].rotation = atan(dir.x, dir.y);
            }
        }
    }

    // === No target: idle behavior ===
    if (tgt == 0xFFFFFFFFu) {
        uint enemy_f = 1u - u.faction;
        if (u_faction_alive[enemy_f] > 0u) {
            vec2 to_enemy = u_faction_center[enemy_f] - u.position;
            float dist = length(to_enemy);
            vec2 dir = to_enemy / (dist + 0.001);
            float spd_mult = (u.type == 4u) ? 0.5 : 0.85;
            if (u.type == 1u || u.type == 6u) {
                spd_mult = 0.7;
                vec2 side = vec2(-dir.y, dir.x);
                float flank_sign = (u.position.x > u_faction_center[u.faction].x) ? 1.0 : -1.0;
                dir = normalize(dir + side * flank_sign * 0.4);
            }
            // Always move toward enemy center (even if close)
            // When very close, patrol in a circle to find targets
            if (dist <= 80.0) {
                // Near enemy center: spread out aggressively to find targets
                vec2 perp = vec2(-dir.y, dir.x);
                float spread = (float(idx % 7u) - 3.0) * 0.3;
                dir = normalize(dir + perp * spread);
                // Don't reduce speed - keep pushing
            }
            units[idx].velocity = dir * u.speed * spd_mult;
            units[idx].rotation = atan(dir.x, dir.y);
            units[idx].state = 1u;
        } else {
            // No enemies left - don't force stop, just slow down
            units[idx].velocity = u.velocity * 0.9;
            if (length(u.velocity) < 0.5) units[idx].state = 0u;
        }
        return;
    }

    // === Combat behavior ===
    vec2 to_target = units[tgt].position - u.position;
    float dist = length(to_target);
    vec2 dir = to_target / (dist + 0.001);
    units[idx].rotation = atan(to_target.x, to_target.y);

    // Archer kiting
    if (u.type == 2u && dist < u.range * 0.3) {
        units[idx].velocity = -dir * u.speed * 0.9;
        return;
    }

    // Cavalry charge
    if (u.type == 1u) {
        if (dist > u.range * 1.5) {
            units[idx].velocity = dir * u.speed * 1.3;
            units[idx].state = 1u;
        } else {
            units[idx].velocity = dir * u.speed * 0.5;
            units[idx].state = 2u;
        }
        return;
    }

    // Samurai charge
    if (u.type == 6u && dist > u.range * 1.2 && dist < u.range * 5.0) {
        units[idx].velocity = dir * u.speed * 1.5;
        units[idx].state = 1u;
        return;
    }

    // Standard melee/ranged
    if (dist > u.range * 1.5) {
        units[idx].state = 1u;
        units[idx].velocity = dir * u.speed;
    } else {
        units[idx].state = 2u;
        units[idx].velocity = vec2(0.0);
        // CPU will check cooldown==0 + state==Attacking to execute attack
    }
}
