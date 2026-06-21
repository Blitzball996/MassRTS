#pragma once
#include <glad/glad.h>
#include <string>
#include <cstring>
#include <cctype>

// ============================================================================
// TextRenderer — minimal 5x7 bitmap font drawn as instanced quads.
//
// The menu system previously had NO text rendering (everything was colored
// rectangles), which is why menus looked invisible/unreadable. This provides
// a compact built-in font so menus can show real labels like a console game
// (Minecraft-console style: title + vertical labeled button list).
//
// Each glyph is a 5-wide x 7-tall bitmap packed as 7 bytes (one per row, low 5
// bits used). We render filled cells as small quads via the same simple shader
// the menu uses (u_screen + per-rect color), so no texture/atlas is needed.
// ============================================================================
class TextRenderer {
public:
    // Returns the 7-row bitmap for a character (uppercased). Unknown -> blank.
    static const unsigned char* glyph(char c) {
        c = (char)std::toupper((unsigned char)c);
        static const unsigned char blank[7] = {0,0,0,0,0,0,0};
        switch (c) {
            // FONT_PLACEHOLDER
            case 'A': { static const unsigned char g[7]={0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; return g; }
            case 'B': { static const unsigned char g[7]={0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; return g; }
            case 'C': { static const unsigned char g[7]={0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; return g; }
            case 'D': { static const unsigned char g[7]={0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; return g; }
            case 'E': { static const unsigned char g[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; return g; }
            case 'F': { static const unsigned char g[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; return g; }
            case 'G': { static const unsigned char g[7]={0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; return g; }
            case 'H': { static const unsigned char g[7]={0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; return g; }
            case 'I': { static const unsigned char g[7]={0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; return g; }
            case 'J': { static const unsigned char g[7]={0x07,0x02,0x02,0x02,0x02,0x12,0x0C}; return g; }
            case 'K': { static const unsigned char g[7]={0x11,0x12,0x14,0x18,0x14,0x12,0x11}; return g; }
            case 'L': { static const unsigned char g[7]={0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; return g; }
            case 'M': { static const unsigned char g[7]={0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; return g; }
            case 'N': { static const unsigned char g[7]={0x11,0x19,0x15,0x13,0x11,0x11,0x11}; return g; }
            case 'O': { static const unsigned char g[7]={0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; return g; }
            case 'P': { static const unsigned char g[7]={0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; return g; }
            case 'Q': { static const unsigned char g[7]={0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; return g; }
            case 'R': { static const unsigned char g[7]={0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; return g; }
            case 'S': { static const unsigned char g[7]={0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; return g; }
            case 'T': { static const unsigned char g[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; return g; }
            case 'U': { static const unsigned char g[7]={0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; return g; }
            case 'V': { static const unsigned char g[7]={0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; return g; }
            case 'W': { static const unsigned char g[7]={0x11,0x11,0x11,0x15,0x15,0x1B,0x11}; return g; }
            case 'X': { static const unsigned char g[7]={0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; return g; }
            case 'Y': { static const unsigned char g[7]={0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; return g; }
            case 'Z': { static const unsigned char g[7]={0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; return g; }
            case '0': { static const unsigned char g[7]={0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; return g; }
            case '1': { static const unsigned char g[7]={0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; return g; }
            case '2': { static const unsigned char g[7]={0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}; return g; }
            case '3': { static const unsigned char g[7]={0x1F,0x01,0x02,0x06,0x01,0x11,0x0E}; return g; }
            case '4': { static const unsigned char g[7]={0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; return g; }
            case '5': { static const unsigned char g[7]={0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}; return g; }
            case '6': { static const unsigned char g[7]={0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; return g; }
            case '7': { static const unsigned char g[7]={0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; return g; }
            case '8': { static const unsigned char g[7]={0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; return g; }
            case '9': { static const unsigned char g[7]={0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; return g; }
            case '.': { static const unsigned char g[7]={0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}; return g; }
            case ',': { static const unsigned char g[7]={0x00,0x00,0x00,0x00,0x0C,0x04,0x08}; return g; }
            case ':': { static const unsigned char g[7]={0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}; return g; }
            case '-': { static const unsigned char g[7]={0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; return g; }
            case '+': { static const unsigned char g[7]={0x00,0x04,0x04,0x1F,0x04,0x04,0x00}; return g; }
            case '/': { static const unsigned char g[7]={0x01,0x02,0x02,0x04,0x08,0x08,0x10}; return g; }
            case '!': { static const unsigned char g[7]={0x04,0x04,0x04,0x04,0x04,0x00,0x04}; return g; }
            case '?': { static const unsigned char g[7]={0x0E,0x11,0x01,0x06,0x04,0x00,0x04}; return g; }
            case '(': { static const unsigned char g[7]={0x02,0x04,0x08,0x08,0x08,0x04,0x02}; return g; }
            case ')': { static const unsigned char g[7]={0x08,0x04,0x02,0x02,0x02,0x04,0x08}; return g; }
            case '%': { static const unsigned char g[7]={0x19,0x1A,0x02,0x04,0x08,0x0B,0x13}; return g; }
            case '<': { static const unsigned char g[7]={0x02,0x04,0x08,0x10,0x08,0x04,0x02}; return g; }
            case '>': { static const unsigned char g[7]={0x08,0x04,0x02,0x01,0x02,0x04,0x08}; return g; }
            default: return blank;
        }
    }

    int char_w = 5, char_h = 7;

    // Measure pixel width of a string at the given scale (incl. 1px spacing).
    float measure(const std::string& s, float scale) const {
        return s.size() * (char_w + 1) * scale;
    }

    // Draw text using a caller-provided cell drawer:
    //   draw_cell(x, y, w, h) fills one pixel cell. This keeps TextRenderer
    //   decoupled from the menu's GL state (it reuses draw_rect).
    template <typename DrawCell>
    void draw(const std::string& s, float x, float y, float scale, DrawCell&& draw_cell) const {
        float cx = x;
        for (char ch : s) {
            if (ch == ' ') { cx += (char_w + 1) * scale; continue; }
            const unsigned char* g = glyph(ch);
            for (int row = 0; row < char_h; row++) {
                unsigned char bits = g[row];
                for (int col = 0; col < char_w; col++) {
                    if (bits & (1 << (char_w - 1 - col))) {
                        draw_cell(cx + col * scale, y + row * scale, scale, scale);
                    }
                }
            }
            cx += (char_w + 1) * scale;
        }
    }
};
