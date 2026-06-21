#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <cstdio>

class UI {
public:
    GLuint shader = 0;
    GLuint vao = 0, vbo = 0;
    int screen_w = 1600, screen_h = 900;

    int selected_count = 0;
    int selected_infantry = 0;
    int selected_cavalry = 0;
    int selected_archer = 0;
    int red_alive = 0, blue_alive = 0;
    int total_units = 0;
    int fps = 0;
    int score = 0;
    int nuke_cost = 500;
    int money = 0;
    int buy_count = 1000;
    bool nuke_ready = false;
    // Survival mode: hide the skirmish-only score/nuke box (the survival banner
    // owns the top-center) to avoid the HUD overlap.
    bool survival_mode = false;
    // Sculpt HUD state (set each frame from main loop)
    bool sculpt_mode = false;
    int  sculpt_brush = 1;   // 0=Raise 1=Dig 2=Smooth 3=Flatten
    int  sculpt_radius = 60;
    float sculpt_strength = 0.35f; // 0.1..1.0 brush push strength

    bool init(int w, int h) {
        screen_w = w; screen_h = h;
        const char* vs_src = R"(
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
        const char* fs_src = R"(
#version 330 core
uniform vec4 u_color;
out vec4 frag;
void main() { frag = u_color; }
)";
        GLuint vs = compile(vs_src, GL_VERTEX_SHADER);
        GLuint fs = compile(fs_src, GL_FRAGMENT_SHADER);
        if (!vs || !fs) return false;
        shader = glCreateProgram();
        glAttachShader(shader, vs); glAttachShader(shader, fs);
        glLinkProgram(shader);
        glDeleteShader(vs); glDeleteShader(fs);

        float quad[] = {0,0, 1,0, 0,1, 1,1};
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        return true;
    }

    void render() {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(shader);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);

        // Top bar
        draw_rect(0, 0, (float)screen_w, 44, {0.04f, 0.04f, 0.08f, 0.88f});

        // Red army bar
        float red_ratio = total_units > 0 ? glm::clamp((float)red_alive / (total_units * 0.5f), 0.0f, 1.0f) : 0;
        draw_rect(10, 8, 150, 28, {0.2f, 0.08f, 0.08f, 0.6f});
        draw_rect(10, 8, 150 * red_ratio, 28, {0.85f, 0.15f, 0.15f, 0.9f});
        draw_number(170, 12, red_alive, {1.0f, 0.3f, 0.3f, 1.0f}, 1.2f);

        // Blue army bar
        float blue_ratio = total_units > 0 ? glm::clamp((float)blue_alive / (total_units * 0.5f), 0.0f, 1.0f) : 0;
        draw_rect((float)screen_w - 160, 8, 150, 28, {0.08f, 0.08f, 0.2f, 0.6f});
        draw_rect((float)screen_w - 160, 8, 150 * blue_ratio, 28, {0.15f, 0.3f, 0.85f, 0.9f});
        draw_number((float)screen_w - 230, 12, blue_alive, {0.4f, 0.5f, 1.0f, 1.0f}, 1.2f);

        // FPS center
        draw_number((float)screen_w * 0.5f - 15, 12, fps, {0.7f, 0.7f, 0.7f, 0.7f}, 1.0f);

        // === SCORE & NUKE INDICATOR ===
        float score_x = (float)screen_w * 0.5f - 80;
        float score_y = 50;
        if (!survival_mode) {
        draw_rect(score_x, score_y, 160, 30, {0.08f, 0.05f, 0.02f, 0.8f});
        // Score progress bar toward nuke
        float nuke_progress = glm::clamp((float)score / (float)nuke_cost, 0.0f, 1.0f);
        draw_rect(score_x + 4, score_y + 4, 152 * nuke_progress, 22,
                  nuke_ready ? glm::vec4(1.0f, 0.3f, 0.0f, 0.95f) : glm::vec4(0.6f, 0.5f, 0.1f, 0.8f));
        draw_number(score_x + 60, score_y + 6, score, {1.0f, 0.9f, 0.3f, 1.0f}, 1.0f);

        // Nuke ready flash
        if (nuke_ready) {
            draw_rect(score_x - 5, score_y - 5, 170, 40, {1.0f, 0.2f, 0.0f, 0.3f});
            // "N" indicator
            draw_rect(score_x + 140, score_y + 5, 3, 20, {1.0f, 0.4f, 0.0f, 1.0f});
            draw_rect(score_x + 152, score_y + 5, 3, 20, {1.0f, 0.4f, 0.0f, 1.0f});
            draw_rect(score_x + 143, score_y + 5, 9, 3, {1.0f, 0.4f, 0.0f, 1.0f});
        }
        } // end !survival_mode (score/nuke box)
        // Selection panel
        if (selected_count > 0) {
            float pw = 220, ph = 55;
            float px = 10, py = (float)screen_h - ph - 10;
            draw_rect(px, py, pw, ph, {0.06f, 0.18f, 0.06f, 0.85f});
            draw_rect(px, py, pw, 2, {0.2f, 0.6f, 0.2f, 0.9f});
            draw_number(px + 10, py + 8, selected_count, {0.3f, 1.0f, 0.3f, 1.0f}, 1.4f);

            float ty = py + 32;
            if (selected_infantry > 0) {
                draw_rect(px + 10, ty, 8, 3, {0.65f, 0.68f, 0.7f, 1.0f});
                draw_number(px + 22, ty - 2, selected_infantry, {0.8f, 0.8f, 0.8f, 1.0f}, 0.8f);
            }
            if (selected_cavalry > 0) {
                draw_rect(px + 75, ty, 10, 5, {0.9f, 0.88f, 0.82f, 1.0f});
                draw_number(px + 90, ty - 2, selected_cavalry, {0.8f, 0.8f, 0.8f, 1.0f}, 0.8f);
            }
            if (selected_archer > 0) {
                draw_rect(px + 140, ty, 2, 8, {0.45f, 0.28f, 0.1f, 1.0f});
                draw_number(px + 150, ty - 2, selected_archer, {0.8f, 0.8f, 0.8f, 1.0f}, 0.8f);
            }
        }

        // === MONEY & SHOP PANEL ===
        float shop_x = (float)screen_w * 0.5f - 100;
        float shop_y = 90;
        draw_rect(shop_x, shop_y, 200, 32, {0.02f, 0.12f, 0.02f, 0.85f});
        draw_rect(shop_x, shop_y, 200, 2, {0.3f, 0.9f, 0.3f, 0.7f});
        // $ icon (3 rects)
        draw_rect(shop_x + 8, shop_y + 6, 2, 20, {0.3f, 0.95f, 0.3f, 1.0f});
        draw_rect(shop_x + 4, shop_y + 10, 10, 3, {0.3f, 0.95f, 0.3f, 1.0f});
        draw_rect(shop_x + 4, shop_y + 18, 10, 3, {0.3f, 0.95f, 0.3f, 1.0f});
        draw_number(shop_x + 20, shop_y + 7, money, {0.2f, 1.0f, 0.2f, 1.0f}, 1.3f);
        // Buy count indicator on right side, with clickable [-]/[+] buttons.
        // Hit-test rects (must match main_gpu.cpp):
        //   minus: (shop_x+130, shop_y+4, 20, 24)
        //   value: (shop_x+152, shop_y+4, 40, 24)
        //   plus:  (shop_x+196, shop_y+4, 20, 24)
        draw_rect(shop_x + 130, shop_y + 4, 20, 24, {0.20f, 0.08f, 0.04f, 0.85f}); // [-]
        draw_rect(shop_x + 137, shop_y + 14, 6, 3, {1.0f, 0.8f, 0.4f, 1.0f});       // minus glyph
        draw_rect(shop_x + 152, shop_y + 4, 40, 24, {0.1f, 0.06f, 0.02f, 0.7f});    // value bg
        draw_number(shop_x + 156, shop_y + 8, buy_count, {1.0f, 0.85f, 0.4f, 1.0f}, 0.85f);
        draw_rect(shop_x + 196, shop_y + 4, 20, 24, {0.04f, 0.20f, 0.06f, 0.85f}); // [+]
        draw_rect(shop_x + 203, shop_y + 14, 6, 3, {0.4f, 1.0f, 0.5f, 1.0f});       // plus glyph h
        draw_rect(shop_x + 205, shop_y + 9, 2, 13, {0.4f, 1.0f, 0.5f, 1.0f});       // plus glyph v

        // Minimap bg
        float mm = 180, mmx = screen_w - mm - 10, mmy = screen_h - mm - 10;
        draw_rect(mmx, mmy, mm, mm, {0.02f, 0.05f, 0.02f, 0.85f});
        draw_rect(mmx, mmy, mm, 2, {0.3f, 0.6f, 0.3f, 0.7f});
        draw_rect(mmx, mmy+mm-2, mm, 2, {0.3f, 0.6f, 0.3f, 0.7f});
        draw_rect(mmx, mmy, 2, mm, {0.3f, 0.6f, 0.3f, 0.7f});
        draw_rect(mmx+mm-2, mmy, 2, mm, {0.3f, 0.6f, 0.3f, 0.7f});

        // === SCULPT TOGGLE BUTTON (always visible, top-right under blue bar) ===
        // Hit-test rect in main_gpu.cpp: (screen_w-110, 50, 100, 26)
        {
            float tx = (float)screen_w - 110, ty2 = 50;
            glm::vec4 tcol = sculpt_mode ? glm::vec4(0.45f,0.30f,0.85f,0.95f)
                                         : glm::vec4(0.12f,0.10f,0.18f,0.9f);
            draw_rect(tx, ty2, 100, 26, tcol);
            draw_rect(tx, ty2, 100, 2, {0.6f, 0.4f, 1.0f, 0.9f});
            // "B" glyph (left)
            draw_rect(tx + 8,  ty2 + 6, 2, 14, {0.9f,0.9f,1.0f,1.0f});
            draw_rect(tx + 10, ty2 + 6, 7, 2,  {0.9f,0.9f,1.0f,1.0f});
            draw_rect(tx + 10, ty2 + 12,7, 2,  {0.9f,0.9f,1.0f,1.0f});
            draw_rect(tx + 10, ty2 + 18,7, 2,  {0.9f,0.9f,1.0f,1.0f});
            draw_rect(tx + 16, ty2 + 7, 2, 5,  {0.9f,0.9f,1.0f,1.0f});
            draw_rect(tx + 16, ty2 + 14,2, 5,  {0.9f,0.9f,1.0f,1.0f});
            // sculpt indicator dot (lit when active)
            draw_rect(tx + 78, ty2 + 9, 12, 8,
                      sculpt_mode ? glm::vec4(0.4f,1.0f,0.5f,1.0f) : glm::vec4(0.3f,0.3f,0.35f,0.9f));
        }

        // === SCULPT MODE INDICATOR (bottom-center) ===
        if (sculpt_mode) {
            float bx = (float)screen_w * 0.5f - 150;
            float by = (float)screen_h - 60;
            draw_rect(bx, by - 22, 300, 68, {0.05f, 0.03f, 0.10f, 0.9f});
            draw_rect(bx, by - 22, 300, 2, {0.6f, 0.4f, 1.0f, 0.9f});
            // brush swatch: 1 Raise-soil=green, 2 Dig-bowl=red, 3 Smooth=cyan, 4 Cave=yellow
            glm::vec4 bc =
                sculpt_brush==0 ? glm::vec4(0.3f,1.0f,0.3f,1.0f) :
                sculpt_brush==1 ? glm::vec4(1.0f,0.3f,0.2f,1.0f) :
                sculpt_brush==2 ? glm::vec4(0.3f,0.8f,1.0f,1.0f) :
                                  glm::vec4(1.0f,0.9f,0.3f,1.0f);
            draw_rect(bx + 10, by + 12, 22, 22, bc);
            // four brush slots (1-4), highlight active
            for (int i = 0; i < 4; i++) {
                glm::vec4 col = (i==sculpt_brush) ? bc : glm::vec4(0.25f,0.25f,0.3f,0.8f);
                draw_rect(bx + 44 + i*30, by + 14, 24, 18, col);
                draw_number(bx + 52 + i*30, by + 16, i+1, {0.9f,0.9f,0.9f,1.0f}, 0.7f);
            }
            // radius [-]/[+] buttons + readout
            // Hit-test rects (main_gpu.cpp):
            //   minus: (bx+170, by+12, 22, 22)  plus: (bx+258, by+12, 22, 22)
            draw_rect(bx + 170, by + 12, 22, 22, {0.25f,0.15f,0.30f,0.9f}); // [-]
            draw_rect(bx + 175, by + 22, 12, 3, {0.9f,0.8f,1.0f,1.0f});
            draw_number(bx + 200, by + 14, sculpt_radius, {0.8f,0.7f,1.0f,1.0f}, 1.0f);
            draw_rect(bx + 258, by + 12, 22, 22, {0.15f,0.30f,0.18f,0.9f}); // [+]
            draw_rect(bx + 263, by + 22, 12, 3, {0.8f,1.0f,0.85f,1.0f});
            draw_rect(bx + 268, by + 17, 3, 13, {0.8f,1.0f,0.85f,1.0f});
            // Strength bar (scroll wheel in sculpt mode). Track + filled portion
            // + numeric readout (0..100). Sits in the row added above the panel.
            float sby = by - 16;
            draw_rect(bx + 10, sby, 200, 10, {0.18f,0.16f,0.24f,0.9f});       // track
            float sfill = 200.0f * ((sculpt_strength - 0.1f) / 0.9f);
            draw_rect(bx + 10, sby, sfill, 10, {0.55f,0.85f,1.0f,1.0f});       // fill
            draw_number(bx + 224, sby - 2, (int)(sculpt_strength * 100.0f),
                        {0.7f,0.9f,1.0f,1.0f}, 1.0f);
        }

        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }

    void render_minimap_dots(const glm::vec2* positions, const glm::vec3* colors,
                             const char* alive, uint32_t count, float map_range) {
        glDisable(GL_DEPTH_TEST);
        glUseProgram(shader);
        glUniform2f(glGetUniformLocation(shader, "u_screen"), (float)screen_w, (float)screen_h);
        glBindVertexArray(vao);

        float mm = 180, mmx = screen_w - mm - 10, mmy = screen_h - mm - 10;
        uint32_t step = count > 2000 ? count / 2000 : 1;
        for (uint32_t i = 0; i < count; i += step) {
            if (!alive[i]) continue;
            float nx = (positions[i].x / map_range + 0.5f) * mm;
            float ny = (positions[i].y / map_range + 0.5f) * mm;
            if (nx < 0 || nx > mm || ny < 0 || ny > mm) continue;
            draw_rect(mmx + nx - 1, mmy + ny - 1, 2, 2, {colors[i].r, colors[i].g, colors[i].b, 0.9f});
        }
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }

    void cleanup() {
        glDeleteProgram(shader);
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vbo);
    }

private:
    void draw_rect(float x, float y, float w, float h, glm::vec4 color) {
        glUniform4f(glGetUniformLocation(shader, "u_rect"), x, y, w, h);
        glUniform4f(glGetUniformLocation(shader, "u_color"), color.r, color.g, color.b, color.a);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void draw_number(float x, float y, int value, glm::vec4 color, float scale) {
        if (value < 0) value = 0;
        char buf[12]; snprintf(buf, sizeof(buf), "%d", value);
        float cx = x, dw = 10 * scale;
        for (int i = 0; buf[i]; i++) {
            if (buf[i] >= '0' && buf[i] <= '9')
                draw_digit(cx, y, buf[i] - '0', color, scale);
            cx += dw + 2 * scale;
        }
    }

    void draw_digit(float x, float y, int d, glm::vec4 color, float scale) {
        float sw = 8*scale, sh = 2*scale, sv = 8*scale, st = 2*scale;
        static const uint8_t segs[10] = {
            0b1110111, 0b0010010, 0b1011101, 0b1011011, 0b0111010,
            0b1101011, 0b1101111, 0b1010010, 0b1111111, 0b1111011
        };
        uint8_t s = segs[d];
        if (s & 0b1000000) draw_rect(x+st, y, sw-st*2, sh, color);
        if (s & 0b0100000) draw_rect(x, y+sh, st, sv, color);
        if (s & 0b0010000) draw_rect(x+sw-st, y+sh, st, sv, color);
        if (s & 0b0001000) draw_rect(x+st, y+sv+sh, sw-st*2, sh, color);
        if (s & 0b0000100) draw_rect(x, y+sv+sh*2, st, sv, color);
        if (s & 0b0000010) draw_rect(x+sw-st, y+sv+sh*2, st, sv, color);
        if (s & 0b0000001) draw_rect(x+st, y+sv*2+sh*2, sw-st*2, sh, color);
    }

    GLuint compile(const char* src, GLenum type) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[256]; glGetShaderInfoLog(s, 256, 0, log); fprintf(stderr, "UI: %s\n", log); return 0; }
        return s;
    }
};
