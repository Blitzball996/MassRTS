#pragma once
#include <cstdio>
static int g_debug_frame_count = 0;
inline void debug_frame_check(int phase) {
    if (g_debug_frame_count < 3) {
        GLenum err = glGetError();
        // Read center pixel
        unsigned char pixel[4] = {0,0,0,0};
        glReadPixels(800, 450, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        fprintf(stderr, "Frame %d: phase=%d GL=%u pixel=(%u,%u,%u,%u)\n",
                g_debug_frame_count, phase, err, pixel[0], pixel[1], pixel[2], pixel[3]);
        g_debug_frame_count++;
    }
}
