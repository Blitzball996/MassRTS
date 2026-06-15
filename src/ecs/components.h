#pragma once
#include <cstdint>
#include <cstddef>
#include <glm/glm.hpp>

using Entity = uint32_t;
constexpr Entity INVALID_ENTITY = UINT32_MAX;
constexpr size_t MAX_ENTITIES = 1000000;

enum class Faction : uint8_t { Red = 0, Blue = 1, COUNT };
enum class UnitType : uint8_t {
    Infantry = 0, Cavalry = 1, Archer = 2, Bomber = 3, Artillery = 4,
    Shield = 5, Samurai = 6, Militia = 7, Wall = 8, Turret = 9, COUNT
};
enum class UnitState : uint8_t {
    Idle = 0, Moving = 1, Attacking = 2, Dead = 3,
    Retreating = 4, Ragdoll = 5, Swimming = 6
};

struct TransformComponents {
    glm::vec2 position[MAX_ENTITIES];
    glm::vec2 velocity[MAX_ENTITIES];
    float rotation[MAX_ENTITIES];
    float y_offset[MAX_ENTITIES];     // vertical offset (ragdoll flying)
    float y_velocity[MAX_ENTITIES];   // vertical velocity
};

struct UnitComponents {
    Faction faction[MAX_ENTITIES];
    UnitType type[MAX_ENTITIES];
    UnitState state[MAX_ENTITIES];
    float health[MAX_ENTITIES];
    float max_health[MAX_ENTITIES];
    float attack_damage[MAX_ENTITIES];
    float attack_range[MAX_ENTITIES];
    float attack_cooldown[MAX_ENTITIES];
    float speed[MAX_ENTITIES];
    Entity target[MAX_ENTITIES];
    float hit_timer[MAX_ENTITIES];
    float ragdoll_timer[MAX_ENTITIES];
    bool is_structure[MAX_ENTITIES]; // walls/turrets don't move
};

struct RenderComponents {
    glm::vec3 color[MAX_ENTITIES];
    float scale[MAX_ENTITIES];
};

struct SelectionComponents {
    bool selected[MAX_ENTITIES];
    bool player_owned[MAX_ENTITIES];
};

struct CommandComponents {
    glm::vec2 move_target[MAX_ENTITIES];
    bool has_move_command[MAX_ENTITIES];
};

// Unit costs for buying
struct UnitShopEntry {
    UnitType type;
    int cost;
    const char* name;
    float hp, dmg, range, speed, scale;
};

static const UnitShopEntry UNIT_SHOP[] = {
    {UnitType::Militia,   20,  "Militia",    60, 6, 6, 5, 1.8f},
    {UnitType::Infantry, 50,  "Infantry",  100, 10, 8, 6, 2.0f},
    {UnitType::Archer,   60,  "Archer",     60, 12, 100, 4, 1.8f},
    {UnitType::Shield,   80,  "Shield",    200, 8, 6, 4, 2.2f},
    {UnitType::Cavalry, 120,  "Cavalry",   150, 18, 8, 12, 2.5f},
    {UnitType::Bomber,  100,  "Bomber",     80, 80, 12, 5, 2.2f},
    {UnitType::Artillery,200, "Artillery",  70, 40, 200, 2, 2.8f},
    {UnitType::Wall,     30,  "Wall",      500, 0, 0, 0, 3.0f},
    {UnitType::Turret,  150,  "Turret",    150, 25, 120, 0, 2.5f},
};
static const int SHOP_COUNT = sizeof(UNIT_SHOP)/sizeof(UNIT_SHOP[0]);
