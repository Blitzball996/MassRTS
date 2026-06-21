#pragma once
// ============================================================================
//  MetaProgression — cross-run persistence for Survival / Roguelite (ddd.txt ③)
// ----------------------------------------------------------------------------
//  The retention layer: a single save file records best results across runs and
//  unlocks that carry between runs (difficulty-tier ceiling, starting bonuses,
//  unlocked content). Plain key=value text so it is trivial to inspect / reset.
//
//  Save location: alongside the executable as "massrts_survival.sav".
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>

struct MetaProgression {
    // ---- lifetime records ----
    int runs_played   = 0;
    int best_wave     = 0;   // furthest wave reached in any run
    int best_tier     = 0;   // highest difficulty tier with >=1 wave cleared
    int total_kills   = 0;   // lifetime swarm kills
    int meta_points   = 0;   // earned per run (wave*tier), spend-able later

    // ---- unlocks (carry into every future run) ----
    int   unlocked_tier   = 1;     // max selectable difficulty tier (1..5)
    bool  unlock_richstart = false;// +400 starting metal
    bool  unlock_veterans  = false;// starter garrison +50%
    bool  unlock_extracard = false;// (reserved) draft shows a 4th card

    const char* path() const { return "massrts_survival.sav"; }

    // Load from disk; silently keeps defaults if the file is absent/garbled.
    void load() {
        FILE* f = fopen(path(), "r");
        if (!f) return;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64]; long val = 0;
            if (sscanf(line, "%63[^=]=%ld", key, &val) == 2) {
                if      (!strcmp(key, "runs_played"))    runs_played = (int)val;
                else if (!strcmp(key, "best_wave"))      best_wave = (int)val;
                else if (!strcmp(key, "best_tier"))      best_tier = (int)val;
                else if (!strcmp(key, "total_kills"))    total_kills = (int)val;
                else if (!strcmp(key, "meta_points"))    meta_points = (int)val;
                else if (!strcmp(key, "unlocked_tier"))  unlocked_tier = (int)val;
                else if (!strcmp(key, "unlock_richstart"))unlock_richstart = (val != 0);
                else if (!strcmp(key, "unlock_veterans")) unlock_veterans = (val != 0);
                else if (!strcmp(key, "unlock_extracard"))unlock_extracard = (val != 0);
            }
        }
        fclose(f);
        if (unlocked_tier < 1) unlocked_tier = 1;
    }

    void save() const {
        FILE* f = fopen(path(), "w");
        if (!f) return;
        fprintf(f, "runs_played=%d\n", runs_played);
        fprintf(f, "best_wave=%d\n", best_wave);
        fprintf(f, "best_tier=%d\n", best_tier);
        fprintf(f, "total_kills=%d\n", total_kills);
        fprintf(f, "meta_points=%d\n", meta_points);
        fprintf(f, "unlocked_tier=%d\n", unlocked_tier);
        fprintf(f, "unlock_richstart=%d\n", unlock_richstart ? 1 : 0);
        fprintf(f, "unlock_veterans=%d\n", unlock_veterans ? 1 : 0);
        fprintf(f, "unlock_extracard=%d\n", unlock_extracard ? 1 : 0);
        fclose(f);
    }

    // Record the outcome of a finished run and bank meta points. Auto-unlocks
    // the next tier when the ceiling is beaten, plus milestone unlocks. Returns
    // points earned this run.
    int record_run(int wave_reached, int tier, int kills) {
        runs_played++;
        total_kills += (kills > 0 ? kills : 0);
        best_wave = std::max(best_wave, wave_reached);
        if (wave_reached >= 1) best_tier = std::max(best_tier, tier);
        int earned = wave_reached * tier;
        meta_points += earned;
        if (tier >= unlocked_tier && wave_reached >= 5 && unlocked_tier < 5)
            unlocked_tier++;
        if (best_wave >= 5)  unlock_richstart = true;
        if (best_wave >= 10) unlock_veterans  = true;
        if (best_wave >= 15) unlock_extracard = true;
        save();
        return earned;
    }
};

