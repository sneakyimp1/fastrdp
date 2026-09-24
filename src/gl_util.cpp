#include "gl_util.hpp"

#include <cstdio>
#include <string>

namespace fastrdp::gl {

namespace {

GLuint compile(const char* name, GLenum type, const char* src) {
    const std::string full = std::string("#version 300 es\nprecision highp float;\nprecision highp int;\n") + src;
    const char* p = full.c_str();
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        fprintf(stderr, "[gl] %s %s shader failed:\n%s\n", name,
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

} // namespace

GLuint makeProgram(const char* name, const char* vs, const char* fs) {
    GLuint v = compile(name, GL_VERTEX_SHADER, vs);
    GLuint f = compile(name, GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        fprintf(stderr, "[gl] %s link failed:\n%s\n", name, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

GLuint makeTexture(GLenum internalFormat, GLenum format, GLenum type, int w, int h,
                   GLenum filter) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, internalFormat, w, h);
    (void)format;
    (void)type;
    return tex;
}

GLuint makeFramebuffer(GLuint tex) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "[gl] framebuffer incomplete 0x%x\n", st);
    return fbo;
}

} // namespace fastrdp::gl
