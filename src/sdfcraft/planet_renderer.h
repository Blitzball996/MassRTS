#pragma once
// =============================================================================
// SDFCraft - Planet renderer (Phase P1 visualisation)
// -----------------------------------------------------------------------------
// Self-contained GL renderer for the cube-sphere LOD planet. Inline shaders
// (no external files) keep it independent. Renders the floating-origin mesh
// produced by PlanetMesh, with simple sun lighting + height tint so you can
// fly around a real-scale (6371 km) planet and watch LOD refine under you.
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "planet_mesh.h"
#include <string>
#include <vector>
#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#include "../stb_image.h"
#include <cstddef>

namespace sdfcraft {

class PlanetRenderer {
public:
    bool init() {
        prog_ = build_program();
        if (!prog_) return false;
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        
        // Load terrain textures (3DWorld assets)
        tex_sand_  = load_texture("assets/textures/terrain/desert_sand.jpg");
        tex_grass_ = load_texture("assets/textures/terrain/grass.png");
        tex_rock_  = load_texture("assets/textures/terrain/rock.png");
        tex_snow_  = load_texture("assets/textures/terrain/snow2.jpg");
        
        return true;
    }
    void shutdown() {
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (prog_) glDeleteProgram(prog_);
        if (tex_sand_)  glDeleteTextures(1, &tex_sand_);
        if (tex_grass_) glDeleteTextures(1, &tex_grass_);
        if (tex_rock_)  glDeleteTextures(1, &tex_rock_);
        if (tex_snow_)  glDeleteTextures(1, &tex_snow_);
    }

    // Upload a freshly built mesh (camera-relative vertices).
    void upload(const std::vector<PlanetVertex>& verts) {
        count_ = (GLsizei)verts.size();
        if (count_ == 0) return;
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(PlanetVertex),
                     verts.data(), GL_DYNAMIC_DRAW);
        // pos(3) normal(3) height(1)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(PlanetVertex),(void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(PlanetVertex),(void*)offsetof(PlanetVertex,normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,1,GL_FLOAT,GL_FALSE,sizeof(PlanetVertex),(void*)offsetof(PlanetVertex,height));
        glBindVertexArray(0);
    }

    // PLACEHOLDER_RENDER
    void render(const glm::mat4& view, const glm::mat4& proj,
                glm::vec3 sun_dir, glm::vec3 cam_up) {
        if (count_ == 0) return;
        glUseProgram(prog_);
        glUniformMatrix4fv(glGetUniformLocation(prog_,"u_view"),1,GL_FALSE,&view[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(prog_,"u_proj"),1,GL_FALSE,&proj[0][0]);
        glUniform3fv(glGetUniformLocation(prog_,"u_sun"),1,&sun_dir[0]);
        
        // Bind terrain textures
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_sand_);
        glUniform1i(glGetUniformLocation(prog_,"u_sand"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tex_grass_);
        glUniform1i(glGetUniformLocation(prog_,"u_grass"), 1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, tex_rock_);
        glUniform1i(glGetUniformLocation(prog_,"u_rock"), 2);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, tex_snow_);
        glUniform1i(glGetUniformLocation(prog_,"u_snow"), 3);
        
        glEnable(GL_DEPTH_TEST);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, count_);
        glBindVertexArray(0);
    }

private:
    GLuint prog_=0, vao_=0, vbo_=0; GLsizei count_=0;
    GLuint tex_sand_=0, tex_grass_=0, tex_rock_=0, tex_snow_=0;

    // Load a texture from disk (JPEG/PNG) with stb_image
    static GLuint load_texture(const char* path) {
        int w, h, ch;
        stbi_set_flip_vertically_on_load(0); // 3DWorld textures not flipped
        unsigned char* data = stbi_load(path, &w, &h, &ch, 0);
        if (!data) {
            fprintf(stderr, "Failed to load texture: %s\n", path);
            return 0;
        }
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        GLenum fmt = (ch == 3) ? GL_RGB : GL_RGBA;
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        stbi_image_free(data);
        return tex;
    }

    // PLACEHOLDER_SHADER
    static GLuint compile(GLenum t, const char* src) {
        GLuint s = glCreateShader(t);
        glShaderSource(s,1,&src,nullptr); glCompileShader(s);
        GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
        if(!ok){ char log[1024]; glGetShaderInfoLog(s,1024,nullptr,log);
                 fprintf(stderr,"planet shader: %s\n",log); }
        return s;
    }
    static GLuint build_program() {
        const char* vs =
            "#version 330 core\n"
            "layout(location=0) in vec3 a_pos;\n"
            "layout(location=1) in vec3 a_nrm;\n"
            "layout(location=2) in float a_h;\n"
            "uniform mat4 u_view; uniform mat4 u_proj;\n"
            "out vec3 v_nrm; out float v_h; out vec3 v_pos;\n"
            "void main(){ v_nrm=a_nrm; v_h=a_h; v_pos=a_pos;\n"
            "  gl_Position = u_proj * u_view * vec4(a_pos,1.0); }\n";
        // Fragment shader: 3DWorld-style multi-texture terrain splatting.
        // Blends sand/grass/rock/snow by height + slope, plus a detail texture
        // to break up the low-res tiling (this is what removes the "blurry" look).
        const char* fs =
            "#version 330 core\n"
            "in vec3 v_nrm; in float v_h; in vec3 v_pos;\n"
            "uniform vec3 u_sun;\n"
            "uniform sampler2D u_sand, u_grass, u_rock, u_snow;\n"
            "out vec4 frag;\n"
            // Triplanar sampling so textures don't stretch on a sphere.
            "vec3 triplanar(sampler2D t, vec3 p, vec3 n, float scale){\n"
            "  vec3 bw = abs(n); bw = pow(bw, vec3(4.0)); bw /= (bw.x+bw.y+bw.z);\n"
            "  vec3 x = texture(t, p.yz*scale).rgb;\n"
            "  vec3 y = texture(t, p.xz*scale).rgb;\n"
            "  vec3 z = texture(t, p.xy*scale).rgb;\n"
            "  return x*bw.x + y*bw.y + z*bw.z; }\n"
            "void main(){\n"
            "  vec3 n=normalize(v_nrm);\n"
            "  float d=max(dot(n,normalize(u_sun)),0.0)*0.85+0.15;\n"
            "  float h=v_h;\n"
            "  float slope = 1.0 - abs(dot(n, normalize(v_pos)));\n" // 0=flat, 1=cliff
            // Texture scale: large feature scale + detail handled by triplanar
            "  float S = 0.0008;\n"
            "  vec3 sand  = triplanar(u_sand,  v_pos, n, S*4.0);\n"
            "  vec3 grass = triplanar(u_grass, v_pos, n, S*4.0);\n"
            "  vec3 rock  = triplanar(u_rock,  v_pos, n, S*2.0);\n"
            "  vec3 snow  = triplanar(u_snow,  v_pos, n, S*4.0);\n"
            // Height-based weights (sand near sea, grass low, rock high, snow peaks)
            "  float w_sand  = (1.0-smoothstep(0.0,0.04,h));\n"
            "  float w_grass = smoothstep(0.0,0.06,h)*(1.0-smoothstep(0.30,0.55,h));\n"
            "  float w_rock  = smoothstep(0.30,0.55,h)*(1.0-smoothstep(0.70,0.92,h));\n"
            "  float w_snow  = smoothstep(0.70,0.92,h);\n"
            // Steep slopes always show rock
            "  float steep = smoothstep(0.45,0.75,slope);\n"
            "  vec3 land = sand*w_sand + grass*w_grass + rock*w_rock + snow*w_snow;\n"
            "  land = mix(land, rock, steep);\n"
            // Ocean below sea level
            "  vec3 ocean=vec3(0.08,0.28,0.48);\n"
            "  vec3 c = h<0.0 ? ocean : land;\n"
            "  frag=vec4(c*d,1.0);\n"
            "}\n";
        GLuint v=compile(GL_VERTEX_SHADER,vs), f=compile(GL_FRAGMENT_SHADER,fs);
        GLuint p=glCreateProgram();
        glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
        glDeleteShader(v); glDeleteShader(f);
        GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
        return ok ? p : 0;
    }
};

} // namespace sdfcraft
