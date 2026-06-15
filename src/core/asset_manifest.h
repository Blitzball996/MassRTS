#pragma once
// ============================================================================
// ASSET MANIFEST  —  data-driven art swapping without touching code.
//
// Drop an `assets/manifest.json` next to the executable (or in the project
// root). It lets you remap each unit's 3D model and per-unit render scale,
// override sound files, and tune particle/effect colors — all without
// recompiling.
//
// EVERYTHING IS OPTIONAL. Missing file, missing keys, or malformed JSON all
// fall back to built-in defaults. The game never crashes on a bad manifest.
//
// Example assets/manifest.json:
// {
//   "models": {
//     "infantry": { "file": "knight.obj", "scale": 1.2 },
//     "archer":   { "file": "ranger.obj", "scale": 1.0 },
//     "artillery":{ "file": "cannon.obj", "scale": 1.5 }
//   },
//   "audio": {
//     "bgm_menu": "music/my_menu.wav",
//     "sword_hit": "sfx/clang.wav"
//   },
//   "effects": {
//     "cannon_blast": { "r": 1.0, "g": 0.5, "b": 0.1, "count": 40 },
//     "nuke":         { "r": 1.0, "g": 0.9, "b": 0.4 }
//   }
// }
// ============================================================================
#include "json.h"
#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <iostream>

struct ModelOverride {
    std::string file;       // filename relative to assets/models/ (e.g. "knight.obj")
    float scale = 1.0f;     // extra render-time scale multiplier
    bool present = false;
};

struct EffectOverride {
    float r = -1, g = -1, b = -1;  // -1 = use default
    int count = -1;                 // -1 = use default
    bool present = false;
};

class AssetManifest {
public:
    std::map<std::string, ModelOverride> models;
    std::map<std::string, std::string>   audio;   // logical name -> file path
    std::map<std::string, EffectOverride> effects;
    bool loaded = false;

    // Try a list of candidate paths (cwd may be project root or build/Release).
    void load() {
        const char* candidates[] = {
            "assets/manifest.json", "../assets/manifest.json",
            "../../assets/manifest.json", "manifest.json"
        };
        for (const char* path : candidates) {
            std::ifstream probe(path);
            if (!probe.good()) continue;
            probe.close();
            JsonValue root = JsonParser::parse_file(path);
            if (root.is_null() || !root.is_object()) {
                std::cerr << "[manifest] " << path << " present but invalid JSON; using defaults\n";
                return;
            }
            parse_models(root["models"]);
            parse_audio(root["audio"]);
            parse_effects(root["effects"]);
            loaded = true;
            std::cout << "[manifest] loaded " << path << ": "
                      << models.size() << " models, "
                      << audio.size() << " audio, "
                      << effects.size() << " effects\n";
            return;
        }
        // No manifest found — totally fine, defaults apply.
    }

    const ModelOverride* model(const std::string& unit) const {
        auto it = models.find(unit);
        return it == models.end() ? nullptr : &it->second;
    }
    std::string audio_path(const std::string& name, const std::string& def) const {
        auto it = audio.find(name);
        return it == audio.end() ? def : it->second;
    }
    const EffectOverride* effect(const std::string& name) const {
        auto it = effects.find(name);
        return it == effects.end() ? nullptr : &it->second;
    }

private:
    void parse_models(const JsonValue& m) {
        if (!m.is_object()) return;
        for (const auto& kv : m.obj) {
            const JsonValue& e = kv.second;
            ModelOverride mo;
            mo.file = e["file"].as_string("");
            mo.scale = (float)e["scale"].as_number(1.0);
            mo.present = !mo.file.empty();
            if (mo.present) models[kv.first] = mo;
        }
    }
    void parse_audio(const JsonValue& a) {
        if (!a.is_object()) return;
        for (const auto& kv : a.obj) {
            std::string f = kv.second.as_string("");
            if (!f.empty()) audio[kv.first] = f;
        }
    }
    void parse_effects(const JsonValue& fx) {
        if (!fx.is_object()) return;
        for (const auto& kv : fx.obj) {
            const JsonValue& e = kv.second;
            EffectOverride eo;
            eo.r = (float)e["r"].as_number(-1);
            eo.g = (float)e["g"].as_number(-1);
            eo.b = (float)e["b"].as_number(-1);
            eo.count = (int)e["count"].as_number(-1);
            eo.present = true;
            effects[kv.first] = eo;
        }
    }
};

// Global manifest instance (loaded once at startup).
inline AssetManifest g_manifest;
