#pragma once
// =============================================================================
// SDFCraft - Space backdrop: procedural starfield + nebula
// -----------------------------------------------------------------------------
// Full-screen background drawn BEFORE the planet (depth write off). Stars and
// nebula are generated procedurally in the fragment shader from the view ray,
// so there are no textures and the sky is stable as the camera rotates.
// Original implementation (no third-party engine code).
// =============================================================================
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <cstdio>

namespace sdfcraft {

class SpaceBackdrop {
public:
    bool init() {
        prog_ = build();
        glGenVertexArrays(1, &vao_); // empty VAO, fullscreen triangle via gl_VertexID
        return prog_ != 0;
    }
    void shutdown() {
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (prog_) glDeleteProgram(prog_);
    }

    // inv_view_rot: inverse of the rotation part of the view matrix (camera->world).
    void render(const glm::mat4& inv_view_rot, float tan_half_fov, float aspect, float time) {
        if (!prog_) return;
        glUseProgram(prog_);
        glUniformMatrix4fv(glGetUniformLocation(prog_, "u_inv_view"), 1, GL_FALSE, &inv_view_rot[0][0]);
        glUniform1f(glGetUniformLocation(prog_, "u_tan_half_fov"), tan_half_fov);
        glUniform1f(glGetUniformLocation(prog_, "u_aspect"), aspect);
        glUniform1f(glGetUniformLocation(prog_, "u_time"), time);
        GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        if (depth_was) glEnable(GL_DEPTH_TEST);
    }

private:
    GLuint prog_ = 0, vao_ = 0;
    // PLACEHOLDER_BUILD
    static GLuint compile(GLenum t, const char* src) {
        GLuint s = glCreateShader(t);
        glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log);
                   fprintf(stderr, "space shader: %s\n", log); }
        return s;
    }
    static GLuint build();
};

} // namespace sdfcraft

namespace sdfcraft {
inline GLuint SpaceBackdrop::build() {
    const char* vs =
        "#version 330 core\n"
        "out vec2 v_uv;\n"
        "void main(){\n"
        // fullscreen triangle
        "  vec2 p = vec2((gl_VertexID==1)?3.0:-1.0, (gl_VertexID==2)?3.0:-1.0);\n"
        "  v_uv = p;\n"
        "  gl_Position = vec4(p, 0.0, 1.0);\n"
        "}\n";
    const char* fs =
        "#version 330 core\n"
        "in vec2 v_uv;\n"
        "uniform mat4 u_inv_view;\n"
        "uniform float u_tan_half_fov, u_aspect, u_time;\n"
        "out vec4 frag;\n"
        "float hash(vec3 p){ p=fract(p*0.3183099+0.1); p*=17.0;\n"
        "  return fract(p.x*p.y*p.z*(p.x+p.y+p.z)); }\n"
        "float noise(vec3 x){\n"
        "  vec3 p=floor(x), f=fract(x); f=f*f*(3.0-2.0*f);\n"
        "  float n=mix(mix(mix(hash(p+vec3(0,0,0)),hash(p+vec3(1,0,0)),f.x),\n"
        "                  mix(hash(p+vec3(0,1,0)),hash(p+vec3(1,1,0)),f.x),f.y),\n"
        "              mix(mix(hash(p+vec3(0,0,1)),hash(p+vec3(1,0,1)),f.x),\n"
        "                  mix(hash(p+vec3(0,1,1)),hash(p+vec3(1,1,1)),f.x),f.y),f.z);\n"
        "  return n; }\n"
        "float fbm(vec3 p){ float a=0.5,s=0.0; for(int i=0;i<5;i++){ s+=a*noise(p); p*=2.02; a*=0.5; } return s; }\n"
        "void main(){\n"
        "  vec3 dir_view = normalize(vec3(v_uv.x*u_tan_half_fov*u_aspect, v_uv.y*u_tan_half_fov, -1.0));\n"
        "  vec3 rd = normalize((u_inv_view*vec4(dir_view,0.0)).xyz);\n"
        // base deep-space gradient
        "  vec3 col = mix(vec3(0.01,0.012,0.03), vec3(0.02,0.01,0.05), rd.y*0.5+0.5);\n"
        // nebula clouds (low-freq fbm, colored)
        "  float neb = fbm(rd*4.0 + 10.0);\n"
        "  neb = smoothstep(0.45,0.95,neb);\n"
        "  vec3 nebcol = mix(vec3(0.25,0.05,0.35), vec3(0.05,0.15,0.40), fbm(rd*3.0));\n"
        "  col += nebcol * neb * 0.6;\n"
        // stars: sample a high-freq grid, keep only brightest cells -> sparse points
        "  vec3 sp = rd*350.0;\n"
        "  vec3 cell = floor(sp);\n"
        "  float star = hash(cell);\n"
        "  if (star > 0.985){\n"
        "    vec3 fp = fract(sp)-0.5;\n"
        "    float d = length(fp);\n"
        "    float bright = (star-0.985)/0.015;\n"
        "    float tw = 0.7 + 0.3*sin(u_time*3.0 + star*100.0);\n"
        "    float s = smoothstep(0.18,0.0,d) * bright * tw;\n"
        "    vec3 scol = mix(vec3(0.7,0.8,1.0), vec3(1.0,0.9,0.7), hash(cell+3.0));\n"
        "    col += scol * s * 1.4;\n"
        "  }\n"
        // a few brighter stars on a coarser grid
        "  vec3 sp2 = rd*90.0; vec3 c2=floor(sp2); float st2=hash(c2+7.0);\n"
        "  if (st2>0.992){ vec3 fp=fract(sp2)-0.5; float d=length(fp);\n"
        "    col += vec3(1.0)*smoothstep(0.25,0.0,d)*((st2-0.992)/0.008)*1.6; }\n"
        "  frag = vec4(col, 1.0);\n"
        "}\n";
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}
} // namespace sdfcraft

