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
#include <cstddef>

namespace sdfcraft {

class PlanetRenderer {
public:
    bool init() {
        prog_ = build_program();
        if (!prog_) return false;
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        return true;
    }
    void shutdown() {
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (prog_) glDeleteProgram(prog_);
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
        glEnable(GL_DEPTH_TEST);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, count_);
        glBindVertexArray(0);
    }

private:
    GLuint prog_=0, vao_=0, vbo_=0; GLsizei count_=0;

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
        const char* fs =
            "#version 330 core\n"
            "in vec3 v_nrm; in float v_h; in vec3 v_pos;\n"
            "uniform vec3 u_sun;\n"
            "out vec4 frag;\n"
            "void main(){\n"
            "  vec3 n=normalize(v_nrm);\n"
            "  float d=max(dot(n,normalize(u_sun)),0.0)*0.8+0.2;\n"
            "  // ocean -> land -> mountain -> snow tint by normalised height\n"
            "  vec3 ocean=vec3(0.05,0.20,0.45), land=vec3(0.18,0.42,0.12);\n"
            "  vec3 rock=vec3(0.40,0.34,0.28), snow=vec3(0.95,0.95,0.98);\n"
            "  float h=v_h;\n"
            "  vec3 c = h<0.0 ? ocean : mix(land,rock,smoothstep(0.0,0.5,h));\n"
            "  c = mix(c,snow,smoothstep(0.6,1.0,h));\n"
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
