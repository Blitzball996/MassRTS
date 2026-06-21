#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <cstdio>
#include "../game/game_state.h"
#include "text_renderer.h"

// ============================================================================
// MENU RENDERER - Main menu + Map selection + Victory/Defeat screens
// Also contains the clickable SHOP PANEL for in-game UI
// ============================================================================

class MenuRenderer {
public:
    GLuint shader = 0;
    GLuint vao = 0, vbo = 0;
    int screen_w = 1600, screen_h = 900;
    TextRenderer font;

    // Draw a text string at (x,y) with given pixel scale and color.
    void draw_text(const std::string& s, float x, float y, float scale, glm::vec4 color) {
        font.draw(s, x, y, scale, [&](float cx, float cy, float cw, float ch) {
            draw_rect(cx, cy, cw, ch, color);
        });
    }
    // Draw text horizontally centered around cx.
    void draw_text_centered(const std::string& s, float cx, float y, float scale, glm::vec4 color) {
        draw_text(s, cx - font.measure(s, scale) * 0.5f, y, scale, color);
    }

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
    // MAIN MENU  (Minecraft-console style: title + vertical labeled buttons)
    // ========================================================================
    void render_main_menu(GameState& state, float mx, float my) {
        glUseProgram(shader);
        glDisable(GL_DEPTH_TEST);  // 2D overlay must not be depth-culled
        glDepthMask(GL_FALSE);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Background: dark gradient panels (faux sky)
        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.04f, 0.06f, 0.10f, 1.0f});
        draw_rect(0, 0, (float)screen_w, screen_h * 0.5f, {0.06f, 0.09f, 0.16f, 1.0f});

        float cx = screen_w * 0.5f;

        // === Title ===
        float ty = screen_h * 0.13f;
        draw_text_centered("MASS RTS", cx, ty, 10.0f, {0.5f, 1.0f, 0.5f, 1.0f});
        draw_text_centered("100,000 UNIT WARFARE", cx, ty + 90, 3.0f, {0.5f, 0.6f, 0.7f, 0.9f});

        // === Vertical button list ===
        struct Btn { const char* label; int id; glm::vec3 tint; };
        Btn buttons[] = {
            {"START BATTLE", 0, {0.3f, 0.9f, 0.3f}},
            {"SURVIVAL",     3, {0.9f, 0.25f, 0.6f}},
            {"SELECT MAP",   1, {0.4f, 0.6f, 1.0f}},
            {"SETTINGS",     2, {0.9f, 0.7f, 0.3f}},
        };
        int n = sizeof(buttons) / sizeof(buttons[0]);

        float bw = 360, bh = 56, gap = 20;
        float start_y = screen_h * 0.42f;
        state.menu_hover = -1;

        for (int i = 0; i < n; i++) {
            float by = start_y + i * (bh + gap);
            bool hover = (mx > cx - bw*0.5f && mx < cx + bw*0.5f && my > by && my < by + bh);
            glm::vec3 t = buttons[i].tint;
            glm::vec4 bg = hover ? glm::vec4(t * 0.45f, 0.98f) : glm::vec4(t * 0.18f, 0.92f);
            // Button body
            draw_rect(cx - bw*0.5f, by, bw, bh, bg);
            // Left accent bar + top/bottom edge
            draw_rect(cx - bw*0.5f, by, 6, bh, glm::vec4(t, 1.0f));
            draw_rect(cx - bw*0.5f, by, bw, 2, glm::vec4(t, hover ? 0.9f : 0.5f));
            draw_rect(cx - bw*0.5f, by + bh - 2, bw, 2, glm::vec4(t * 0.6f, 0.6f));
            // Hover marker (>)
            if (hover) draw_text(">", cx - bw*0.5f + 18, by + bh*0.5f - 10, 3.0f, glm::vec4(t, 1.0f));
            // Label text
            glm::vec4 txt = hover ? glm::vec4(1,1,1,1) : glm::vec4(t * 0.9f + glm::vec3(0.1f), 1.0f);
            draw_text_centered(buttons[i].label, cx, by + bh*0.5f - 11, 3.2f, txt);
            if (hover) state.menu_hover = buttons[i].id;
        }

        // Footer hint
        draw_text_centered("CLICK TO SELECT", cx, screen_h - 50, 2.0f, {0.4f, 0.45f, 0.5f, 0.8f});

        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    // ========================================================================
    // PAUSE MENU  (ESC overlay drawn on top of the frozen battlefield)
    //   Buttons -> state.menu_hover: 10 = Resume, 11 = Settings, 12 = Quit
    // ========================================================================
    void render_pause_menu(GameState& state, float mx, float my) {
        glUseProgram(shader);
        glDisable(GL_DEPTH_TEST);  // 2D overlay must not be depth-culled
        glDepthMask(GL_FALSE);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Dim the frozen battlefield so the menu reads clearly on top of it.
        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.0f, 0.0f, 0.0f, 0.55f});

        float cx = screen_w * 0.5f;
        draw_text_centered("PAUSED", cx, screen_h * 0.20f, 8.0f, {0.7f, 0.85f, 1.0f, 1.0f});

        struct Btn { const char* label; int id; glm::vec3 tint; };
        Btn buttons[] = {
            {"RESUME",       10, {0.3f, 0.9f, 0.3f}},
            {"SETTINGS",     11, {0.9f, 0.7f, 0.3f}},
            {"QUIT TO MENU", 12, {0.9f, 0.4f, 0.3f}},
        };
        int n = sizeof(buttons) / sizeof(buttons[0]);

        float bw = 360, bh = 56, gap = 20;
        float start_y = screen_h * 0.40f;
        state.menu_hover = -1;

        for (int i = 0; i < n; i++) {
            float by = start_y + i * (bh + gap);
            bool hover = (mx > cx - bw*0.5f && mx < cx + bw*0.5f && my > by && my < by + bh);
            glm::vec3 t = buttons[i].tint;
            glm::vec4 bg = hover ? glm::vec4(t * 0.45f, 0.98f) : glm::vec4(t * 0.18f, 0.92f);
            draw_rect(cx - bw*0.5f, by, bw, bh, bg);
            draw_rect(cx - bw*0.5f, by, 6, bh, glm::vec4(t, 1.0f));
            draw_rect(cx - bw*0.5f, by, bw, 2, glm::vec4(t, hover ? 0.9f : 0.5f));
            draw_rect(cx - bw*0.5f, by + bh - 2, bw, 2, glm::vec4(t * 0.6f, 0.6f));
            if (hover) draw_text(">", cx - bw*0.5f + 18, by + bh*0.5f - 10, 3.0f, glm::vec4(t, 1.0f));
            glm::vec4 txt = hover ? glm::vec4(1,1,1,1) : glm::vec4(t * 0.9f + glm::vec3(0.1f), 1.0f);
            draw_text_centered(buttons[i].label, cx, by + bh*0.5f - 11, 3.2f, txt);
            if (hover) state.menu_hover = buttons[i].id;
        }

        draw_text_centered("ESC TO RESUME", cx, screen_h - 50, 2.0f, {0.4f, 0.45f, 0.5f, 0.8f});

        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    // ========================================================================
    // MAP SELECTION SCREEN
    // ========================================================================
    void render_map_select(GameState& state, float mx, float my) {
        glUseProgram(shader);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.02f, 0.03f, 0.08f, 1.0f});

        // Title
        float cx = screen_w * 0.5f;
        draw_text_centered("SELECT MAP", cx, 45, 4.5f, {0.5f, 0.8f, 1.0f, 1.0f});

        // Map cards (2 columns x 3 rows)
        state.menu_hover = -1;
        for (int i = 0; i < MAP_COUNT; i++) {
            int col = i % 2, row = i / 2;
            float card_x = cx - 340 + col * 360;
            float card_y = 120 + row * 165;
            float card_w = 320, card_h = 145;

            bool hover = (mx > card_x && mx < card_x+card_w && my > card_y && my < card_y+card_h);
            bool selected = (state.selected_map == i);
            glm::vec4 bg = {0.05f, 0.08f, 0.12f, 0.9f};
            if (hover) bg = {0.1f, 0.18f, 0.25f, 0.95f};
            if (selected) bg = {0.1f, 0.28f, 0.18f, 0.95f};

            draw_rect(card_x, card_y, card_w, card_h, bg);
            glm::vec4 border = selected ? glm::vec4(0.3f,1.0f,0.4f,0.9f) : glm::vec4(0.25f,0.4f,0.6f,0.6f);
            draw_rect(card_x, card_y, card_w, 2, border);
            draw_rect(card_x, card_y+card_h-2, card_w, 2, border);
            draw_rect(card_x, card_y, 2, card_h, border);
            draw_rect(card_x+card_w-2, card_y, 2, card_h, border);

            // Map name (title) + description
            draw_text(MAP_PRESETS[i].name, card_x + 16, card_y + 14, 2.6f,
                      selected ? glm::vec4(0.5f,1.0f,0.6f,1.0f) : glm::vec4(0.7f,0.85f,1.0f,1.0f));
            draw_text(MAP_PRESETS[i].description, card_x + 16, card_y + 44, 1.4f, {0.55f,0.65f,0.7f,0.95f});

            // Terrain preview mini-bars at the bottom of the card
            float preview_y = card_y + 70, bar_h = 55;
            for (int b = 0; b < 22; b++) {
                float bh = 8.0f + 35.0f * MAP_PRESETS[i].height_scale *
                    (sin(b * 0.8f + MAP_PRESETS[i].seed * 0.01f) * 0.5f + 0.5f);
                bh = std::min(bh, bar_h);
                float bx = card_x + 18 + b * 13;
                glm::vec4 terrain_col = {0.2f, 0.45f + bh/bar_h*0.3f, 0.15f, 0.85f};
                if (MAP_PRESETS[i].has_river && b >= 10 && b <= 12)
                    terrain_col = {0.1f, 0.3f, 0.7f, 0.85f};
                draw_rect(bx, preview_y + bar_h - bh, 10, bh, terrain_col);
            }

            if (selected) draw_text("OK", card_x + card_w - 40, card_y + 14, 2.2f, {0.3f,1.0f,0.3f,1.0f});
            if (hover) state.menu_hover = i;
        }

        // Confirm button at bottom
        float btn_y = screen_h - 80;
        // START button (right)
        bool hover_confirm = (mx > cx+20 && mx < cx+240 && my > btn_y && my < btn_y+50);
        glm::vec4 cc = hover_confirm ? glm::vec4(0.2f,0.6f,0.25f,1.0f) : glm::vec4(0.1f,0.35f,0.12f,0.95f);
        draw_rect(cx + 20, btn_y, 220, 50, cc);
        draw_rect(cx + 20, btn_y, 220, 2, {0.4f, 1.0f, 0.4f, 0.8f});
        draw_text_centered("START >", cx + 130, btn_y + 17, 3.0f,
                           hover_confirm ? glm::vec4(1,1,1,1) : glm::vec4(0.6f,1.0f,0.6f,1.0f));
        if (hover_confirm) state.menu_hover = 100; // special: confirm/start

        // BACK button (left)
        bool hover_back = (mx > cx-240 && mx < cx-20 && my > btn_y && my < btn_y+50);
        glm::vec4 bc = hover_back ? glm::vec4(0.15f,0.15f,0.25f,0.95f) : glm::vec4(0.08f,0.08f,0.15f,0.9f);
        draw_rect(cx - 240, btn_y, 220, 50, bc);
        draw_rect(cx - 240, btn_y, 220, 2, {0.5f, 0.5f, 0.9f, 0.7f});
        draw_text_centered("< BACK", cx - 130, btn_y + 17, 3.0f,
                           hover_back ? glm::vec4(1,1,1,1) : glm::vec4(0.6f,0.7f,0.9f,1.0f));
        if (hover_back) state.menu_hover = 101; // special: back to main menu

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

    // ========================================================================
    // SETTINGS SCREEN (Categorized: Video / Graphics / Audio / Controls)
    // MinecraftConsoles-inspired multi-page settings menu
    // ========================================================================
    void render_settings(GameState& state, float mx, float my) {
        if (state.settings_submenu == 0) {
            render_settings_main(state, mx, my);
        } else if (state.settings_submenu == 1) {
            render_settings_video(state, mx, my);
        } else if (state.settings_submenu == 2) {
            render_settings_graphics(state, mx, my);
        } else if (state.settings_submenu == 3) {
            render_settings_audio(state, mx, my);
        } else if (state.settings_submenu == 4) {
            render_settings_controls(state, mx, my);
        }

        // APPLY button (bottom-right) if settings changed
        if (state.settings_dirty && !state.settings_confirming && state.settings_submenu > 0) {
            glUseProgram(shader);
            glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
            glBindVertexArray(vao);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            float bx = screen_w - 240, by = screen_h - 80;
            bool hover = (mx > bx && mx < bx+220 && my > by && my < by+50);
            glm::vec4 bg = hover ? glm::vec4(0.2f,0.6f,0.3f,0.95f) : glm::vec4(0.15f,0.45f,0.2f,0.9f);
            draw_rect(bx, by, 220, 50, bg);
            draw_rect(bx, by, 220, 2, {0.4f, 1.0f, 0.5f, 0.9f});
            draw_text_centered("APPLY CHANGES", bx + 110, by + 17, 2.8f,
                               hover ? glm::vec4(1,1,1,1) : glm::vec4(0.7f,1.0f,0.8f,1.0f));
            if (hover) state.menu_hover = 400; // special: apply button
            glDisable(GL_BLEND); glBindVertexArray(0);
        }

        // Confirmation overlay (countdown after APPLY clicked or exit to menu)
        if (state.settings_confirming) {
            glUseProgram(shader);
            glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
            glBindVertexArray(vao);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Dark overlay
            draw_rect(0, 0, (float)screen_w, (float)screen_h, {0,0,0,0.75f});

            float cx = screen_w * 0.5f, cy = screen_h * 0.5f;
            draw_rect(cx - 300, cy - 120, 600, 240, {0.08f, 0.1f, 0.15f, 0.95f});
            draw_rect(cx - 300, cy - 120, 600, 2, {0.5f, 0.7f, 1.0f, 0.8f});

            char msg[64];
            snprintf(msg, 64, "Keep these settings? (%d)", (int)state.settings_confirm_timer);
            draw_text_centered(msg, cx, cy - 60, 3.2f, {1, 1, 1, 1});

            // YES button
            bool hover_yes = (mx > cx-160 && mx < cx-20 && my > cy+20 && my < cy+70);
            glm::vec4 yes_bg = hover_yes ? glm::vec4(0.2f,0.6f,0.3f,1) : glm::vec4(0.1f,0.4f,0.15f,0.9f);
            draw_rect(cx - 160, cy + 20, 140, 50, yes_bg);
            draw_text_centered("YES", cx - 90, cy + 35, 3.0f, hover_yes ? glm::vec4(1,1,1,1) : glm::vec4(0.7f,1,0.8f,1));
            if (hover_yes) state.menu_hover = 401;

            // NO button
            bool hover_no = (mx > cx+20 && mx < cx+160 && my > cy+20 && my < cy+70);
            glm::vec4 no_bg = hover_no ? glm::vec4(0.6f,0.2f,0.2f,1) : glm::vec4(0.4f,0.1f,0.1f,0.9f);
            draw_rect(cx + 20, cy + 20, 140, 50, no_bg);
            draw_text_centered("NO", cx + 90, cy + 35, 3.0f, hover_no ? glm::vec4(1,1,1,1) : glm::vec4(1,0.7f,0.7f,1));
            if (hover_no) state.menu_hover = 402;

            glDisable(GL_BLEND); glBindVertexArray(0);
        }
    }

    // Settings main page: category buttons
    void render_settings_main(GameState& state, float mx, float my) {
        glUseProgram(shader);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.02f, 0.03f, 0.08f, 1.0f});

        float cx = screen_w * 0.5f;
        draw_text_centered("SETTINGS", cx, 60, 5.0f, {1.0f, 0.85f, 0.5f, 1.0f});

        struct Cat { const char* label; int id; glm::vec3 tint; };
        Cat cats[] = {
            {"VIDEO", 1, {0.4f, 0.6f, 1.0f}},
            {"GRAPHICS", 2, {0.3f, 0.9f, 0.3f}},
            {"AUDIO", 3, {0.9f, 0.5f, 0.9f}},
            {"CONTROLS", 4, {0.9f, 0.7f, 0.3f}},
        };
        int n = 4;
        float bw = 320, bh = 65, gap = 25;
        float start_y = 180;
        state.menu_hover = -1;

        for (int i = 0; i < n; i++) {
            float by = start_y + i * (bh + gap);
            bool hover = (mx > cx - bw*0.5f && mx < cx + bw*0.5f && my > by && my < by + bh);
            glm::vec3 t = cats[i].tint;
            glm::vec4 bg = hover ? glm::vec4(t * 0.35f, 0.95f) : glm::vec4(t * 0.15f, 0.9f);
            draw_rect(cx - bw*0.5f, by, bw, bh, bg);
            draw_rect(cx - bw*0.5f, by, 5, bh, glm::vec4(t, 1.0f));
            draw_rect(cx - bw*0.5f, by, bw, 2, glm::vec4(t, hover ? 0.9f : 0.6f));
            if (hover) draw_text(">", cx - bw*0.5f + 15, by + bh*0.5f - 10, 3.0f, glm::vec4(t, 1.0f));
            glm::vec4 txt = hover ? glm::vec4(1,1,1,1) : glm::vec4(t * 0.9f + glm::vec3(0.1f), 1.0f);
            draw_text_centered(cats[i].label, cx, by + bh*0.5f - 12, 3.5f, txt);
            if (hover) state.menu_hover = 100 + cats[i].id;  // 101=Video, 102=Graphics...
        }

        // Back button
        float btn_y = screen_h - 90;
        bool hover_back = (mx > cx-120 && mx < cx+120 && my > btn_y && my < btn_y+55);
        glm::vec4 back_col = hover_back ? glm::vec4(0.15f,0.15f,0.25f,0.95f) : glm::vec4(0.08f,0.08f,0.15f,0.9f);
        draw_rect(cx - 120, btn_y, 240, 55, back_col);
        draw_rect(cx - 120, btn_y, 240, 2, {0.5f, 0.5f, 0.9f, 0.7f});
        draw_text_centered("< BACK TO MENU", cx, btn_y + 19, 3.0f, hover_back ? glm::vec4(1,1,1,1) : glm::vec4(0.6f,0.7f,0.9f,1.0f));
        if (hover_back) state.menu_hover = 999;  // back to main menu

        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    // Video settings page (resolution, fullscreen, vsync)
    // --- shared widgets for settings sub-pages ---
    // Begin a sub-page: dark bg + title + returns first row Y.
    float page_begin(GameState& state, const char* title, glm::vec3 tint) {
        glUseProgram(shader);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.02f, 0.03f, 0.08f, 1.0f});
        float cx = screen_w * 0.5f;
        draw_rect(cx - 250, 45, 500, 3, glm::vec4(tint, 0.8f));
        draw_text_centered(title, cx, 60, 4.5f, glm::vec4(tint + glm::vec3(0.2f), 1.0f));
        state.menu_hover = -1;
        return 150.0f;
    }
    // A labeled toggle row. Returns hover state; sets menu_hover=id if hovered.
    bool widget_toggle(GameState& state, float y, const char* label, bool on, int id, float mx, float my) {
        float cx = screen_w * 0.5f, rw = 460, rx = cx - rw*0.5f;
        draw_rect(rx, y, rw, 40, {0.05f, 0.07f, 0.10f, 0.85f});
        draw_text(label, rx + 18, y + 13, 2.2f, {0.85f, 0.9f, 0.95f, 1.0f});
        bool hover = (mx > rx && mx < rx+rw && my > y && my < y+40);
        glm::vec4 c = on ? glm::vec4(0.2f,0.8f,0.3f,0.95f) : glm::vec4(0.4f,0.4f,0.4f,0.7f);
        if (hover) c *= 1.2f;
        draw_rect(rx + rw - 90, y + 7, 70, 26, c);
        draw_text(on ? "ON" : "OFF", rx + rw - 72, y + 13, 2.2f, on ? glm::vec4(0.2f,0.3f,0.1f,1.0f) : glm::vec4(0.8f,0.8f,0.8f,1.0f));
        if (hover) state.menu_hover = id;
        return hover;
    }
    // A labeled slider row showing a value string. Sets menu_hover=id if hovered.
    bool widget_slider(GameState& state, float y, const char* label, float frac, const std::string& valstr, int id, float mx, float my) {
        float cx = screen_w * 0.5f, rw = 460, rx = cx - rw*0.5f;
        draw_rect(rx, y, rw, 40, {0.05f, 0.07f, 0.10f, 0.85f});
        draw_text(label, rx + 18, y + 13, 2.2f, {0.85f, 0.9f, 0.95f, 1.0f});
        float sx = rx + 180, sw = 200;
        draw_rect(sx, y + 18, sw, 5, {0.15f, 0.15f, 0.18f, 0.9f});
        float kx = sx + glm::clamp(frac, 0.0f, 1.0f) * sw;
        bool hover = (mx > sx-8 && mx < sx+sw+8 && my > y && my < y+40);
        glm::vec4 kc = hover ? glm::vec4(0.4f,0.9f,1.0f,1.0f) : glm::vec4(0.3f,0.6f,0.9f,0.95f);
        draw_rect(kx - 6, y + 11, 12, 19, kc);
        draw_text(valstr, rx + rw - 95, y + 13, 2.0f, {0.7f, 0.85f, 1.0f, 1.0f});
        if (hover) state.menu_hover = id;
        return hover;
    }
    // A "< BACK" footer button returning to the settings main page.
    void widget_back(GameState& state, float mx, float my) {
        float cx = screen_w * 0.5f, btn_y = screen_h - 80;
        bool hover = (mx > cx-110 && mx < cx+110 && my > btn_y && my < btn_y+50);
        draw_rect(cx - 110, btn_y, 220, 50, hover ? glm::vec4(0.15f,0.15f,0.25f,0.95f) : glm::vec4(0.08f,0.08f,0.15f,0.9f));
        draw_rect(cx - 110, btn_y, 220, 2, {0.5f, 0.5f, 0.9f, 0.7f});
        draw_text_centered("< BACK", cx, btn_y + 17, 3.0f, hover ? glm::vec4(1,1,1,1) : glm::vec4(0.6f,0.7f,0.9f,1.0f));
        if (hover) state.menu_hover = 998;  // back to settings main
    }
    void page_end() { glDisable(GL_BLEND); glBindVertexArray(0); }

    // ========================================================================
    // LOADING SCREEN — progress bar + stage label while the world is generated
    // on a background thread. Keeps the UI responsive instead of freezing.
    // ========================================================================
    void render_loading(float progress, const std::string& stage) {
        glUseProgram(shader);
        glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Dark background
        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.02f, 0.03f, 0.06f, 1.0f});

        float cx = screen_w * 0.5f;
        float cy = screen_h * 0.5f;

        draw_text_centered("LOADING BATTLEFIELD", cx, cy - 80, 4.0f, {0.5f, 0.9f, 0.6f, 1.0f});

        // Progress bar frame
        float bw = 600, bh = 36;
        float bx = cx - bw*0.5f, by = cy - 10;
        draw_rect(bx - 3, by - 3, bw + 6, bh + 6, {0.1f, 0.15f, 0.1f, 0.9f});
        draw_rect(bx, by, bw, bh, {0.04f, 0.06f, 0.04f, 1.0f});
        // Fill
        float fill = glm::clamp(progress, 0.0f, 1.0f) * bw;
        draw_rect(bx, by, fill, bh, {0.25f, 0.85f, 0.35f, 0.95f});
        draw_rect(bx, by, fill, 3, {0.5f, 1.0f, 0.6f, 1.0f}); // top highlight

        // Percentage + stage text
        char pct[16]; snprintf(pct, sizeof(pct), "%.0f%%", progress * 100.0f);
        draw_text_centered(pct, cx, by + 50, 2.5f, {0.7f, 0.9f, 0.7f, 1.0f});
        draw_text_centered(stage, cx, by + 85, 2.0f, {0.5f, 0.6f, 0.55f, 0.9f});

        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    // VIDEO: resolution, fullscreen, vsync
    void render_settings_video(GameState& state, float mx, float my) {
        float y = page_begin(state, "VIDEO", {0.4f, 0.6f, 1.0f});
        char buf[32];
        snprintf(buf, sizeof(buf), "%dX%d", state.settings_resolution_w, state.settings_resolution_h);
        widget_slider(state, y, "RESOLUTION", 0.5f, buf, 300, mx, my); y += 55;
        widget_toggle(state, y, "FULLSCREEN", state.settings_fullscreen, 301, mx, my); y += 55;
        widget_toggle(state, y, "VSYNC", state.settings_vsync, 302, mx, my); y += 55;
        widget_back(state, mx, my);
        page_end();
    }

    // GRAPHICS: bloom, exposure, fov, render distance, gamma
    void render_settings_graphics(GameState& state, float mx, float my) {
        float y = page_begin(state, "GRAPHICS", {0.3f, 0.9f, 0.3f});
        char buf[32];
        widget_toggle(state, y, "BLOOM", state.settings_bloom, 310, mx, my); y += 55;
        snprintf(buf, sizeof(buf), "%.2f", state.settings_exposure);
        widget_slider(state, y, "EXPOSURE", (state.settings_exposure-0.3f)/1.7f, buf, 311, mx, my); y += 55;
        snprintf(buf, sizeof(buf), "%.0f", state.settings_fov);
        widget_slider(state, y, "FOV", (state.settings_fov-50.0f)/50.0f, buf, 312, mx, my); y += 55;
        snprintf(buf, sizeof(buf), "%.0f", state.settings_render_distance);
        widget_slider(state, y, "RENDER DIST", (state.settings_render_distance-2000.0f)/6000.0f, buf, 313, mx, my); y += 55;
        snprintf(buf, sizeof(buf), "%.2f", state.settings_gamma);
        widget_slider(state, y, "GAMMA", (state.settings_gamma-0.5f)/1.5f, buf, 314, mx, my); y += 55;
        widget_back(state, mx, my);
        page_end();
    }

    // AUDIO: master + sfx volume
    void render_settings_audio(GameState& state, float mx, float my) {
        float y = page_begin(state, "AUDIO", {0.9f, 0.5f, 0.9f});
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f%%", state.settings_master_volume*100);
        widget_slider(state, y, "MASTER VOL", state.settings_master_volume, buf, 320, mx, my); y += 55;
        snprintf(buf, sizeof(buf), "%.0f%%", state.settings_sfx_volume*100);
        widget_slider(state, y, "SFX VOL", state.settings_sfx_volume, buf, 321, mx, my); y += 55;
        widget_back(state, mx, my);
        page_end();
    }

    // CONTROLS: camera speed, edge pan
    void render_settings_controls(GameState& state, float mx, float my) {
        float y = page_begin(state, "CONTROLS", {0.9f, 0.7f, 0.3f});
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f", state.settings_camera_speed);
        widget_slider(state, y, "CAM SPEED", (state.settings_camera_speed-200.0f)/800.0f, buf, 330, mx, my); y += 55;
        widget_toggle(state, y, "EDGE PAN", state.settings_edge_pan, 331, mx, my); y += 55;
        widget_back(state, mx, my);
        page_end();
    }

    void cleanup() {
        if (shader) glDeleteProgram(shader);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
    }

    // ========================================================================
    //  SURVIVAL MODE UI (decoupled from WaveDirector — caller passes values)
    // ========================================================================

    void begin_2d() {
        glUseProgram(shader);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    void end_2d() { glDisable(GL_BLEND); glBindVertexArray(0); }

    // Top-center survival status banner: WAVE n, phase, prep timer / enemies.
    void render_survival_banner(int wave, int phase, float prep_timer,
                                int enemies_left, int nests_alive, int tier) {
        begin_2d();
        float cx = screen_w * 0.5f;
        // Banner panel
        float bw = 420, bh = 56, bx = cx - bw*0.5f, by = 50;
        draw_rect(bx, by, bw, bh, {0.06f, 0.03f, 0.09f, 0.85f});
        draw_rect(bx, by, bw, 3, {0.9f, 0.25f, 0.6f, 0.95f});
        char buf[64];
        snprintf(buf, sizeof(buf), "WAVE %d", wave);
        draw_text(buf, bx + 16, by + 8, 3.0f, {1.0f, 0.5f, 0.75f, 1.0f});
        snprintf(buf, sizeof(buf), "TIER %d", tier);
        draw_text(buf, bx + bw - 110, by + 10, 1.8f, {0.8f, 0.7f, 0.4f, 0.9f});
        if (phase == 0) { // Prep
            snprintf(buf, sizeof(buf), "PREP  %0.0fs  (SPACE: READY)", prep_timer);
            draw_text(buf, bx + 16, by + 34, 1.6f, {0.5f, 0.9f, 0.6f, 0.95f});
        } else if (phase == 1) { // Combat
            snprintf(buf, sizeof(buf), "ENEMIES %d   NESTS %d", enemies_left, nests_alive);
            draw_text(buf, bx + 16, by + 34, 1.6f, {1.0f, 0.6f, 0.4f, 0.95f});
        } else {
            draw_text("WAVE CLEARED - CHOOSE A REWARD", bx + 16, by + 34, 1.5f,
                      {0.6f, 0.9f, 1.0f, 0.95f});
        }
        end_2d();
    }

    // 3-choose-1 upgrade draft. Returns the picked index (0..2) on click, else
    // -1. names[i]/descs[i] supply card text. Highlights the hovered card.
    int render_draft(float mx, float my, bool click,
                     const char* names[3], const char* descs[3]) {
        begin_2d();
        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.0f, 0.0f, 0.0f, 0.6f});
        float cx = screen_w * 0.5f;
        draw_text_centered("CHOOSE AN UPGRADE", cx, screen_h * 0.18f, 4.0f,
                           {1.0f, 0.6f, 0.85f, 1.0f});
        float cw = 320, ch = 300, gap = 40;
        float total = cw * 3 + gap * 2;
        float x0 = cx - total * 0.5f;
        float y0 = screen_h * 0.34f;
        int result = -1;
        for (int i = 0; i < 3; i++) {
            float x = x0 + i * (cw + gap);
            bool hover = (mx > x && mx < x + cw && my > y0 && my < y0 + ch);
            glm::vec4 bg = hover ? glm::vec4(0.18f, 0.10f, 0.20f, 0.98f)
                                 : glm::vec4(0.10f, 0.06f, 0.13f, 0.95f);
            draw_rect(x, y0, cw, ch, bg);
            glm::vec4 edge = hover ? glm::vec4(1.0f, 0.5f, 0.8f, 1.0f)
                                   : glm::vec4(0.5f, 0.3f, 0.5f, 0.8f);
            draw_rect(x, y0, cw, 4, edge);
            draw_rect(x, y0 + ch - 4, cw, 4, edge);
            draw_rect(x, y0, 4, ch, edge);
            draw_rect(x + cw - 4, y0, 4, ch, edge);
            draw_text_centered(names[i], x + cw*0.5f, y0 + 40, 2.4f,
                               hover ? glm::vec4(1,1,1,1) : glm::vec4(0.9f,0.8f,0.9f,1));
            // wrap desc roughly: print on two centered lines if long
            draw_text_centered(descs[i], x + cw*0.5f, y0 + ch*0.5f, 1.7f,
                               {0.7f, 0.85f, 0.9f, 0.95f});
            draw_text_centered(hover ? "> CLICK <" : "", x + cw*0.5f, y0 + ch - 40,
                               1.8f, {1.0f, 0.6f, 0.4f, 1.0f});
            if (hover && click) result = i;
        }
        end_2d();
        return result;
    }

    // Survival setup overlay: difficulty tier (gated by unlocked_tier), a
    // shareable seed, lifetime records, and START/BACK. Returns an action:
    //   0 = none, 1 = START run, 2 = BACK, 3 = tier-, 4 = tier+, 5 = reroll seed
    int render_survival_setup(GameState& state, float mx, float my, bool click,
                              int unlocked_tier, int best_wave, int best_tier,
                              int runs_played, int meta_points) {
        begin_2d();
        draw_rect(0, 0, (float)screen_w, (float)screen_h, {0.0f, 0.0f, 0.0f, 0.7f});
        float cx = screen_w * 0.5f;
        draw_text_centered("SURVIVAL", cx, screen_h * 0.12f, 5.0f, {1.0f, 0.45f, 0.7f, 1.0f});

        char buf[96];
        // Lifetime records line
        snprintf(buf, sizeof(buf), "BEST: WAVE %d  (TIER %d)   RUNS %d   MP %d",
                 best_wave, best_tier, runs_played, meta_points);
        draw_text_centered(buf, cx, screen_h * 0.22f, 1.7f, {0.7f, 0.8f, 0.9f, 0.9f});

        int action = 0;
        // --- difficulty tier row ---
        float ty = screen_h * 0.34f;
        draw_text_centered("DIFFICULTY TIER", cx, ty - 30, 2.0f, {0.9f, 0.8f, 0.4f, 1.0f});
        // minus
        if (button_box(cx - 180, ty, 60, 50, "-", mx, my, click)) action = 3;
        snprintf(buf, sizeof(buf), "%d", state.survival_tier);
        draw_text_centered(buf, cx, ty + 8, 3.5f, {1.0f, 1.0f, 1.0f, 1.0f});
        snprintf(buf, sizeof(buf), "/ %d", unlocked_tier);
        draw_text_centered(buf, cx + 70, ty + 16, 1.6f, {0.6f, 0.6f, 0.7f, 0.9f});
        // plus
        if (button_box(cx + 120, ty, 60, 50, "+", mx, my, click)) action = 4;

        // --- seed row ---
        float sy = screen_h * 0.50f;
        snprintf(buf, sizeof(buf), "SEED  %u", state.survival_seed);
        draw_text_centered(buf, cx, sy, 2.2f, {0.6f, 0.9f, 0.8f, 1.0f});
        if (button_box(cx - 90, sy + 36, 180, 44, "REROLL SEED", mx, my, click)) action = 5;

        // --- start / back ---
        float by = screen_h * 0.70f;
        if (button_box(cx - 220, by, 200, 64, "START RUN", mx, my, click)) action = 1;
        if (button_box(cx + 20, by, 200, 64, "BACK", mx, my, click)) action = 2;
        end_2d();
        return action;
    }

    // Small clickable box helper (hover highlight + centered label).
    bool button_box(float x, float y, float w, float h, const char* label,
                    float mx, float my, bool click) {
        bool hover = (mx > x && mx < x + w && my > y && my < y + h);
        glm::vec4 bg = hover ? glm::vec4(0.25f, 0.15f, 0.28f, 0.98f)
                             : glm::vec4(0.12f, 0.08f, 0.14f, 0.95f);
        draw_rect(x, y, w, h, bg);
        glm::vec4 edge = hover ? glm::vec4(1.0f, 0.6f, 0.85f, 1.0f)
                               : glm::vec4(0.5f, 0.35f, 0.5f, 0.8f);
        draw_rect(x, y, w, 3, edge);
        draw_rect(x, y + h - 3, w, 3, edge);
        float sc = (h > 50) ? 2.2f : 1.7f;
        draw_text_centered(label, x + w * 0.5f, y + h * 0.5f - sc * 4, sc,
                           hover ? glm::vec4(1, 1, 1, 1) : glm::vec4(0.85f, 0.8f, 0.85f, 1));
        return hover && click;
    }
};
