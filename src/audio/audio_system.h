#pragma once

// Must define MINIAUDIO_IMPLEMENTATION before including this header in exactly one .cpp
// In main_gpu.cpp: #define MINIAUDIO_IMPLEMENTATION before #include "audio/audio_system.h"

#include "miniaudio.h"
#include <string>
#include <iostream>
#include <cstring>
#include <cmath>
#include <random>
#include <fstream>
#include <unordered_map>

// ============================================================================
// AudioSystem — file-based music + SFX (miniaudio high-level engine) with a
// procedural battle-ambience layer as fallback.
//
// Drop audio files in assets/audio/ (mp3 / ogg / wav / flac all decode):
//   music_menu.*    — main-menu background track (loops)
//   music_battle.*  — in-battle background track (loops)
//   ui_click.*      — button press / click
//   ui_hover.*      — (optional) button hover blip
//   sfx_arrow.*     — arrow impact / volley
//   sfx_cannon.*    — cannon boom
//   sfx_nuke.*      — nuke detonation
//   sfx_explosion.* — generic explosion
// Missing files are skipped silently; the procedural ambience still plays so
// the game is never silent even with zero assets shipped.
// ============================================================================
class AudioSystem {
public:
    ma_engine engine;
    bool initialized = false;

    // Mixed-in procedural battle ambience (independent device, see below).
    float battle_intensity = 0;
    float music_volume = 0.6f;   // music bus (0..1, scaled by master)
    float sfx_volume   = 0.8f;   // sfx bus  (0..1, scaled by master)
    float master_volume = 0.7f;

    // Procedural ambience device.
    ma_device device;
    bool device_active = false;
    float phase_sword = 0, phase_march = 0, phase_wind = 0;
    float explosion_decay = 0, arrow_whoosh = 0, cannon_boom = 0, nuke_rumble = 0;
    std::mt19937 rng{42};

    // --- File-based music tracks ---
    ma_sound music_menu, music_battle;
    bool has_menu_music = false, has_battle_music = false;
    int  current_track = -1; // -1 none, 0 menu, 1 battle

    // --- File-based SFX (one-shot, may overlap via fire-and-forget) ---
    std::unordered_map<std::string, std::string> sfx_paths; // key -> resolved path
    bool has_click = false, has_hover = false;

    std::string asset_base; // resolved "assets/audio/" root (with trailing slash)

    // ---- helpers -----------------------------------------------------------

    // Probe a set of candidate roots / extensions and return the first that
    // exists, or "" if none. miniaudio decodes mp3/ogg/wav/flac.
    std::string resolve(const char* stem) {
        static const char* roots[] = {
            "assets/audio/", "../assets/audio/",
            "../../assets/audio/", "../../../assets/audio/"
        };
        static const char* exts[] = { ".ogg", ".mp3", ".wav", ".flac" };
        for (const char* root : roots)
            for (const char* ext : exts) {
                std::string p = std::string(root) + stem + ext;
                std::ifstream probe(p, std::ios::binary);
                if (probe.good()) return p;
            }
        return std::string();
    }

    bool init() {
        ma_engine_config cfg = ma_engine_config_init();
        cfg.channels = 2;
        cfg.sampleRate = 44100;
        if (ma_engine_init(&cfg, &engine) != MA_SUCCESS) {
            std::cerr << "Audio: Failed to init engine\n";
            return false;
        }
        initialized = true;

        // --- Load music tracks from files (looping) ---
        std::string mm = resolve("music_menu");
        if (!mm.empty() && ma_sound_init_from_file(&engine, mm.c_str(),
                MA_SOUND_FLAG_STREAM, NULL, NULL, &music_menu) == MA_SUCCESS) {
            ma_sound_set_looping(&music_menu, MA_TRUE);
            has_menu_music = true;
            std::cout << "Audio: menu music " << mm << "\n";
        }
        std::string mb = resolve("music_battle");
        if (!mb.empty() && ma_sound_init_from_file(&engine, mb.c_str(),
                MA_SOUND_FLAG_STREAM, NULL, NULL, &music_battle) == MA_SUCCESS) {
            ma_sound_set_looping(&music_battle, MA_TRUE);
            has_battle_music = true;
            std::cout << "Audio: battle music " << mb << "\n";
        }

        // --- Resolve SFX file paths (played one-shot, fire-and-forget) ---
        const char* sfx_keys[] = {
            "ui_click", "ui_hover", "sfx_arrow", "sfx_cannon",
            "sfx_nuke", "sfx_explosion"
        };
        for (const char* k : sfx_keys) {
            std::string p = resolve(k);
            if (!p.empty()) sfx_paths[k] = p;
        }
        has_click = sfx_paths.count("ui_click") > 0;
        has_hover = sfx_paths.count("ui_hover") > 0;

        apply_volumes();

        // --- Procedural ambience device (battle clashes / wind / blasts) ---
        ma_device_config devCfg = ma_device_config_init(ma_device_type_playback);
        devCfg.playback.format = ma_format_f32;
        devCfg.playback.channels = 2;
        devCfg.sampleRate = 44100;
        devCfg.dataCallback = audio_callback;
        devCfg.pUserData = this;
        if (ma_device_init(NULL, &devCfg, &device) == MA_SUCCESS) {
            ma_device_start(&device);
            device_active = true;
        }

        std::cout << "Audio: Initialized (files+procedural)\n";
        return true;
    }

    // ---- volume control ----------------------------------------------------
    // Master applies to the whole engine; per-track music volume is master*music.
    void set_volumes(float master, float music, float sfx) {
        master_volume = master; music_volume = music; sfx_volume = sfx;
        apply_volumes();
    }
    void apply_volumes() {
        if (!initialized) return;
        ma_engine_set_volume(&engine, master_volume);
        if (has_menu_music)   ma_sound_set_volume(&music_menu,   music_volume);
        if (has_battle_music) ma_sound_set_volume(&music_battle, music_volume);
    }

    // ---- music track switching ---------------------------------------------
    // track: 0 = menu, 1 = battle. Stops the other, (re)starts the requested
    // one from the top. No-op if the file is missing (procedural still plays).
    void play_music(int track) {
        if (track == current_track) return;
        if (has_menu_music)   ma_sound_stop(&music_menu);
        if (has_battle_music) ma_sound_stop(&music_battle);
        if (track == 0 && has_menu_music) {
            ma_sound_seek_to_pcm_frame(&music_menu, 0);
            ma_sound_start(&music_menu);
        } else if (track == 1 && has_battle_music) {
            ma_sound_seek_to_pcm_frame(&music_battle, 0);
            ma_sound_start(&music_battle);
        }
        current_track = track;
    }
    void stop_music() {
        if (has_menu_music)   ma_sound_stop(&music_menu);
        if (has_battle_music) ma_sound_stop(&music_battle);
        current_track = -1;
    }

    // ---- one-shot SFX ------------------------------------------------------
    // Fire-and-forget: ma_engine_play_sound spins up a managed voice that the
    // engine reclaims when it finishes, so overlapping shots are fine.
    void play_sfx(const char* key) {
        if (!initialized) return;
        auto it = sfx_paths.find(key);
        if (it == sfx_paths.end()) return;
        ma_engine_play_sound(&engine, it->second.c_str(), NULL);
    }

    // ---- UI sounds ---------------------------------------------------------
    void play_click() { if (has_click) play_sfx("ui_click"); }
    void play_hover() { if (has_hover) play_sfx("ui_hover"); }

    // ---- combat SFX: play the file if present, else fall back to procedural -
    void trigger_explosion() { if (sfx_paths.count("sfx_explosion")) play_sfx("sfx_explosion"); else explosion_decay = 1.0f; }
    void trigger_arrow()     { if (sfx_paths.count("sfx_arrow"))     play_sfx("sfx_arrow");     else arrow_whoosh = 1.0f; }
    void trigger_cannon()    { if (sfx_paths.count("sfx_cannon"))    play_sfx("sfx_cannon");    else cannon_boom = 1.0f; }
    void trigger_nuke()      { if (sfx_paths.count("sfx_nuke"))      play_sfx("sfx_nuke");      else nuke_rumble = 2.0f; }

    void update(float dt, float intensity) {
        battle_intensity = intensity;
        if (explosion_decay > 0) explosion_decay -= dt * 2.0f;
        if (arrow_whoosh > 0)    arrow_whoosh    -= dt * 4.0f;
        if (cannon_boom > 0)     cannon_boom     -= dt * 1.5f;
        if (nuke_rumble > 0)     nuke_rumble     -= dt * 0.3f;
    }

    void cleanup() {
        if (device_active) { ma_device_uninit(&device); device_active = false; }
        if (has_menu_music)   ma_sound_uninit(&music_menu);
        if (has_battle_music) ma_sound_uninit(&music_battle);
        if (initialized) { ma_engine_uninit(&engine); initialized = false; }
    }

private:
    static void audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
        AudioSystem* self = (AudioSystem*)pDevice->pUserData;
        float* out = (float*)pOutput;
        float dt = 1.0f / 44100.0f;
        // Procedural ambience gated by master volume so the slider turns it
        // down together with the file music/sfx.
        float gain = self->master_volume;

        for (ma_uint32 i = 0; i < frameCount; i++) {
            float sample = 0;
            float intensity = self->battle_intensity;
            if (intensity > 0.01f) {
                self->phase_sword += dt * (2000.0f + sin(self->phase_sword * 0.1f) * 500.0f);
                float sword = sin(self->phase_sword) * 0.03f * intensity;
                sword *= (sin(self->phase_march * 7.3f) > 0.7f) ? 1.0f : 0.0f;
                self->phase_march += dt * 3.0f;
                float march = sin(self->phase_march * 6.28f) * 0.05f * intensity;
                march += sin(self->phase_march * 12.56f) * 0.02f * intensity;
                float noise = ((float)(self->rng() % 10000) / 10000.0f - 0.5f) * 0.04f * intensity;
                float shout_env = (sin(self->phase_march * 0.5f) > 0.6f) ? 1.0f : 0.2f;
                noise *= shout_env;
                sample += sword + march + noise;
            }
            self->phase_wind += dt;
            float wind_noise = ((float)(self->rng() % 10000) / 10000.0f - 0.5f);
            float wind_env = sin(self->phase_wind * 0.3f) * 0.5f + 0.5f;
            sample += wind_noise * 0.008f * wind_env;
            if (self->explosion_decay > 0) {
                float boom = sin(self->explosion_decay * 80.0f) * self->explosion_decay;
                boom += ((float)(self->rng() % 10000) / 10000.0f - 0.5f) * self->explosion_decay * 0.5f;
                sample += boom * 0.15f;
            }
            if (self->cannon_boom > 0) {
                float t = 1.0f - self->cannon_boom;
                float boom = sin(t * 150.0f) * exp(-t * 8.0f);
                boom += ((float)(self->rng() % 10000) / 10000.0f - 0.5f) * self->cannon_boom * 0.3f;
                sample += boom * 0.2f;
            }
            if (self->arrow_whoosh > 0) {
                float t = 1.0f - self->arrow_whoosh;
                float whoosh = sin(t * 800.0f + sin(t * 200.0f) * 3.0f) * exp(-t * 10.0f);
                sample += whoosh * 0.04f;
            }
            if (self->nuke_rumble > 0) {
                float t = 2.0f - self->nuke_rumble;
                float rumble = sin(t * 30.0f) * exp(-t * 0.5f);
                rumble += sin(t * 50.0f) * 0.5f * exp(-t * 0.8f);
                float crackle = ((float)(self->rng() % 10000) / 10000.0f - 0.5f) * exp(-t * 1.5f);
                sample += (rumble * 0.3f + crackle * 0.15f) * fminf(self->nuke_rumble, 1.0f);
            }
            // Procedural pentatonic melody ONLY when no file music is loaded,
            // so we don't clash with a real soundtrack.
            if (!self->has_menu_music && !self->has_battle_music) {
                static float music_phase = 0, note_timer = 0; static int note_idx = 0;
                static const float notes[] = {220.0f,261.6f,293.7f,349.2f,392.0f,440.0f,523.3f};
                note_timer += dt;
                if (note_timer > 2.0f) { note_timer = 0; note_idx = (note_idx + 1) % 7; }
                music_phase += dt * notes[note_idx];
                float melody = sin(music_phase * 6.28f) * 0.015f + sin(music_phase * 12.56f) * 0.005f;
                melody *= sin(note_timer * 3.14159f / 2.0f) * self->music_volume;
                sample += melody;
            }
            sample = fmaxf(-0.8f, fminf(0.8f, sample * self->sfx_volume * gain));
            out[i * 2 + 0] = sample;
            out[i * 2 + 1] = sample;
        }
        (void)pInput;
    }
};
