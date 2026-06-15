#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <iostream>

struct GameConfig {
    // Display
    int window_width = 1600;
    int window_height = 900;
    bool fullscreen = false;
    bool vsync = true;

    // Balance
    int starting_money_red = 5000;
    int starting_money_blue = 5000;
    int income_rate = 50;
    int win_score = 200;

    // AI
    float target_search_range = 500.0f;
    float artillery_search_range = 600.0f;
    float retreat_threshold = 0.2f;
    float shield_retreat_threshold = 0.1f;

    // Maps
    int current_map = 1;

    // Unit stats (indexed by shop order)
    struct UnitDef {
        int cost; float hp, damage, range, speed;
    };
    UnitDef units[9] = {
        {20,  60, 6, 6, 5},      // Militia
        {50, 100, 10, 8, 6},     // Infantry
        {60,  60, 12, 100, 4},   // Archer
        {80, 200, 8, 6, 4},      // Shield
        {120, 150, 18, 8, 12},   // Cavalry
        {100, 80, 80, 12, 7},    // Bomber
        {200, 70, 40, 200, 2},   // Artillery
        {30, 500, 0, 0, 0},      // Wall
        {150, 300, 25, 120, 0},  // Turret
    };

    void load(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cout << "[Config] No config file found, using defaults\n";
            return;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '/' || line[0] == '[') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            // trim
            while (!key.empty() && key.back() == ' ') key.pop_back();
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());

            if (key == "window_width") window_width = std::stoi(val);
            else if (key == "window_height") window_height = std::stoi(val);
            else if (key == "starting_money_red") starting_money_red = std::stoi(val);
            else if (key == "starting_money_blue") starting_money_blue = std::stoi(val);
            else if (key == "income_rate") income_rate = std::stoi(val);
            else if (key == "win_score") win_score = std::stoi(val);
            else if (key == "target_search_range") target_search_range = std::stof(val);
            else if (key == "artillery_search_range") artillery_search_range = std::stof(val);
            else if (key == "retreat_threshold") retreat_threshold = std::stof(val);
            else if (key == "current_map") current_map = std::stoi(val);
            // Unit costs
            else if (key == "militia_cost") units[0].cost = std::stoi(val);
            else if (key == "militia_hp") units[0].hp = std::stof(val);
            else if (key == "militia_damage") units[0].damage = std::stof(val);
            else if (key == "infantry_cost") units[1].cost = std::stoi(val);
            else if (key == "infantry_hp") units[1].hp = std::stof(val);
            else if (key == "infantry_damage") units[1].damage = std::stof(val);
            else if (key == "archer_cost") units[2].cost = std::stoi(val);
            else if (key == "archer_hp") units[2].hp = std::stof(val);
            else if (key == "archer_damage") units[2].damage = std::stof(val);
            else if (key == "cavalry_cost") units[4].cost = std::stoi(val);
            else if (key == "cavalry_hp") units[4].hp = std::stof(val);
            else if (key == "cavalry_speed") units[4].speed = std::stof(val);
            else if (key == "artillery_cost") units[6].cost = std::stoi(val);
            else if (key == "artillery_damage") units[6].damage = std::stof(val);
            else if (key == "artillery_range") units[6].range = std::stof(val);
        }
        std::cout << "[Config] Loaded from " << path << "\n";
    }
};

static GameConfig g_config;
