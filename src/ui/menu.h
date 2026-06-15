#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <cstdio>
#include "../game/game_state.h"

// ============================================================================
// MENU RENDERER - Main menu + Map selection + Victory/Defeat screens
// Also contains the clickable SHOP PANEL for in-game UI
// ============================================================================

class MenuRenderer {
public:
    GLuint shader = 0;
    GLuint vao = 0, vbo = 0;
    int screen_w = 1600, screen_h = 900;

    // Shop button layout
    static constexpr int SHOP_COLS = 3;
    static constexpr int SHOP_ROWS = 3;
    static constexpr float BTN_W = 120.0f;
    static constexpr float BTN_H = 50.0f;
    static constexpr float BTN_PAD = 6.0f;
    float shop_panel_x = 10.0f;
    float shop_panel_y = 100.0f;

    bool init(int w, int h) {
        screen_w = w; screen_h = h;
        const char* vs = R"(
#version 330 core
layout(location=0) in vec2 a_pos;
uniform vec4 u_rect;
uniform vec2 u_screen;
void main() {
    vec2 p = u_rect.xy + a_pos * u_rect.zw;
    vec2 ndc = p / u_screen * 2.0 - 1.0;
    ndc.y = -ndc.y;
    gl_Position = vec4(ndc, 0, 1);
}
)";
        const char* fs = R"(
#version 330 core
uniform vec4 u_color;
out vec4 frag;
void main() { frag = u_color; }
)";
        GLuint v = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(v, 1, &vs, nullptr); glCompileShader(v);
        GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(f, 1, &fs, nullptr); glCompileShader(f);
        shader = glCreateProgram();
        glAttachShader(shader, v); glAttachShader(shader, f);
        glLinkProgram(shader);
        glDeleteShader(v); glDeleteShader(f);

        float quad[] = {0,0, 1,0, 1,1, 0,0, 1,1, 0,1};
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        return true;
    }

    void draw_rect(float x, float y, float w, float h, glm::vec4 color) {
        glUniform4f(glGetUniformLocation(shader, "u_rect"), x, y, w, h);
        glUniform4f(glGetUniformLocation(shader, "u_color"), color.r, color.g, color.b, color.a);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // 7-segment digit + integer rendering (so shop shows real costs/counts)
    void draw_digit(float x, float y, int d, glm::vec4 color, float scale) {
        float sw=8*scale, sh=2*scale, sv=8*scale, st=2*scale;
        static const uint8_t segs[10]={0b1110111,0b0010010,0b1011101,0b1011011,
            0b0111010,0b1101011,0b1101111,0b1010010,0b1111111,0b1111011};
        uint8_t s=segs[d];
        if(s&0b1000000) draw_rect(x+st,y,sw-st*2,sh,color);
        if(s&0b0100000) draw_rect(x,y+sh,st,sv,color);
        if(s&0b0010000) draw_rect(x+sw-st,y+sh,st,sv,color);
        if(s&0b0001000) draw_rect(x+st,y+sv+sh,sw-st*2,sh,color);
        if(s&0b0000100) draw_rect(x,y+sv+sh*2,st,sv,color);
        if(s&0b0000010) draw_rect(x+sw-st,y+sv+sh*2,st,sv,color);
        if(s&0b0000001) draw_rect(x+st,y+sv*2+sh*2,sw-st*2,sh,color);
    }
    float draw_number(float x, float y, int value, glm::vec4 color, float scale) {
        if(value<0) value=0;
        char buf[12]; snprintf(buf,sizeof(buf),"%d",value);
        float cx=x, dw=10*scale;
        for(int i=0;buf[i];i++){ if(buf[i]>='0'&&buf[i]<='9') draw_digit(cx,y,buf[i]-'0',color,scale); cx+=dw+2*scale; }
        return cx; // returns x after last digit
    }

    // ========================================================================
    // MAIN MENU
    // ========================================================================
    void render_main_menu(GameState& state, float mx, float my) {
        glUseProgram(shader);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Full screen dark overlay
        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.02f, 0.02f, 0.05f, 0.95f});

        // Title area
        float cx = screen_w * 0.5f;
        float ty = screen_h * 0.15f;
        draw_rect(cx - 200, ty, 400, 60, {0.1f, 0.2f, 0.1f, 0.9f});
        draw_rect(cx - 200, ty, 400, 3, {0.3f, 0.9f, 0.3f, 0.8f});
        // "MASS RTS" title indicator bars
        for (int i = 0; i < 7; i++) {
            float bx = cx - 140 + i * 42;
            draw_rect(bx, ty + 15, 36, 30, {0.15f + i*0.08f, 0.6f, 0.2f, 0.9f});
        }

        // "Start Battle" button
        float btn_y = screen_h * 0.45f;
        bool hover_start = (mx > cx-150 && mx < cx+150 && my > btn_y && my < btn_y+55);
        glm::vec4 btn_col = hover_start ? glm::vec4(0.15f, 0.4f, 0.15f, 0.95f) : glm::vec4(0.08f, 0.25f, 0.08f, 0.9f);
        draw_rect(cx - 150, btn_y, 300, 55, btn_col);
        draw_rect(cx - 150, btn_y, 300, 2, {0.4f, 1.0f, 0.4f, 0.7f});
        // Arrow indicator
        draw_rect(cx - 20, btn_y + 18, 40, 20, {0.3f, 0.9f, 0.3f, 0.9f});
        state.menu_hover = hover_start ? 0 : -1;

        // "Select Map" button
        float btn2_y = btn_y + 80;
        bool hover_map = (mx > cx-150 && mx < cx+150 && my > btn2_y && my < btn2_y+55);
        btn_col = hover_map ? glm::vec4(0.12f, 0.12f, 0.35f, 0.95f) : glm::vec4(0.06f, 0.06f, 0.2f, 0.9f);
        draw_rect(cx - 150, btn2_y, 300, 55, btn_col);
        draw_rect(cx - 150, btn2_y, 300, 2, {0.4f, 0.4f, 1.0f, 0.7f});
        draw_rect(cx - 20, btn2_y + 18, 40, 20, {0.4f, 0.4f, 1.0f, 0.9f});
        if (hover_map) state.menu_hover = 1;

        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    // ========================================================================
    // MAP SELECTION SCREEN
    // ========================================================================
    void render_map_select(GameState& state, float mx, float my) {
        glUseProgram(shader);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.02f, 0.02f, 0.05f, 0.95f});

        // Title
        float cx = screen_w * 0.5f;
        draw_rect(cx - 150, 40, 300, 40, {0.1f, 0.1f, 0.3f, 0.9f});

        // Map cards (2 columns x 3 rows)
        state.menu_hover = -1;
        for (int i = 0; i < MAP_COUNT; i++) {
            int col = i % 2, row = i / 2;
            float card_x = cx - 320 + col * 340;
            float card_y = 120 + row * 180;
            float card_w = 300, card_h = 150;

            bool hover = (mx > card_x && mx < card_x+card_w && my > card_y && my < card_y+card_h);
            bool selected = (state.selected_map == i);
            glm::vec4 bg = {0.05f, 0.08f, 0.05f, 0.9f};
            if (hover) bg = {0.1f, 0.2f, 0.1f, 0.95f};
            if (selected) bg = {0.1f, 0.3f, 0.1f, 0.95f};

            draw_rect(card_x, card_y, card_w, card_h, bg);
            // Border
            glm::vec4 border = selected ? glm::vec4(0.3f,1.0f,0.3f,0.9f) : glm::vec4(0.2f,0.4f,0.2f,0.6f);
            draw_rect(card_x, card_y, card_w, 2, border);
            draw_rect(card_x, card_y+card_h-2, card_w, 2, border);
            draw_rect(card_x, card_y, 2, card_h, border);
            draw_rect(card_x+card_w-2, card_y, 2, card_h, border);

            // Terrain preview mini-bars (represent terrain type visually)
            float preview_y = card_y + 30;
            float bar_h = 80;
            // Height bars
            for (int b = 0; b < 20; b++) {
                float bh = 10.0f + 40.0f * MAP_PRESETS[i].height_scale * 
                    (sin(b * 0.8f + MAP_PRESETS[i].seed * 0.01f) * 0.5f + 0.5f);
                bh = std::min(bh, bar_h);
                float bx = card_x + 20 + b * 13;
                glm::vec4 terrain_col = {0.2f, 0.5f + bh/bar_h*0.3f, 0.15f, 0.8f};
                if (MAP_PRESETS[i].has_river && b >= 9 && b <= 11)
                    terrain_col = {0.1f, 0.3f, 0.7f, 0.8f};
                draw_rect(bx, preview_y + bar_h - bh, 10, bh, terrain_col);
            }

            // Selected indicator
            if (selected) {
                draw_rect(card_x + card_w - 30, card_y + 5, 25, 25, {0.2f, 0.9f, 0.2f, 0.9f});
            }

            if (hover) state.menu_hover = i;
        }

        // Confirm button at bottom
        float btn_y = screen_h - 80;
        bool hover_confirm = (mx > cx-100 && mx < cx+100 && my > btn_y && my < btn_y+50);
        glm::vec4 cc = hover_confirm ? glm::vec4(0.2f,0.5f,0.2f,1.0f) : glm::vec4(0.1f,0.3f,0.1f,0.9f);
        draw_rect(cx - 100, btn_y, 200, 50, cc);
        draw_rect(cx - 100, btn_y, 200, 2, {0.4f, 1.0f, 0.4f, 0.8f});
        if (hover_confirm) state.menu_hover = 100; // special: confirm

        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    // ========================================================================
    // VICTORY / DEFEAT SCREEN
    // ========================================================================
    void render_end_screen(GameState& state, bool victory, float mx, float my) {
        glUseProgram(shader);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Dark overlay
        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.0f, 0.0f, 0.0f, 0.7f});

        float cx = screen_w * 0.5f;
        float cy = screen_h * 0.35f;

        // Big banner
        glm::vec4 banner_col = victory ? glm::vec4(0.05f,0.2f,0.05f,0.95f) : glm::vec4(0.2f,0.02f,0.02f,0.95f);
        draw_rect(cx - 250, cy, 500, 80, banner_col);
        glm::vec4 border_col = victory ? glm::vec4(0.3f,1.0f,0.3f,0.9f) : glm::vec4(1.0f,0.2f,0.2f,0.9f);
        draw_rect(cx - 250, cy, 500, 3, border_col);
        draw_rect(cx - 250, cy+77, 500, 3, border_col);

        // Victory/Defeat icon (colored block)
        glm::vec4 icon_col = victory ? glm::vec4(0.2f,1.0f,0.2f,1.0f) : glm::vec4(1.0f,0.1f,0.1f,1.0f);
        draw_rect(cx - 30, cy + 20, 60, 40, icon_col);

        // Stats panel
        float stats_y = cy + 120;
        draw_rect(cx - 200, stats_y, 400, 120, {0.05f, 0.05f, 0.08f, 0.9f});
        // Territory bars
        float red_pct = state.red_territory / 5.0f;
        float blue_pct = state.blue_territory / 5.0f;
        draw_rect(cx - 180, stats_y + 20, 360 * red_pct, 20, {0.8f, 0.2f, 0.15f, 0.9f});
        draw_rect(cx - 180, stats_y + 50, 360 * blue_pct, 20, {0.15f, 0.2f, 0.8f, 0.9f});
        // Time indicator
        draw_rect(cx - 180, stats_y + 85, 360 * std::min(state.match_time / 600.0f, 1.0f), 12, {0.5f, 0.5f, 0.5f, 0.7f});

        // "Return to Menu" button
        float btn_y = screen_h * 0.75f;
        bool hover_btn = (mx > cx-120 && mx < cx+120 && my > btn_y && my < btn_y+50);
        glm::vec4 btn_col = hover_btn ? glm::vec4(0.15f,0.15f,0.3f,0.95f) : glm::vec4(0.08f,0.08f,0.2f,0.9f);
        draw_rect(cx - 120, btn_y, 240, 50, btn_col);
        draw_rect(cx - 120, btn_y, 240, 2, {0.5f, 0.5f, 1.0f, 0.7f});
        state.menu_hover = hover_btn ? 0 : -1;

        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    // ========================================================================
    // IN-GAME SHOP PANEL (Clickable buttons like Red Alert)
    // ========================================================================
    // Returns: index of clicked shop button, or -1
    int render_shop_panel(float mx, float my, bool mouse_clicked,
                          const char* names[], const int costs[], int count,
                          int money, int buy_count) {
        glUseProgram(shader);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        int clicked = -1;
        float px = shop_panel_x;
        float py = shop_panel_y;

        // Panel background
        float panel_w = SHOP_COLS * (BTN_W + BTN_PAD) + BTN_PAD;
        float panel_h = ((count + SHOP_COLS - 1) / SHOP_COLS) * (BTN_H + BTN_PAD) + BTN_PAD + 30;
        draw_rect(px, py, panel_w, panel_h, {0.03f, 0.05f, 0.03f, 0.85f});
        draw_rect(px, py, panel_w, 2, {0.3f, 0.7f, 0.3f, 0.6f});

        // Header bar
        draw_rect(px, py, panel_w, 24, {0.06f, 0.12f, 0.06f, 0.9f});

        // Buy count indicator on header (shows the actual batch size)
        draw_rect(px + panel_w - 80, py + 3, 75, 18, {0.1f, 0.08f, 0.02f, 0.8f});
        draw_number(px + panel_w - 74, py + 6, buy_count, {1.0f, 0.85f, 0.3f, 1.0f}, 1.0f);

        float start_y = py + 30;

        for (int i = 0; i < count; i++) {
            int col = i % SHOP_COLS;
            int row = i / SHOP_COLS;
            float bx = px + BTN_PAD + col * (BTN_W + BTN_PAD);
            float by = start_y + row * (BTN_H + BTN_PAD);

            bool hover = (mx > bx && mx < bx + BTN_W && my > by && my < by + BTN_H);
            bool affordable = (costs[i] * buy_count <= money);

            glm::vec4 bg;
            if (!affordable) bg = {0.08f, 0.05f, 0.05f, 0.8f};  // red tint = can't afford
            else if (hover) bg = {0.1f, 0.25f, 0.1f, 0.95f};    // highlight
            else bg = {0.06f, 0.1f, 0.06f, 0.85f};               // normal

            draw_rect(bx, by, BTN_W, BTN_H, bg);

            // Border
            glm::vec4 brd = hover ? glm::vec4(0.4f,1.0f,0.4f,0.8f) : glm::vec4(0.2f,0.4f,0.2f,0.5f);
            if (!affordable) brd = {0.5f, 0.2f, 0.2f, 0.6f};
            draw_rect(bx, by, BTN_W, 1, brd);
            draw_rect(bx, by+BTN_H-1, BTN_W, 1, brd);

            // Unit type color indicator (left strip)
            glm::vec4 type_col;
            switch(i) {
                case 0: type_col = {0.7f,0.7f,0.2f,0.9f}; break; // militia-yellow
                case 1: type_col = {0.3f,0.6f,0.3f,0.9f}; break; // infantry-green
                case 2: type_col = {0.6f,0.3f,0.1f,0.9f}; break; // cavalry-brown
                case 3: type_col = {0.2f,0.5f,0.7f,0.9f}; break; // archer-blue
                case 4: type_col = {0.8f,0.3f,0.1f,0.9f}; break; // bomber-orange
                case 5: type_col = {0.4f,0.4f,0.5f,0.9f}; break; // artillery-grey
                case 6: type_col = {0.5f,0.5f,0.6f,0.9f}; break; // shield-silver
                case 7: type_col = {0.8f,0.1f,0.1f,0.9f}; break; // samurai-red
                case 8: type_col = {0.6f,0.4f,0.2f,0.9f}; break; // wall
                default: type_col = {0.5f,0.5f,0.5f,0.9f}; break;
            }
            draw_rect(bx, by + 2, 4, BTN_H - 4, type_col);

            // Unit icon (simple block figure)
            float icon_x = bx + 10;
            float icon_y = by + 8;
            draw_rect(icon_x, icon_y, 8, 8, type_col);        // head
            draw_rect(icon_x - 1, icon_y + 9, 10, 14, type_col * 0.7f);  // body
            draw_rect(icon_x + 1, icon_y + 24, 3, 8, type_col * 0.5f);  // legs
            draw_rect(icon_x + 5, icon_y + 24, 3, 8, type_col * 0.5f);

            // Cost indicator (bottom right) with the REAL unit cost
            draw_rect(bx + BTN_W - 46, by + BTN_H - 16, 42, 13, {0.02f,0.02f,0.02f,0.6f});
            // Gold coin icon
            draw_rect(bx + BTN_W - 44, by + BTN_H - 14, 8, 9, {0.9f,0.75f,0.1f,0.9f});
            glm::vec4 cost_col = affordable ? glm::vec4(0.95f,0.85f,0.35f,1.0f)
                                            : glm::vec4(0.9f,0.35f,0.35f,1.0f);
            draw_number(bx + BTN_W - 33, by + BTN_H - 14, costs[i], cost_col, 0.85f);

            // Hotkey number top-right (1..9)
            draw_rect(bx + BTN_W - 16, by + 2, 14, 14, {0.0f,0.0f,0.0f,0.4f});
            if (i < 9) draw_number(bx + BTN_W - 12, by + 4, i + 1, {0.7f,0.85f,0.7f,0.9f}, 0.8f);

            if (hover && mouse_clicked && affordable) {
                clicked = i;
            }
        }

        glDisable(GL_BLEND);
        glBindVertexArray(0);
        return clicked;
    }

    // ========================================================================
    // TERRITORY HUD (capture points status bar at top)
    // ========================================================================
    void render_territory_bar(const GameState& state) {
        glUseProgram(shader);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        float bar_y = 48; // below main HUD bar
        float cx = screen_w * 0.5f;
        float total_w = 400.0f;
        float dot_r = 16.0f;

        draw_rect(cx - total_w/2 - 10, bar_y, total_w + 20, 28, {0.02f, 0.02f, 0.04f, 0.8f});

        for (int i = 0; i < (int)state.capture_points.size(); i++) {
            float dx = cx - total_w/2 + (i + 0.5f) * (total_w / state.capture_points.size());
            const auto& cp = state.capture_points[i];

            glm::vec4 dot_col;
            if (cp.owner == 0) dot_col = {0.9f, 0.2f, 0.15f, 0.9f};
            else if (cp.owner == 1) dot_col = {0.15f, 0.2f, 0.9f, 0.9f};
            else dot_col = {0.4f, 0.4f, 0.4f, 0.7f};

            draw_rect(dx - dot_r/2, bar_y + 6, dot_r, dot_r, dot_col);

            // Capture progress bar below dot
            float prog = std::max(cp.capture_progress[0], cp.capture_progress[1]) / cp.capture_threshold;
            if (cp.owner == -1 && prog > 0.01f) {
                glm::vec4 prog_col = cp.capture_progress[0] > cp.capture_progress[1] ?
                    glm::vec4(0.9f,0.3f,0.2f,0.7f) : glm::vec4(0.2f,0.3f,0.9f,0.7f);
                draw_rect(dx - dot_r/2, bar_y + 23, dot_r * prog, 3, prog_col);
            }
        }

        // Victory timer indicator
        if (state.winning_faction >= 0) {
            float timer_pct = state.victory_timer / state.victory_threshold;
            glm::vec4 timer_col = (state.winning_faction == 0) ?
                glm::vec4(0.9f, 0.2f, 0.1f, 0.8f) : glm::vec4(0.1f, 0.2f, 0.9f, 0.8f);
            draw_rect(cx - total_w/2, bar_y + 26, total_w * timer_pct, 2, timer_col);
        }

        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    // ========================================================================
    // RALLY POINT INDICATOR (rendered in world-space via separate pass)
    // ========================================================================
    void render_rally_indicator(float screen_x, float screen_y, float pulse) {
        glUseProgram(shader);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        float size = 12.0f + sin(pulse) * 3.0f;
        float alpha = 0.6f + sin(pulse) * 0.2f;

        // Flag pole
        draw_rect(screen_x, screen_y - 30, 2, 30, {0.4f, 0.4f, 0.4f, alpha});
        // Flag
        draw_rect(screen_x + 2, screen_y - 30, 18, 12, {0.2f, 0.9f, 0.2f, alpha});
        // Ring (4 rects forming diamond)
        draw_rect(screen_x - size, screen_y - 2, size*2, 4, {0.2f, 1.0f, 0.3f, alpha * 0.5f});
        draw_rect(screen_x - 2, screen_y - size, 4, size*2, {0.2f, 1.0f, 0.3f, alpha * 0.5f});

        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    void cleanup() {
        if (shader) glDeleteProgram(shader);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
    }
};
