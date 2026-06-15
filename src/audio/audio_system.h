#pragma once

// Must define MINIAUDIO_IMPLEMENTATION before including this header in exactly one .cpp
// In main.cpp, add: #define MINIAUDIO_IMPLEMENTATION before #include "audio/audio_system.h"
#ifndef MINIAUDIO_IMPLEMENTATION
// Just declarations
#endif

#include "miniaudio.h"
#include <string>
#include <iostream>
#include <cstring>
#include <cmath>
#include <random>

// Procedural audio - generates sound effects without files
class AudioSystem {
public:
    ma_engine engine;
    bool initialized = false;

    // Sound state
    float battle_intensity = 0; // 0-1, drives ambient battle sounds
    float last_explosion_time = 0;
    float music_volume = 0.3f;
    float sfx_volume = 0.5f;

    // Procedural noise generator for battle ambience
    ma_device device;
    bool device_active = false;

    // Simple state for procedural audio
    float phase_sword = 0;
    float phase_march = 0;
    float phase_wind = 0;
    float explosion_decay = 0;
    float arrow_whoosh = 0;
    float cannon_boom = 0;
    float nuke_rumble = 0;
    std::mt19937 rng{42};

    bool init() {
        ma_engine_config cfg = ma_engine_config_init();
        cfg.channels = 2;
        cfg.sampleRate = 44100;
        if (ma_engine_init(&cfg, &engine) != MA_SUCCESS) {
            std::cerr << "Audio: Failed to init engine\n";
            return false;
        }
        initialized = true;

        // Setup custom device for procedural audio
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

        std::cout << "Audio: Initialized (procedural)\n";
        return true;
    }

    void trigger_explosion() { explosion_decay = 1.0f; }
    void trigger_arrow() { arrow_whoosh = 1.0f; }
    void trigger_cannon() { cannon_boom = 1.0f; }
    void trigger_nuke() { nuke_rumble = 2.0f; }

    void update(float dt, float intensity) {
        battle_intensity = intensity;
        // Decay effects
        if (explosion_decay > 0) explosion_decay -= dt * 2.0f;
        if (arrow_whoosh > 0) arrow_whoosh -= dt * 4.0f;
        if (cannon_boom > 0) cannon_boom -= dt * 1.5f;
        if (nuke_rumble > 0) nuke_rumble -= dt * 0.3f;
    }

    void cleanup() {
        if (device_active) {
            ma_device_uninit(&device);
            device_active = false;
        }
        if (initialized) {
            ma_engine_uninit(&engine);
            initialized = false;
        }
    }

private:
    static void audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
        AudioSystem* self = (AudioSystem*)pDevice->pUserData;
        float* out = (float*)pOutput;
        float dt = 1.0f / 44100.0f;

        for (ma_uint32 i = 0; i < frameCount; i++) {
            float sample = 0;

            // === Battle ambience (metal clashing, distant yells) ===
            float intensity = self->battle_intensity;
            if (intensity > 0.01f) {
                // Metallic clashing (random burst of high freq)
                self->phase_sword += dt * (2000.0f + sin(self->phase_sword * 0.1f) * 500.0f);
                float sword = sin(self->phase_sword) * 0.03f * intensity;
                sword *= (sin(self->phase_march * 7.3f) > 0.7f) ? 1.0f : 0.0f; // intermittent

                // Marching rumble (low freq)
                self->phase_march += dt * 3.0f;
                float march = sin(self->phase_march * 6.28f) * 0.05f * intensity;
                march += sin(self->phase_march * 12.56f) * 0.02f * intensity;

                // Distant shouting (filtered noise bursts)
                float noise = ((float)(self->rng() % 10000) / 10000.0f - 0.5f) * 0.04f * intensity;
                float shout_env = (sin(self->phase_march * 0.5f) > 0.6f) ? 1.0f : 0.2f;
                noise *= shout_env;

                sample += sword + march + noise;
            }

            // === Wind (always present, gentle) ===
            self->phase_wind += dt;
            float wind_noise = ((float)(self->rng() % 10000) / 10000.0f - 0.5f);
            float wind_env = sin(self->phase_wind * 0.3f) * 0.5f + 0.5f;
            sample += wind_noise * 0.008f * wind_env;

            // === Explosion ===
            if (self->explosion_decay > 0) {
                float boom = sin(self->explosion_decay * 80.0f) * self->explosion_decay;
                boom += ((float)(self->rng() % 10000) / 10000.0f - 0.5f) * self->explosion_decay * 0.5f;
                sample += boom * 0.15f;
            }

            // === Cannon boom ===
            if (self->cannon_boom > 0) {
                float t = 1.0f - self->cannon_boom;
                float boom = sin(t * 150.0f) * exp(-t * 8.0f);
                boom += ((float)(self->rng() % 10000) / 10000.0f - 0.5f) * self->cannon_boom * 0.3f;
                sample += boom * 0.2f;
            }

            // === Arrow whoosh ===
            if (self->arrow_whoosh > 0) {
                float t = 1.0f - self->arrow_whoosh;
                float whoosh = sin(t * 800.0f + sin(t * 200.0f) * 3.0f) * exp(-t * 10.0f);
                sample += whoosh * 0.04f;
            }

            // === Nuke rumble ===
            if (self->nuke_rumble > 0) {
                float t = 2.0f - self->nuke_rumble;
                // Deep rumble + crackle
                float rumble = sin(t * 30.0f) * exp(-t * 0.5f);
                rumble += sin(t * 50.0f) * 0.5f * exp(-t * 0.8f);
                float crackle = ((float)(self->rng() % 10000) / 10000.0f - 0.5f) * exp(-t * 1.5f);
                sample += (rumble * 0.3f + crackle * 0.15f) * glm::min(self->nuke_rumble, 1.0f);
            }

            // === Simple music: pentatonic ambient melody ===
            static float music_phase = 0;
            static float note_timer = 0;
            static int note_idx = 0;
            static const float notes[] = {220.0f, 261.6f, 293.7f, 349.2f, 392.0f, 440.0f, 523.3f};
            note_timer += dt;
            if (note_timer > 2.0f) { note_timer = 0; note_idx = (note_idx + 1) % 7; }
            music_phase += dt * notes[note_idx];
            float melody = sin(music_phase * 6.28f) * 0.015f;
            melody += sin(music_phase * 6.28f * 2.0f) * 0.005f; // harmonic
            float music_env = sin(note_timer * 3.14159f / 2.0f); // fade in
            melody *= music_env * self->music_volume;
            sample += melody;

            // Soft clip
            sample = glm::clamp(sample * self->sfx_volume, -0.8f, 0.8f);

            // Stereo
            out[i * 2 + 0] = sample;
            out[i * 2 + 1] = sample;
        }
        (void)pInput;
    }
};
