#pragma once

#include <epoxy/gl.h>

namespace fastrdp::gl {

// Compiles and links a program from GLSL ES 3.0 sources (the "#version" line is
// prepended). Returns 0 and prints the log on failure.
GLuint makeProgram(const char* name, const char* vs, const char* fs);

GLuint makeTexture(GLenum internalFormat, GLenum format, GLenum type, int w, int h,
                   GLenum filter = GL_NEAREST);

// Framebuffer with a single color attachment.
GLuint makeFramebuffer(GLuint tex);

} // namespace fastrdp::gl
