#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <iostream>

// ============================================================================
// PostFX — HDR + Bloom + ACES tonemapping post-processing stack.
//
// Scene is rendered into a floating-point HDR framebuffer (RGBA16F) so emissive
// surfaces can exceed 1.0. After the scene is drawn we:
//   1. Bright-pass prefilter (extract pixels above threshold)
//   2. Separable Gaussian blur across a few half-res mips
//   3. Composite scene + bloom, apply exposure + ACES tonemap + gamma
// Without this, anything "glowing" just clips flat to white. This is the
// prerequisite for fire cores, projectile trails and the particle overhaul.
// ============================================================================
class PostFX {
public:
    bool enabled = true;
    float exposure = 1.1f;
    float bloom_threshold = 1.0f;
    float bloom_knee = 0.6f;
    float bloom_strength = 0.65f;

    static constexpr int BLOOM_MIPS = 5;

    bool init(const std::string& shader_dir, int w, int h) {
        width = w; height = h;
        prefilter_prog = load(shader_dir + "fullscreen.vert", shader_dir + "bloom_prefilter.frag");
        blur_prog      = load(shader_dir + "fullscreen.vert", shader_dir + "bloom_blur.frag");
        composite_prog = load(shader_dir + "fullscreen.vert", shader_dir + "post_composite.frag");
        if (!prefilter_prog || !blur_prog || !composite_prog) {
            std::cerr << "PostFX: shader load failed, disabling.\n";
            enabled = false;
            return false;
        }
        glGenVertexArrays(1, &empty_vao); // fullscreen triangle needs a bound VAO
        build_targets(w, h);
        return true;
    }

    void resize(int w, int h) {
        if (w == width && h == height) return;
        width = w; height = h;
        destroy_targets();
        build_targets(w, h);
    }

    // PLACEHOLDER_METHODS

    // Bind the HDR framebuffer; render the whole scene into it.
    void begin_scene() {
        if (!enabled) { glBindFramebuffer(GL_FRAMEBUFFER, 0); return; }
        glBindFramebuffer(GL_FRAMEBUFFER, hdr_fbo);
        glViewport(0, 0, width, height);
    }

    // Access the scene depth texture (for soft particles)
    GLuint get_depth_texture() const { return hdr_depth; }

    // Resolve HDR -> bloom -> tonemap into the default framebuffer (screen).
    void resolve() {
        if (!enabled) return;
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glBindVertexArray(empty_vao);

        // --- 1. Bright-pass prefilter into mip 0 ---
        glUseProgram(prefilter_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdr_color);
        glUniform1i(glGetUniformLocation(prefilter_prog, "u_scene"), 0);
        glUniform1f(glGetUniformLocation(prefilter_prog, "u_threshold"), bloom_threshold);
        glUniform1f(glGetUniformLocation(prefilter_prog, "u_knee"), bloom_knee);
        glBindFramebuffer(GL_FRAMEBUFFER, mip_fbo[0]);
        glViewport(0, 0, mip_w[0], mip_h[0]);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // --- 2. Downsample + blur through the mip chain ---
        for (int i = 0; i < BLOOM_MIPS; i++) {
            blur_level(i);
            if (i + 1 < BLOOM_MIPS) downsample(i, i + 1);
        }
        // Upsample-accumulate back to mip 0 (additive)
        for (int i = BLOOM_MIPS - 1; i > 0; i--) upsample_add(i, i - 1);

        // --- 3. Composite to screen with ACES tonemap ---
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        glUseProgram(composite_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hdr_color);
        glUniform1i(glGetUniformLocation(composite_prog, "u_scene"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mip_tex[0]);
        glUniform1i(glGetUniformLocation(composite_prog, "u_bloom"), 1);
        glUniform1f(glGetUniformLocation(composite_prog, "u_exposure"), exposure);
        glUniform1f(glGetUniformLocation(composite_prog, "u_bloom_strength"), bloom_strength);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }


private:
    int width = 0, height = 0;
    GLuint empty_vao = 0;
    GLuint prefilter_prog = 0, blur_prog = 0, composite_prog = 0;

    // HDR scene target
    GLuint hdr_fbo = 0, hdr_color = 0, hdr_depth = 0;
    // Bloom mip chain (ping-pong handled per level)
    GLuint mip_fbo[BLOOM_MIPS] = {0};
    GLuint mip_tex[BLOOM_MIPS] = {0};
    GLuint mip_tmp[BLOOM_MIPS] = {0};
    int mip_w[BLOOM_MIPS] = {0};
    int mip_h[BLOOM_MIPS] = {0};

    // BUILD_PLACEHOLDER

    void draw_quad() { glDrawArrays(GL_TRIANGLES, 0, 3); }

    // Blur mip[i] in place: horizontal into tmp, vertical back into tex.
    void blur_level(int i) {
        glUseProgram(blur_prog);
        glUniform2f(glGetUniformLocation(blur_prog, "u_texel"), 1.0f / mip_w[i], 1.0f / mip_h[i]);
        glViewport(0, 0, mip_w[i], mip_h[i]);
        // horizontal: tex -> tmp
        glBindFramebuffer(GL_FRAMEBUFFER, mip_fbo[i]); // reuse fbo, swap attachment
        attach(mip_fbo[i], mip_tmp[i]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mip_tex[i]);
        glUniform1i(glGetUniformLocation(blur_prog, "u_tex"), 0);
        glUniform2f(glGetUniformLocation(blur_prog, "u_direction"), 1.0f, 0.0f);
        draw_quad();
        // vertical: tmp -> tex
        attach(mip_fbo[i], mip_tex[i]);
        glBindTexture(GL_TEXTURE_2D, mip_tmp[i]);
        glUniform2f(glGetUniformLocation(blur_prog, "u_direction"), 0.0f, 1.0f);
        draw_quad();
    }

    // Copy/blit src mip into dst mip (smaller) via the blur shader at zero radius
    // — simple bilinear downsample is fine here.
    void downsample(int src, int dst) {
        glUseProgram(blur_prog);
        glViewport(0, 0, mip_w[dst], mip_h[dst]);
        attach(mip_fbo[dst], mip_tex[dst]);
        glUniform2f(glGetUniformLocation(blur_prog, "u_texel"), 0.0f, 0.0f);
        glUniform2f(glGetUniformLocation(blur_prog, "u_direction"), 0.0f, 0.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mip_tex[src]);
        glUniform1i(glGetUniformLocation(blur_prog, "u_tex"), 0);
        draw_quad();
    }

    // Additively blend src mip up into dst mip.
    void upsample_add(int src, int dst) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(blur_prog);
        glViewport(0, 0, mip_w[dst], mip_h[dst]);
        attach(mip_fbo[dst], mip_tex[dst]);
        glUniform2f(glGetUniformLocation(blur_prog, "u_texel"), 0.0f, 0.0f);
        glUniform2f(glGetUniformLocation(blur_prog, "u_direction"), 0.0f, 0.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mip_tex[src]);
        glUniform1i(glGetUniformLocation(blur_prog, "u_tex"), 0);
        draw_quad();
        glDisable(GL_BLEND);
    }

    void attach(GLuint fbo, GLuint tex) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }

    GLuint make_color_tex(int w, int h) {
        GLuint t; glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return t;
    }

    void build_targets(int w, int h) {
        // HDR scene FBO (color RGBA16F + depth texture for soft particles)
        hdr_color = make_color_tex(w, h);
        
        // Depth as a texture (not renderbuffer) so particles can sample it
        glGenTextures(1, &hdr_depth);
        glBindTexture(GL_TEXTURE_2D, hdr_depth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        glGenFramebuffers(1, &hdr_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, hdr_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdr_color, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, hdr_depth, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            std::cerr << "PostFX: HDR FBO incomplete\n";

        // Bloom mip chain — start at half res, halve each level.
        int mw = w / 2, mh = h / 2;
        for (int i = 0; i < BLOOM_MIPS; i++) {
            mip_w[i] = mw < 1 ? 1 : mw;
            mip_h[i] = mh < 1 ? 1 : mh;
            mip_tex[i] = make_color_tex(mip_w[i], mip_h[i]);
            mip_tmp[i] = make_color_tex(mip_w[i], mip_h[i]);
            glGenFramebuffers(1, &mip_fbo[i]);
            attach(mip_fbo[i], mip_tex[i]);
            mw /= 2; mh /= 2;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void destroy_targets() {
        if (hdr_color) glDeleteTextures(1, &hdr_color);
        if (hdr_depth) glDeleteTextures(1, &hdr_depth); // now a texture
        if (hdr_fbo) glDeleteFramebuffers(1, &hdr_fbo);
        for (int i = 0; i < BLOOM_MIPS; i++) {
            if (mip_tex[i]) glDeleteTextures(1, &mip_tex[i]);
            if (mip_tmp[i]) glDeleteTextures(1, &mip_tmp[i]);
            if (mip_fbo[i]) glDeleteFramebuffers(1, &mip_fbo[i]);
        }
        hdr_color = hdr_depth = hdr_fbo = 0;
    }

    GLuint compile(const std::string& src, GLenum type) {
        GLuint s = glCreateShader(type);
        const char* c = src.c_str();
        glShaderSource(s, 1, &c, nullptr);
        glCompileShader(s);
        int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, 0, log); std::cerr << "PostFX shader: " << log << "\n"; return 0; }
        return s;
    }

    GLuint load(const std::string& vpath, const std::string& fpath) {
        auto read = [](const std::string& p) -> std::string {
            FILE* f = fopen(p.c_str(), "rb");
            if (!f) { std::cerr << "PostFX: cannot open " << p << "\n"; return ""; }
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            std::string s(n, '\0'); fread(&s[0], 1, n, f); fclose(f); return s;
        };
        std::string vs = read(vpath), fs = read(fpath);
        if (vs.empty() || fs.empty()) return 0;
        GLuint v = compile(vs, GL_VERTEX_SHADER), f = compile(fs, GL_FRAGMENT_SHADER);
        if (!v || !f) return 0;
        GLuint p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        int ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { char log[512]; glGetProgramInfoLog(p, 512, 0, log); std::cerr << "PostFX link: " << log << "\n"; return 0; }
        glDeleteShader(v); glDeleteShader(f);
        return p;
    }


};
