#include "renderer.hpp"

#include "gl_util.hpp"
#include "h264_decoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <drm_fourcc.h>

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}

namespace fastrdp {

namespace {

constexpr uint16_t kLegacySurface = 0xFFFF;

uint64_t nowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// A quad covering uRect (target pixels). Texture space and framebuffer space share
// orientation (row 0 at y = 0); only the final present to the window flips.
const char* kRectVs = R"(
uniform vec4 uRect;
uniform vec2 uTarget;
uniform int uFlip;
out vec2 vUV;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vUV = c;
    vec2 p = (uRect.xy + c * uRect.zw) / uTarget * 2.0 - 1.0;
    if (uFlip == 1) p.y = -p.y;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Surfaces store BGRA bytes in RGBA8 textures (FreeRDP's native layout, zero CPU swizzle).
const char* kPresentFs = R"(
in vec2 vUV;
uniform sampler2D uTex;
out vec4 o;
void main() {
    vec4 t = texture(uTex, vUV);
    o = vec4(t.b, t.g, t.r, 1.0);
}
)";

const char* kAlphaFs = R"(
uniform sampler2D uTex;
uniform ivec2 uOrigin;
out vec4 o;
void main() {
    float a = texelFetch(uTex, ivec2(gl_FragCoord.xy) - uOrigin, 0).r;
    o = vec4(0.0, 0.0, 0.0, a);
}
)";

// Shared YUV sampling. NV12 (hardware / downloaded) keeps chroma interleaved in uU.
// RDP's H.264 uses BT.709 coefficients at full range.
const char* kYuvCommon = R"(
uniform sampler2D uY;
uniform sampler2D uU;
uniform sampler2D uV;
uniform int uNV12;
out vec4 o;
float Y(ivec2 p) { return texelFetch(uY, p, 0).r; }
vec2 UV(ivec2 p) {
    if (uNV12 == 1) return texelFetch(uU, p, 0).rg;
    return vec2(texelFetch(uU, p, 0).r, texelFetch(uV, p, 0).r);
}
vec3 toBGR(float y, float u, float v) {
    u -= 128.0 / 255.0;
    v -= 128.0 / 255.0;
    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;
    return clamp(vec3(b, g, r), 0.0, 1.0);
}
)";

const char* kYuv420Fs = R"(
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 c = UV(p >> 1);
    o = vec4(toBGR(Y(p), c.x, c.y), 1.0);
}
)";

// AVC444 main view: luma plus 4:2:0 chroma replicated over each 2x2 block
// ([MS-RDPEGFX] 3.3.8.3.2, B1-B3).
const char* kLumaFs = R"(
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    o = vec4(Y(p), UV(p >> 1), 1.0);
}
)";

// AVC444 auxiliary view, v1 layout. Luma plane rows carry odd chroma rows (8 rows of U
// then 8 rows of V per 16-row block, B4/B5); the chroma planes carry odd columns of even
// rows (B6/B7). Even/even samples come from the main view and are left untouched.
const char* kChromaV1Fs = R"(
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 c;
    if ((p.y & 1) == 1) {
        int k = p.y >> 1;
        int base = (k >> 3) * 16 + (k & 7);
        c = vec2(Y(ivec2(p.x, base)), Y(ivec2(p.x, base + 8)));
    } else if ((p.x & 1) == 1) {
        c = UV(p >> 1);
    } else {
        discard;
    }
    o = vec4(0.0, c, 0.0);
}
)";

// AVC444v2 auxiliary view: luma plane holds odd columns (U left half, V right half);
// chroma planes hold even columns of odd rows (U/V split into quarters).
const char* kChromaV2Fs = R"(
uniform int uHalfW;
uniform int uQuarterW;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 c;
    if ((p.x & 1) == 1) {
        int xx = p.x >> 1;
        c = vec2(Y(ivec2(xx, p.y)), Y(ivec2(uHalfW + xx, p.y)));
    } else if ((p.y & 1) == 1) {
        int j = p.y >> 1;
        int q = p.x >> 2;
        if ((p.x & 3) == 0)
            c = vec2(UV(ivec2(q, j)).x, UV(ivec2(uQuarterW + q, j)).x);
        else
            c = vec2(UV(ivec2(q, j)).y, UV(ivec2(uQuarterW + q, j)).y);
    } else {
        discard;
    }
    o = vec4(0.0, c, 0.0);
}
)";

const char* kConvert444Fs = R"(
uniform sampler2D u444;
out vec4 o;
vec3 toBGR(float y, float u, float v) {
    u -= 128.0 / 255.0;
    v -= 128.0 / 255.0;
    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;
    return clamp(vec3(b, g, r), 0.0, 1.0);
}
void main() {
    vec4 t = texelFetch(u444, ivec2(gl_FragCoord.xy), 0);
    o = vec4(toBGR(t.r, t.g, t.b), 1.0);
}
)";

std::string withYuv(const char* body) { return std::string(kYuvCommon) + body; }

bool hasExt(const char* list, const char* ext) {
    if (!list) return false;
    const size_t n = strlen(ext);
    for (const char* p = strstr(list, ext); p; p = strstr(p + 1, ext))
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    return false;
}

void setSamplers(GLuint prog) {
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uY"), 0);
    glUniform1i(glGetUniformLocation(prog, "uU"), 1);
    glUniform1i(glGetUniformLocation(prog, "uV"), 2);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 3);
    glUniform1i(glGetUniformLocation(prog, "u444"), 4);
}

} // namespace

bool Renderer::init(EGLDisplay display) {
    egl_ = display;

    const char* eglExts = eglQueryString(egl_, EGL_EXTENSIONS);
    zeroCopy_ = hasExt(eglExts, "EGL_EXT_image_dma_buf_import") &&
                hasExt(eglExts, "EGL_EXT_image_dma_buf_import_modifiers") &&
                epoxy_has_gl_extension("GL_OES_EGL_image");
    if (!zeroCopy_) VideoConfig::zeroCopyBroken() = true;

    // Find which GPU the compositor/EGL is on so VAAPI decodes there too.
    const char* clientExts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (hasExt(clientExts, "EGL_EXT_device_query")) {
        EGLAttrib dev = 0;
        if (eglQueryDisplayAttribEXT(egl_, EGL_DEVICE_EXT, &dev) && dev) {
            auto device = reinterpret_cast<EGLDeviceEXT>(dev);
            const char* devExts = eglQueryDeviceStringEXT(device, EGL_EXTENSIONS);
            if (hasExt(devExts, "EGL_EXT_device_drm_render_node")) {
                const char* node = eglQueryDeviceStringEXT(device, EGL_DRM_RENDER_NODE_FILE_EXT);
                if (node) renderNode_ = node;
            }
        }
    }

    fprintf(stderr, "[gl] %s | %s | zero-copy video %s | render node %s\n",
            reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
            reinterpret_cast<const char*>(glGetString(GL_VERSION)), zeroCopy_ ? "yes" : "no",
            renderNode_.empty() ? "?" : renderNode_.c_str());

    progPresent_ = gl::makeProgram("present", kRectVs, kPresentFs);
    progAlpha_ = gl::makeProgram("alpha", kRectVs, kAlphaFs);
    progYuv420_ = gl::makeProgram("yuv420", kRectVs, withYuv(kYuv420Fs).c_str());
    progLuma_ = gl::makeProgram("avc444-luma", kRectVs, withYuv(kLumaFs).c_str());
    progChromaV1_ = gl::makeProgram("avc444-chroma-v1", kRectVs, withYuv(kChromaV1Fs).c_str());
    progChromaV2_ = gl::makeProgram("avc444-chroma-v2", kRectVs, withYuv(kChromaV2Fs).c_str());
    progConvert444_ = gl::makeProgram("avc444-convert", kRectVs, kConvert444Fs);
    if (!progPresent_ || !progAlpha_ || !progYuv420_ || !progLuma_ || !progChromaV1_ ||
        !progChromaV2_ || !progConvert444_)
        return false;
    for (GLuint p : {progPresent_, progAlpha_, progYuv420_, progLuma_, progChromaV1_,
                     progChromaV2_, progConvert444_})
        setSamplers(p);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    glGenTextures(3, planeTex_);
    for (GLuint t : planeTex_) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glGenTextures(1, &alphaTex_);
    glBindTexture(GL_TEXTURE_2D, alphaTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    return true;
}

void Renderer::shutdown() {
    releaseRetiredFrames(true);
    inFlight_.clear();
    for (auto& [id, s] : surfaces_) destroySurface(s);
    surfaces_.clear();
    for (auto& [slot, c] : cache_) destroyCache(c);
    cache_.clear();
    if (scratchFbo_) glDeleteFramebuffers(1, &scratchFbo_);
    if (scratchTex_) glDeleteTextures(1, &scratchTex_);
    glDeleteTextures(3, planeTex_);
    glDeleteTextures(1, &alphaTex_);
    glDeleteVertexArrays(1, &vao_);
    for (GLuint p : {progPresent_, progAlpha_, progYuv420_, progLuma_, progChromaV1_,
                     progChromaV2_, progConvert444_})
        glDeleteProgram(p);
}

bool Renderer::execute(std::vector<Command>& commands) {
    releaseRetiredFrames(false);
    if (commands.empty()) return false;

    const uint64_t t0 = nowUs();
    bool visible = false;
    for (auto& c : commands) {
        if (std::holds_alternative<cmd::EndFrame>(c) || std::holds_alternative<cmd::LegacyUpdate>(c))
            visible = true;
        std::visit([this](auto& x) { exec(x); }, c);
    }
    commands.clear();

    if (!inFlight_.empty()) {
        Retired r{glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), std::move(inFlight_)};
        inFlight_.clear();
        retired_.push_back(std::move(r));
    }
    stats_.renderUsTotal += nowUs() - t0;
    return visible;
}

void Renderer::releaseRetiredFrames(bool wait) {
    while (!retired_.empty()) {
        auto& r = retired_.front();
        GLenum st = glClientWaitSync(r.fence, 0, wait ? 1000000000ull : 0);
        if (st == GL_TIMEOUT_EXPIRED) break;
        glDeleteSync(r.fence);
        retired_.pop_front();
    }
}

Renderer::Surface* Renderer::surface(uint16_t id) {
    auto it = surfaces_.find(id);
    return it == surfaces_.end() ? nullptr : &it->second;
}

void Renderer::destroySurface(Surface& s) {
    if (s.fbo) glDeleteFramebuffers(1, &s.fbo);
    if (s.tex) glDeleteTextures(1, &s.tex);
    if (s.yuv444Fbo) glDeleteFramebuffers(1, &s.yuv444Fbo);
    if (s.yuv444Tex) glDeleteTextures(1, &s.yuv444Tex);
    s = {};
}

void Renderer::destroyCache(CacheEntry& c) {
    if (c.fbo) glDeleteFramebuffers(1, &c.fbo);
    if (c.tex) glDeleteTextures(1, &c.tex);
    c = {};
}

// ---------------------------------------------------------------------------------------

void Renderer::exec(const cmd::ResetGraphics& c) {
    desktopW_ = c.width;
    desktopH_ = c.height;
    // The legacy fallback surface is meaningless once GFX drives the output.
    if (auto* s = surface(kLegacySurface)) {
        destroySurface(*s);
        surfaces_.erase(kLegacySurface);
    }
}

void Renderer::exec(const cmd::CreateSurface& c) {
    if (auto* old = surface(c.id)) destroySurface(*old);
    Surface s;
    s.w = std::max<uint32_t>(1, c.width);
    s.h = std::max<uint32_t>(1, c.height);
    s.alpha = c.alpha;
    s.tex = gl::makeTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, s.w, s.h);
    s.fbo = gl::makeFramebuffer(s.tex);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    surfaces_[c.id] = s;
}

void Renderer::exec(const cmd::DeleteSurface& c) {
    if (auto* s = surface(c.id)) {
        destroySurface(*s);
        surfaces_.erase(c.id);
    }
}

void Renderer::exec(const cmd::MapSurfaceToOutput& c) {
    if (auto* s = surface(c.id)) {
        s->mapped = true;
        s->outX = c.x;
        s->outY = c.y;
        s->targetW = c.targetW;
        s->targetH = c.targetH;
    }
}

void Renderer::exec(const cmd::SolidFill& c) {
    auto* s = surface(c.id);
    if (!s) return;
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glViewport(0, 0, s->w, s->h);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]);
    for (const Rect& r : c.rects) {
        glScissor(r.x, r.y, r.w, r.h);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

void Renderer::blit(GLuint srcFbo, const Rect& src, GLuint dstFbo, int dx, int dy,
                    uint32_t dstW, uint32_t dstH) {
    // Clip the destination to the target, shifting the source to match.
    int sx = src.x, sy = src.y, w = src.w, h = src.h;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    w = std::min<int>(w, int(dstW) - dx);
    h = std::min<int>(h, int(dstH) - dy);
    if (w <= 0 || h <= 0) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
}

void Renderer::ensureScratch(uint32_t w, uint32_t h) {
    if (scratchW_ >= w && scratchH_ >= h) return;
    if (scratchFbo_) glDeleteFramebuffers(1, &scratchFbo_);
    if (scratchTex_) glDeleteTextures(1, &scratchTex_);
    scratchW_ = std::max(w, scratchW_);
    scratchH_ = std::max(h, scratchH_);
    scratchTex_ = gl::makeTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, scratchW_, scratchH_);
    scratchFbo_ = gl::makeFramebuffer(scratchTex_);
}

// The hot path for scrolling: the server copies the still-valid part of the window and
// only sends the newly exposed strip.
void Renderer::exec(const cmd::SurfaceToSurface& c) {
    auto* src = surface(c.src);
    auto* dst = surface(c.dst);
    if (!src || !dst) return;
    if (c.src == c.dst) {
        // Overlapping blits within one framebuffer are undefined; bounce via scratch.
        ensureScratch(c.rect.w, c.rect.h);
        blit(src->fbo, c.rect, scratchFbo_, 0, 0, scratchW_, scratchH_);
        const Rect tmp{0, 0, c.rect.w, c.rect.h};
        for (const Point& p : c.dests) blit(scratchFbo_, tmp, dst->fbo, p.x, p.y, dst->w, dst->h);
    } else {
        for (const Point& p : c.dests) blit(src->fbo, c.rect, dst->fbo, p.x, p.y, dst->w, dst->h);
    }
}

void Renderer::exec(const cmd::SurfaceToCache& c) {
    auto* s = surface(c.id);
    if (!s) return;
    CacheEntry& e = cache_[c.slot];
    if (e.w != uint32_t(c.rect.w) || e.h != uint32_t(c.rect.h)) {
        destroyCache(e);
        e.w = c.rect.w;
        e.h = c.rect.h;
        e.tex = gl::makeTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, e.w, e.h);
        e.fbo = gl::makeFramebuffer(e.tex);
    }
    blit(s->fbo, c.rect, e.fbo, 0, 0, e.w, e.h);
}

void Renderer::exec(const cmd::CacheToSurface& c) {
    auto it = cache_.find(c.slot);
    auto* s = surface(c.id);
    if (it == cache_.end() || !s) return;
    const CacheEntry& e = it->second;
    const Rect r{0, 0, int32_t(e.w), int32_t(e.h)};
    for (const Point& p : c.dests) blit(e.fbo, r, s->fbo, p.x, p.y, s->w, s->h);
}

void Renderer::exec(const cmd::EvictCache& c) {
    auto it = cache_.find(c.slot);
    if (it == cache_.end()) return;
    destroyCache(it->second);
    cache_.erase(it);
}

void Renderer::exec(const cmd::UploadBGRA& c) {
    auto* s = surface(c.id);
    if (!s || c.rect.empty()) return;
    if (c.rect.x + c.rect.w > int(s->w) || c.rect.y + c.rect.h > int(s->h)) return;
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, c.rect.x, c.rect.y, c.rect.w, c.rect.h, GL_RGBA,
                    GL_UNSIGNED_BYTE, c.pixels.data());
}

void Renderer::drawRects(GLuint program, const std::vector<Rect>& rects, uint32_t targetW,
                         uint32_t targetH) {
    glUseProgram(program);
    glViewport(0, 0, targetW, targetH);
    glUniform2f(glGetUniformLocation(program, "uTarget"), float(targetW), float(targetH));
    glUniform1i(glGetUniformLocation(program, "uFlip"), 0);
    const GLint loc = glGetUniformLocation(program, "uRect");
    for (const Rect& r : rects) {
        glUniform4f(loc, float(r.x), float(r.y), float(r.w), float(r.h));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}

void Renderer::exec(const cmd::UploadAlpha& c) {
    auto* s = surface(c.id);
    if (!s || c.rect.empty()) return;
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, alphaTex_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, c.rect.w, c.rect.h, 0, GL_RED, GL_UNSIGNED_BYTE,
                 c.alpha.data());
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glUseProgram(progAlpha_);
    glUniform2i(glGetUniformLocation(progAlpha_, "uOrigin"), c.rect.x, c.rect.y);
    drawRects(progAlpha_, {c.rect}, s->w, s->h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
}

// ---------------------------------------------------------------------------------------
// Video

bool Renderer::bindVideoPlanes(const VideoFramePtr& vf, int& frameW, int& frameH, bool& nv12) {
    AVFrame* f = vf->frame;
    frameW = f->width;
    frameH = f->height;

    if (f->format == AV_PIX_FMT_DRM_PRIME) {
        auto* desc = reinterpret_cast<const AVDRMFrameDescriptor*>(f->data[0]);
        struct Plane { int fd; uint64_t modifier; ptrdiff_t offset, pitch; };
        Plane planes[4];
        int n = 0;
        for (int l = 0; l < desc->nb_layers && n < 4; l++)
            for (int p = 0; p < desc->layers[l].nb_planes && n < 4; p++) {
                const auto& pl = desc->layers[l].planes[p];
                const auto& obj = desc->objects[pl.object_index];
                planes[n++] = {obj.fd, obj.format_modifier, pl.offset, pl.pitch};
            }
        if (n < 2) return false;

        const uint32_t fourcc[2] = {DRM_FORMAT_R8, DRM_FORMAT_GR88};
        const int w[2] = {frameW, (frameW + 1) / 2};
        const int h[2] = {frameH, (frameH + 1) / 2};
        for (int i = 0; i < 2; i++) {
            std::vector<EGLAttrib> a = {
                EGL_WIDTH, w[i], EGL_HEIGHT, h[i], EGL_LINUX_DRM_FOURCC_EXT, EGLAttrib(fourcc[i]),
                EGL_DMA_BUF_PLANE0_FD_EXT, planes[i].fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLAttrib(planes[i].offset),
                EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLAttrib(planes[i].pitch)};
            if (planes[i].modifier != DRM_FORMAT_MOD_INVALID) {
                a.insert(a.end(), {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                                   EGLAttrib(planes[i].modifier & 0xFFFFFFFFu),
                                   EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                                   EGLAttrib(planes[i].modifier >> 32)});
            }
            a.push_back(EGL_NONE);
            EGLImage img = eglCreateImage(egl_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr,
                                          a.data());
            if (img == EGL_NO_IMAGE) {
                fprintf(stderr, "[gl] dmabuf import failed (0x%x); falling back to frame download\n",
                        eglGetError());
                VideoConfig::zeroCopyBroken() = true;
                return false;
            }
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, planeTex_[i]);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
            eglDestroyImage(egl_, img);
            planeW_[i] = planeH_[i] = -1; // storage now belongs to the image
        }
        nv12 = true;
        glActiveTexture(GL_TEXTURE0);
        // Keep the VA surface alive until the GPU has finished sampling it.
        inFlight_.push_back(vf);
        return true;
    }

    // Software / downloaded frames.
    struct P { GLenum ifmt, fmt; int w, h, bpp; };
    P p[3];
    int count = 0;
    if (f->format == AV_PIX_FMT_NV12) {
        p[0] = {GL_R8, GL_RED, frameW, frameH, 1};
        p[1] = {GL_RG8, GL_RG, (frameW + 1) / 2, (frameH + 1) / 2, 2};
        count = 2;
        nv12 = true;
    } else if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        p[0] = {GL_R8, GL_RED, frameW, frameH, 1};
        p[1] = {GL_R8, GL_RED, (frameW + 1) / 2, (frameH + 1) / 2, 1};
        p[2] = p[1];
        count = 3;
        nv12 = false;
    } else {
        fprintf(stderr, "[gl] unsupported decoded pixel format %d\n", f->format);
        return false;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < count; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, planeTex_[i]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, f->linesize[i] / p[i].bpp);
        if (planeW_[i] != p[i].w || planeH_[i] != p[i].h) {
            glTexImage2D(GL_TEXTURE_2D, 0, p[i].ifmt, p[i].w, p[i].h, 0, p[i].fmt,
                         GL_UNSIGNED_BYTE, f->data[i]);
            planeW_[i] = p[i].w;
            planeH_[i] = p[i].h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p[i].w, p[i].h, p[i].fmt, GL_UNSIGNED_BYTE,
                            f->data[i]);
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glActiveTexture(GL_TEXTURE0);
    return true;
}

void Renderer::exec(const cmd::Video& c) {
    auto* s = surface(c.id);
    if (!s || !c.frame || !c.frame->frame) return;

    int fw = 0, fh = 0;
    bool nv12 = false;
    if (!bindVideoPlanes(c.frame, fw, fh, nv12)) return;

    auto setNv12 = [&](GLuint prog) {
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "uNV12"), nv12 ? 1 : 0);
    };

    if (c.pass == VideoPass::Avc420) {
        setNv12(progYuv420_);
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
        drawRects(progYuv420_, c.rects, s->w, s->h);
        return;
    }

    if (!s->yuv444Tex) {
        s->yuv444Tex = gl::makeTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, s->w, s->h);
        s->yuv444Fbo = gl::makeFramebuffer(s->yuv444Tex);
        glClearColor(0, 0.5f, 0.5f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, s->yuv444Fbo);
    if (c.pass == VideoPass::Avc444Luma) {
        setNv12(progLuma_);
        drawRects(progLuma_, c.rects, s->w, s->h);
    } else {
        GLuint prog = c.pass == VideoPass::Avc444ChromaV1 ? progChromaV1_ : progChromaV2_;
        setNv12(prog);
        if (prog == progChromaV2_) {
            glUniform1i(glGetUniformLocation(prog, "uHalfW"), fw / 2);
            glUniform1i(glGetUniformLocation(prog, "uQuarterW"), fw / 4);
        }
        glColorMask(GL_FALSE, GL_TRUE, GL_TRUE, GL_FALSE);
        drawRects(prog, c.rects, s->w, s->h);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    if (!c.convertRects.empty()) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, s->yuv444Tex);
        glActiveTexture(GL_TEXTURE0);
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
        drawRects(progConvert444_, c.convertRects, s->w, s->h);
    }
}

void Renderer::exec(const cmd::EndFrame& c) {
    stats_.gfxFrames++;
    stats_.lastFrameDecodeUs = c.decodeUs;
}

void Renderer::exec(const cmd::LegacyUpdate& c) {
    Surface* s = surface(kLegacySurface);
    if (!s || s->w != c.width || s->h != c.height) {
        exec(cmd::CreateSurface{kLegacySurface, c.width, c.height, false});
        s = surface(kLegacySurface);
        s->mapped = true;
        s->targetW = c.width;
        s->targetH = c.height;
        desktopW_ = c.width;
        desktopH_ = c.height;
    }
    exec(cmd::UploadBGRA{kLegacySurface, c.rect, c.pixels, false});
}

// ---------------------------------------------------------------------------------------

View Renderer::fitView(int windowW, int windowH) const {
    View v;
    if (!desktopW_ || !desktopH_ || windowW <= 0 || windowH <= 0) return v;
    switch (scaleMode_) {
    case ScaleMode::Native: break;
    case ScaleMode::Stretch:
        v.scaleX = float(windowW) / desktopW_;
        v.scaleY = float(windowH) / desktopH_;
        break;
    case ScaleMode::Fit:
        v.scaleX = v.scaleY = std::min(float(windowW) / desktopW_, float(windowH) / desktopH_);
        break;
    }
    if (std::fabs(v.scaleX - 1.0f) < 1e-3f && std::fabs(v.scaleY - 1.0f) < 1e-3f)
        v.scaleX = v.scaleY = 1.0f;
    if (scaleMode_ != ScaleMode::Native) {
        v.offsetX = std::floor((windowW - desktopW_ * v.scaleX) * 0.5f);
        v.offsetY = std::floor((windowH - desktopH_ * v.scaleY) * 0.5f);
    }
    return v;
}

View Renderer::regionView(int windowW, int windowH, const Rect& region) {
    View v;
    v.srcX = float(region.x);
    v.srcY = float(region.y);
    if (region.w > 0 && region.h > 0) {
        v.scaleX = float(windowW) / region.w;
        v.scaleY = float(windowH) / region.h;
    }
    if (std::fabs(v.scaleX - 1.0f) < 1e-3f && std::fabs(v.scaleY - 1.0f) < 1e-3f)
        v.scaleX = v.scaleY = 1.0f;
    return v;
}

void Renderer::present(int windowW, int windowH, const View& v) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowW, windowH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!desktopW_ || !desktopH_ || windowW <= 0 || windowH <= 0) return;

    const bool exact = v.scaleX == 1.0f && v.scaleY == 1.0f;
    glUseProgram(progPresent_);
    glUniform2f(glGetUniformLocation(progPresent_, "uTarget"), float(windowW), float(windowH));
    glUniform1i(glGetUniformLocation(progPresent_, "uFlip"), 1);
    const GLint rectLoc = glGetUniformLocation(progPresent_, "uRect");
    glActiveTexture(GL_TEXTURE3);
    for (auto& [id, s] : surfaces_) {
        if (!s.mapped) continue;
        const uint32_t tw = s.targetW ? s.targetW : s.w;
        const uint32_t th = s.targetH ? s.targetH : s.h;
        const float x = v.offsetX + (s.outX - v.srcX) * v.scaleX;
        const float y = v.offsetY + (s.outY - v.srcY) * v.scaleY;
        const float w = tw * v.scaleX, h = th * v.scaleY;
        if (x >= windowW || y >= windowH || x + w <= 0 || y + h <= 0) continue;
        glBindTexture(GL_TEXTURE_2D, s.tex);
        const GLint filter = exact ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glUniform4f(rectLoc, x, y, w, h);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glActiveTexture(GL_TEXTURE0);
    stats_.presents++;
}

bool View::toDesktop(float wx, float wy, uint32_t desktopW, uint32_t desktopH, int& dx,
                     int& dy) const {
    if (!desktopW || !desktopH) return false;
    const float x = (wx - offsetX) / scaleX + srcX;
    const float y = (wy - offsetY) / scaleY + srcY;
    dx = std::clamp(int(x), 0, int(desktopW) - 1);
    dy = std::clamp(int(y), 0, int(desktopH) - 1);
    return true;
}

} // namespace fastrdp
