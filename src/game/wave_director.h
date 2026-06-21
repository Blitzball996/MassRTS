#pragma once
// ============================================================================
//  WaveDirector — Endless Wave Survival / Roguelite core loop
// ----------------------------------------------------------------------------
//  Design (from ddd.txt): a data-driven wave spawner. Each wave has a troop
//  composition, count, spawn points (nests at the map edge), and pacing. The
//  difficulty curve ramps per wave (count / type / HP). Enemies spawn from
//  destructible "nests" (BAR Raptor-nest idea). Between waves the player gets
//  a build/prep window (terrain sculpting + turrets) and a 3-choose-1 draft of
//  roguelite upgrade cards. Die after N waves -> run ends; seed-able difficulty
//  tiers feed shareable "can you beat tier X" content.
//
//  This header is engine-agnostic gameplay logic: it owns the run/wave state
//  machine and mutates the ECS World directly (Blue = enemy swarm, Red = the
//  player's defenders). main_gpu.cpp drives it from the frame loop.
// ============================================================================
#include "../ecs/world.h"
#include "../ecs/components.h"
#include "../core/json.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <random>
#include <cmath>
#include <cstdint>

// ----------------------------------------------------------------------------
//  Data-driven tuning (BAR-style: balance lives in data, not code).
//  Loaded from survival_waves.json next to the exe; every field falls back to a
//  hardcoded default if the file is missing or a key is absent, so the game
//  always runs and designers/modders can re-tune without recompiling.
// ----------------------------------------------------------------------------
struct WaveConfig {
    // difficulty curve (see wave_enemy_count / wave_hp_scale / wave_dmg_scale)
    float count_base       = 40.0f;
    float count_per_wave   = 28.0f;
    float count_per_wave_sq= 3.0f;
    float count_tier_mul   = 0.25f;  // applied as (0.8 + tier_mul * tier)
    float hp_per_wave      = 0.06f;
    float hp_per_tier      = 0.10f;
    float dmg_per_wave     = 0.04f;
    float dmg_per_tier     = 0.08f;
    int   nest_max         = 6;
    float prep_time        = 30.0f;
    float prep_grant       = 250.0f; // free build budget each prep
    float spawn_rate_base  = 8.0f;   // units/sec across nests
    float spawn_rate_per_wave = 1.5f;
    float nest_hp_base     = 3000.0f;
    float nest_hp_per_wave = 600.0f;
    float nest_hp_per_tier = 1500.0f;
};

// Sub-phase of a survival run. The outer GamePhase stays Playing; the director
// owns this finer state machine so the existing menu/pause/loading flow is
// untouched.
enum class SurvivalPhase : uint8_t {
    Prep = 0,    // build window: sculpt terrain, place turrets, then "READY"
    Combat = 1,  // a wave is live; survive until all enemies + nests are gone
    Draft = 2,   // pick 1 of 3 upgrade cards before the next prep
    GameOver = 3 // base destroyed; run is over (handled as Defeat by caller)
};

// A roguelite upgrade card. Modifiers stack across a run (the meta layer that
// drives retention). `apply` mutates run-scoped multipliers held on the
// director; gameplay systems read those multipliers each frame.
enum class ModKind : uint8_t {
    Income,        // +metal/sec
    StartCash,     // lump sum now
    TurretPower,   // turret damage x
    UnitHP,        // friendly HP x
    UnitDamage,    // friendly damage x
    CarveBudget,   // free terrain-sculpt budget per prep
    BaseRepair,    // heal base now + small regen
    KillBounty,    // x money per kill
    CheapUnits     // unit cost x (<1 = cheaper)
};

struct Modifier {
    ModKind kind;
    std::string name;
    std::string desc;
    float value;      // meaning depends on kind
};

// One spawn slice inside a wave: a fraction of the wave count of a given type.
struct WaveSlice { UnitType type; float pct; };

// A "nest": a destructible structure at the map edge that the swarm pours out
// of. Killing all nests early ends the wave faster (a reason to push out and
// use terrain/turrets offensively, not just turtle).
struct Nest {
    glm::vec2 pos{0, 0};
    float health = 4000.0f;
    float max_health = 4000.0f;
    bool alive = true;
    float spawn_accum = 0.0f; // drip-spawn pacing
};

class WaveDirector {
public:
    // ---- run state ----
    SurvivalPhase phase = SurvivalPhase::Prep;
    int wave = 0;                 // 1-based once the first wave starts
    uint32_t seed = 1337;         // shareable run seed
    int difficulty_tier = 1;      // 1..N scales the whole curve (Hades "heat")
    float prep_time = 30.0f;      // build window length (seconds)
    float prep_timer = 30.0f;     // counts down during Prep
    bool prep_skip = false;       // player hit READY to start early
    float combat_timer = 0.0f;    // time spent in the current wave
    int enemies_remaining = 0;    // alive Blue units still owed/onfield this wave
    int enemies_to_spawn = 0;     // not-yet-spawned this wave
    float spawn_drip = 0.0f;      // accumulator for trickle spawning

    // ---- run-scoped roguelite multipliers (driven by drafted cards) ----
    float m_income_bonus = 0.0f;  // flat +metal/sec
    float m_turret_power = 1.0f;
    float m_unit_hp = 1.0f;
    float m_unit_damage = 1.0f;
    float m_kill_bounty = 1.0f;
    float m_unit_cost = 1.0f;
    int   carve_budget_bonus = 0; // extra free sculpt money granted each prep

    // ---- nests (enemy spawners) ----
    std::vector<Nest> nests;

    // ---- draft offer (3 cards) ----
    Modifier draft_offer[3];
    bool draft_ready = false;

    // ---- run stats ----
    int total_kills = 0;
    bool run_active = false;

    glm::vec2 base_pos{0, 0};
    float world_size = 3000.0f;
    std::mt19937 rng;

    // Data-driven tuning (loaded from survival_waves.json; defaults otherwise).
    WaveConfig cfg;

    // Load balance config from disk. Tolerant: missing file / keys keep
    // defaults. Call once at startup (before start_run). Tries several paths so
    // it works from the repo root or the build/Release dir.
    void load_config(const std::string& override_path = "") {
        const char* candidates[] = {
            "assets/survival_waves.json", "../assets/survival_waves.json",
            "../../assets/survival_waves.json", "survival_waves.json"
        };
        JsonValue root;
        if (!override_path.empty()) {
            root = JsonParser::parse_file(override_path);
        } else {
            for (const char* path : candidates) {
                root = JsonParser::parse_file(path);
                if (root.is_object()) break;
            }
        }
        if (!root.is_object()) return; // file absent -> keep defaults
        const JsonValue& w = root["wave"];
        if (w.type == JsonValue::Object) {
            cfg.count_base        = (float)w["count_base"].as_number(cfg.count_base);
            cfg.count_per_wave    = (float)w["count_per_wave"].as_number(cfg.count_per_wave);
            cfg.count_per_wave_sq = (float)w["count_per_wave_sq"].as_number(cfg.count_per_wave_sq);
            cfg.count_tier_mul    = (float)w["count_tier_mul"].as_number(cfg.count_tier_mul);
            cfg.hp_per_wave       = (float)w["hp_per_wave"].as_number(cfg.hp_per_wave);
            cfg.hp_per_tier       = (float)w["hp_per_tier"].as_number(cfg.hp_per_tier);
            cfg.dmg_per_wave      = (float)w["dmg_per_wave"].as_number(cfg.dmg_per_wave);
            cfg.dmg_per_tier      = (float)w["dmg_per_tier"].as_number(cfg.dmg_per_tier);
            cfg.nest_max          = (int)w["nest_max"].as_number(cfg.nest_max);
            cfg.prep_time         = (float)w["prep_time"].as_number(cfg.prep_time);
            cfg.prep_grant        = (float)w["prep_grant"].as_number(cfg.prep_grant);
            cfg.spawn_rate_base   = (float)w["spawn_rate_base"].as_number(cfg.spawn_rate_base);
            cfg.spawn_rate_per_wave = (float)w["spawn_rate_per_wave"].as_number(cfg.spawn_rate_per_wave);
            cfg.nest_hp_base      = (float)w["nest_hp_base"].as_number(cfg.nest_hp_base);
            cfg.nest_hp_per_wave  = (float)w["nest_hp_per_wave"].as_number(cfg.nest_hp_per_wave);
            cfg.nest_hp_per_tier  = (float)w["nest_hp_per_tier"].as_number(cfg.nest_hp_per_tier);
        }
        // Optional designer-defined card pool overrides the built-in one.
        const JsonValue& cards = root["cards"];
        if (cards.type == JsonValue::Array && !cards.arr.empty()) {
            custom_cards.clear();
            for (const auto& c : cards.arr) {
                Modifier m;
                std::string k = c["kind"].as_string("Income");
                m.kind = kind_from_string(k);
                m.name = c["name"].as_string("Upgrade");
                m.desc = c["desc"].as_string("");
                m.value = (float)c["value"].as_number(1.0);
                custom_cards.push_back(m);
            }
        }
    }

    static ModKind kind_from_string(const std::string& s) {
        if (s == "Income")      return ModKind::Income;
        if (s == "StartCash")   return ModKind::StartCash;
        if (s == "TurretPower") return ModKind::TurretPower;
        if (s == "UnitHP")      return ModKind::UnitHP;
        if (s == "UnitDamage")  return ModKind::UnitDamage;
        if (s == "CarveBudget") return ModKind::CarveBudget;
        if (s == "BaseRepair")  return ModKind::BaseRepair;
        if (s == "KillBounty")  return ModKind::KillBounty;
        if (s == "CheapUnits")  return ModKind::CheapUnits;
        return ModKind::Income;
    }

    // Designer-supplied cards from JSON; empty -> use the built-in card_pool().
    std::vector<Modifier> custom_cards;
    const std::vector<Modifier>& active_pool() const {
        return custom_cards.empty() ? card_pool() : custom_cards;
    }

    // ----- pool of all possible upgrade cards -----
    static const std::vector<Modifier>& card_pool() {
        static const std::vector<Modifier> pool = {
            {ModKind::Income,      "Supply Lines",  "+8 metal / sec",            8.0f},
            {ModKind::StartCash,   "War Chest",     "+600 metal now",            600.0f},
            {ModKind::TurretPower, "Heavy Barrels", "Turret damage +35%",        1.35f},
            {ModKind::UnitHP,      "Iron Plating",  "Friendly HP +25%",          1.25f},
            {ModKind::UnitDamage,  "Sharpened",     "Friendly damage +20%",      1.20f},
            {ModKind::CarveBudget, "Earthworks",    "+400 free sculpt budget",   400.0f},
            {ModKind::BaseRepair,  "Field Repairs", "Repair base +15000 HP",     15000.0f},
            {ModKind::KillBounty,  "Scavengers",    "Kill rewards +50%",         1.50f},
            {ModKind::CheapUnits,  "Mass Production","Unit cost -15%",           0.85f},
        };
        return pool;
    }

    // ===== run lifecycle =====
    void start_run(glm::vec2 base, float wsize, uint32_t run_seed, int tier) {
        base_pos = base;
        world_size = wsize;
        seed = run_seed;
        difficulty_tier = std::max(1, tier);
        rng.seed(seed);
        phase = SurvivalPhase::Prep;
        wave = 0;
        prep_time = cfg.prep_time;
        prep_timer = prep_time;
        prep_skip = false;
        combat_timer = 0.0f;
        enemies_remaining = 0;
        enemies_to_spawn = 0;
        m_income_bonus = 0.0f;
        m_turret_power = 1.0f;
        m_unit_hp = 1.0f;
        m_unit_damage = 1.0f;
        m_kill_bounty = 1.0f;
        m_unit_cost = 1.0f;
        carve_budget_bonus = 0;
        total_kills = 0;
        run_active = true;
        nests.clear();
        draft_ready = false;
    }

    // Per-wave enemy budget. Ramps count, and (via prep) HP/damage scale with
    // tier and wave so the curve keeps biting. Pure function of (wave, tier).
    int wave_enemy_count(int w) const {
        float base = cfg.count_base + w * cfg.count_per_wave
                   + (float)(w * w) * cfg.count_per_wave_sq;
        return (int)(base * (0.8f + cfg.count_tier_mul * difficulty_tier));
    }
    float wave_hp_scale(int w) const {
        return (1.0f + cfg.hp_per_wave * (w - 1)) * (1.0f + cfg.hp_per_tier * (difficulty_tier - 1));
    }
    float wave_dmg_scale(int w) const {
        return (1.0f + cfg.dmg_per_wave * (w - 1)) * (1.0f + cfg.dmg_per_tier * (difficulty_tier - 1));
    }
    int num_nests_for_wave(int w) const {
        return std::min(cfg.nest_max, 1 + w / 3 + (difficulty_tier - 1) / 2);
    }

    // Composition shifts toward heavier units as waves climb.
    std::vector<WaveSlice> wave_composition(int w) const {
        std::vector<WaveSlice> c;
        float heavy = glm::clamp(0.05f + 0.03f * w, 0.05f, 0.45f);
        c.push_back({UnitType::Militia,  glm::max(0.05f, 0.45f - 0.03f * w)});
        c.push_back({UnitType::Infantry, 0.30f});
        c.push_back({UnitType::Archer,   0.12f});
        c.push_back({UnitType::Cavalry,  0.08f + heavy * 0.4f});
        c.push_back({UnitType::Bomber,   heavy * 0.3f});
        c.push_back({UnitType::Artillery,heavy * 0.2f});
        c.push_back({UnitType::Shield,   heavy * 0.3f});
        return c;
    }

    // ===== prep phase =====
    // Called once when entering Prep (after a draft, or at run start). Grants
    // the free sculpt/turret budget and resets the timer.
    void begin_prep(World& world) {
        phase = SurvivalPhase::Prep;
        prep_timer = prep_time;
        prep_skip = false;
        // Hand the player a build budget so prep is meaningful even broke.
        int grant = (int)cfg.prep_grant + carve_budget_bonus;
        world.money[0] += grant;
    }

    // Tick the prep countdown. Returns true when prep is over (timer hit 0 or
    // the player pressed READY) and the wave should begin.
    bool tick_prep(float dt) {
        if (phase != SurvivalPhase::Prep) return false;
        prep_timer -= dt;
        if (prep_timer <= 0.0f || prep_skip) { prep_timer = 0.0f; return true; }
        return false;
    }

    // ===== wave start =====
    // Spawn the nests at the far edge (opposite the player base) and arm the
    // enemy budget for this wave. Units drip out of nests during Combat.
    // Deterministic nest layout for wave `w`: an arc on the far side of the map
    // from the base. Pure function of (w, base_pos, world_size) so the prep
    // phase can preview the NEXT wave's nests before they actually spawn.
    void compute_nest_positions(int w, std::vector<glm::vec2>& out) const {
        out.clear();
        int nn = num_nests_for_wave(w);
        glm::vec2 dir = glm::length(base_pos) > 1.0f ? -glm::normalize(base_pos)
                                                     : glm::vec2(1, 0);
        float edge = world_size * 0.42f;
        glm::vec2 center = dir * edge;
        float spread = world_size * 0.30f;
        glm::vec2 perp(-dir.y, dir.x);
        for (int i = 0; i < nn; i++) {
            float t = (nn == 1) ? 0.0f : ((float)i / (nn - 1) - 0.5f);
            out.push_back(center + perp * (t * spread));
        }
    }

    void begin_wave(World& world) {
        wave++;
        phase = SurvivalPhase::Combat;
        combat_timer = 0.0f;
        spawn_drip = 0.0f;

        enemies_to_spawn = wave_enemy_count(wave);
        enemies_remaining = enemies_to_spawn;

        // Place nests in an arc on the far side of the map from the base.
        nests.clear();
        std::vector<glm::vec2> pos;
        compute_nest_positions(wave, pos);
        for (auto& p : pos) {
            Nest n;
            n.pos = p;
            n.max_health = n.health = cfg.nest_hp_base + cfg.nest_hp_per_wave * wave
                                    + cfg.nest_hp_per_tier * (difficulty_tier - 1);
            n.alive = true;
            n.spawn_accum = 0.0f;
            nests.push_back(n);
            spawn_nest_structure(world, n.pos);
        }
    }

    // The nest body is a Blue Wall (structure) so it shows up, blocks, takes
    // damage and is destructible via the existing combat/explosion path. We
    // tag it by stashing its index; here we just create a tough wall.
    void spawn_nest_structure(World& world, glm::vec2 pos) {
        Entity e = world.create_entity();
        if (e == INVALID_ENTITY) return;
        world.transforms.position[e] = pos;
        world.transforms.velocity[e] = {0, 0};
        world.transforms.rotation[e] = 0;
        world.transforms.y_offset[e] = 0;
        world.transforms.y_velocity[e] = 0;
        world.units.faction[e] = Faction::Blue;
        world.units.type[e] = UnitType::Turret;     // nest defends itself a bit
        world.units.state[e] = UnitState::Idle;
        world.units.health[e] = 6000.0f;
        world.units.max_health[e] = 6000.0f;
        world.units.attack_damage[e] = 30.0f;
        world.units.attack_range[e] = 160.0f;
        world.units.speed[e] = 0.0f;
        world.units.attack_cooldown[e] = 0;
        world.units.target[e] = INVALID_ENTITY;
        world.units.hit_timer[e] = 0;
        world.units.ragdoll_timer[e] = 0;
        world.units.is_structure[e] = true;
        world.renders.color[e] = glm::vec3(0.7f, 0.15f, 0.5f); // magenta nest
        world.renders.scale[e] = 5.0f;
        world.selection.player_owned[e] = false;
        world.selection.selected[e] = false;
        world.commands.has_move_command[e] = false;
    }

    // Spawn a single enemy unit of the given type at a nest with the wave's
    // HP/damage scaling and the swarm color. Targets the player base.
    void spawn_enemy(World& world, UnitType type, glm::vec2 at) {
        Entity e = world.create_entity();
        if (e == INVALID_ENTITY) return;
        // base stats from the shop table
        float hp = 60, dmg = 8, range = 8, spd = 6, scl = 2.0f;
        for (int i = 0; i < SHOP_COUNT; i++) {
            if (UNIT_SHOP[i].type == type) {
                hp = UNIT_SHOP[i].hp; dmg = UNIT_SHOP[i].dmg;
                range = UNIT_SHOP[i].range; spd = UNIT_SHOP[i].speed;
                scl = UNIT_SHOP[i].scale; break;
            }
        }
        std::uniform_real_distribution<float> jit(-8.0f, 8.0f);
        glm::vec2 pos = at + glm::vec2(jit(rng), jit(rng));
        world.transforms.position[e] = pos;
        world.transforms.velocity[e] = {0, 0};
        world.transforms.rotation[e] = 3.14159f;
        world.transforms.y_offset[e] = 0;
        world.transforms.y_velocity[e] = 0;
        world.units.faction[e] = Faction::Blue;
        world.units.type[e] = type;
        world.units.state[e] = UnitState::Idle;
        world.units.health[e] = hp * wave_hp_scale(wave);
        world.units.max_health[e] = world.units.health[e];
        world.units.attack_damage[e] = dmg * wave_dmg_scale(wave);
        world.units.attack_range[e] = range;
        world.units.speed[e] = spd * 1.05f; // swarm presses the attack
        world.units.attack_cooldown[e] = 0;
        world.units.target[e] = INVALID_ENTITY;
        world.units.hit_timer[e] = 0;
        world.units.ragdoll_timer[e] = 0;
        world.units.is_structure[e] = false;
        world.renders.color[e] = glm::vec3(0.55f, 0.12f, 0.45f); // swarm magenta
        world.renders.scale[e] = scl;
        world.selection.player_owned[e] = false;
        world.selection.selected[e] = false;
        // March straight at the base.
        world.commands.move_target[e] = base_pos;
        world.commands.has_move_command[e] = true;
    }

    // ===== combat tick =====
    // Drip enemies from living nests, refresh nest health from the ECS, and
    // detect wave end. `blue_alive` is the live Blue count from the combat
    // system (units only, structures counted separately is fine here). Returns
    // true when the wave is cleared (all spawned + nests dead).
    bool tick_combat(World& world, float dt, int blue_alive) {
        if (phase != SurvivalPhase::Combat) return false;
        combat_timer += dt;

        // Count living nests; pick composition for trickle spawning.
        int live_nests = 0;
        for (auto& n : nests) if (n.alive) live_nests++;

        if (enemies_to_spawn > 0 && live_nests > 0) {
            // Spawn rate scales with wave so big waves don't take forever.
            float rate = cfg.spawn_rate_base + wave * cfg.spawn_rate_per_wave; // units/sec across all nests
            spawn_drip += rate * dt;
            int batch = (int)spawn_drip;
            if (batch > 0) {
                spawn_drip -= batch;
                auto comp = wave_composition(wave);
                for (int b = 0; b < batch && enemies_to_spawn > 0; b++) {
                    // weighted pick
                    float r = std::uniform_real_distribution<float>(0, 1)(rng);
                    float acc = 0; UnitType pick = UnitType::Militia;
                    float total = 0; for (auto& s : comp) total += s.pct;
                    r *= (total > 0 ? total : 1.0f);
                    for (auto& s : comp) { acc += s.pct; if (r <= acc) { pick = s.type; break; } }
                    // choose a random living nest
                    int idx = std::uniform_int_distribution<int>(0, live_nests - 1)(rng);
                    int seen = 0; glm::vec2 from = base_pos;
                    for (auto& n : nests) { if (n.alive) { if (seen == idx) { from = n.pos; break; } seen++; } }
                    spawn_enemy(world, pick, from);
                    enemies_to_spawn--;
                }
            }
        }

        // Wave is cleared when nothing is left to spawn, every nest is dead,
        // and no Blue units remain on the field.
        bool nests_dead = (live_nests == 0);
        if (enemies_to_spawn <= 0 && nests_dead && blue_alive <= 0) {
            phase = SurvivalPhase::Draft;
            roll_draft();
            return true;
        }
        // If all nests die mid-wave, stop owing un-spawned units (reward push).
        if (nests_dead) enemies_to_spawn = 0;
        enemies_remaining = enemies_to_spawn + glm::max(0, blue_alive);
        return false;
    }

    // Sync nest alive/health from the ECS each frame so a nest the player
    // destroys (via units/turrets/explosions) flips alive=false and stops
    // spawning. Matching is positional (nests are static). Cheap (<=6 nests).
    void sync_nests(World& world) {
        for (auto& n : nests) {
            if (!n.alive) continue;
            bool found = false;
            for (uint32_t i = 0; i < world.entity_count; i++) {
                if (!world.alive[i]) continue;
                if (!world.units.is_structure[i]) continue;
                if (world.units.faction[i] != Faction::Blue) continue;
                if (world.units.state[i] == UnitState::Dead) continue;
                if (glm::length(world.transforms.position[i] - n.pos) < 12.0f) {
                    n.health = world.units.health[i];
                    found = true; break;
                }
            }
            if (!found) n.alive = false; // structure gone -> nest destroyed
        }
    }

    // ===== draft =====
    void roll_draft() {
        const auto& pool = active_pool();
        // pick 3 distinct cards
        int picks[3] = {-1, -1, -1};
        for (int i = 0; i < 3; i++) {
            int idx; bool dup;
            int guard = 0;
            do {
                idx = std::uniform_int_distribution<int>(0, (int)pool.size() - 1)(rng);
                dup = (idx == picks[0] || idx == picks[1] || idx == picks[2]);
            } while (dup && guard++ < 32);
            picks[i] = idx;
            draft_offer[i] = pool[idx];
        }
        draft_ready = true;
    }

    // Apply the chosen card, then exit Draft -> Prep for the next wave.
    void choose_card(World& world, int which) {
        if (which < 0 || which > 2 || !draft_ready) return;
        const Modifier& m = draft_offer[which];
        switch (m.kind) {
            case ModKind::Income:      m_income_bonus += m.value; break;
            case ModKind::StartCash:   world.money[0] += (int)m.value; break;
            case ModKind::TurretPower: m_turret_power *= m.value; break;
            case ModKind::UnitHP:      m_unit_hp *= m.value; break;
            case ModKind::UnitDamage:  m_unit_damage *= m.value; break;
            case ModKind::CarveBudget: carve_budget_bonus += (int)m.value; break;
            case ModKind::BaseRepair:  /* handled by caller (owns BaseSystem) */ break;
            case ModKind::KillBounty:  m_kill_bounty *= m.value; break;
            case ModKind::CheapUnits:  m_unit_cost *= m.value; break;
        }
        draft_ready = false;
        begin_prep(world);
    }
};


