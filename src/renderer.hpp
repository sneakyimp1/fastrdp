#pragma once

#include "commands.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <epoxy/egl.h>
#include <epoxy/gl.h>

namespace fastrdp {

enum class ScaleMode { Fit, Stretch, Native };

struct RenderStats {
    uint64_t presents = 0;
    uint64_t gfxFrames = 0;
    uint64_t lastFrameDecodeUs = 0;
    uint64_t renderUsTotal = 0;
};

// Owns all GL state. Every method must be called on the thread that owns the GL context.
class Renderer {
public:
    bool init(EGLDisplay display);
    void shutdown();

    // Executes queued commands. Returns true when something visible changed.
    bool execute(std::vector<Command>& commands);

    // Draws all mapped surfaces into the window's default framebuffer.
    void present(int windowW, int windowH);

    // Session (remote desktop) size, as last announced by the server.
    uint32_t desktopWidth() const { return desktopW_; }
    uint32_t desktopHeight() const { return desktopH_; }

    // Window-pixel -> desktop-pixel mapping used by the last present, for input.
    bool windowToDesktop(float wx, float wy, int& dx, int& dy) const;

    void setScaleMode(ScaleMode m) { scaleMode_ = m; }
    const RenderStats& stats() const { return stats_; }

    // DRM render node backing the EGL display ("" if unknown).
    std::string renderNode() const { return renderNode_; }

private:
    struct Surface {
        GLuint tex = 0, fbo = 0;
        uint32_t w = 0, h = 0;
        bool alpha = false;
        bool mapped = false;
        int32_t outX = 0, outY = 0;
        uint32_t targetW = 0, targetH = 0;
        GLuint yuv444Tex = 0, yuv444Fbo = 0; // AVC444 reconstruction plane
    };
    struct CacheEntry {
        GLuint tex = 0, fbo = 0;
        uint32_t w = 0, h = 0;
    };

    void exec(const cmd::ResetGraphics&);
    void exec(const cmd::CreateSurface&);
    void exec(const cmd::DeleteSurface&);
    void exec(const cmd::MapSurfaceToOutput&);
    void exec(const cmd::SolidFill&);
    void exec(const cmd::SurfaceToSurface&);
    void exec(const cmd::SurfaceToCache&);
    void exec(const cmd::CacheToSurface&);
    void exec(const cmd::EvictCache&);
    void exec(const cmd::UploadBGRA&);
    void exec(const cmd::UploadAlpha&);
    void exec(const cmd::Video&);
    void exec(const cmd::EndFrame&);
    void exec(const cmd::LegacyUpdate&);

    void destroySurface(Surface& s);
    void destroyCache(CacheEntry& c);
    Surface* surface(uint16_t id);
    void blit(GLuint srcFbo, const Rect& src, GLuint dstFbo, int dx, int dy, uint32_t dstW,
              uint32_t dstH);
    void ensureScratch(uint32_t w, uint32_t h);
    void drawRects(GLuint program, const std::vector<Rect>& rects, uint32_t targetW,
                   uint32_t targetH);

    bool bindVideoPlanes(const VideoFramePtr& frame, int& frameW, int& frameH, bool& nv12);
    void releaseRetiredFrames(bool wait);

    GLuint rectVs_ = 0;
    GLuint progPresent_ = 0, progAlpha_ = 0;
    GLuint progYuv420_ = 0, progLuma_ = 0, progChromaV1_ = 0, progChromaV2_ = 0,
           progConvert444_ = 0;
    GLuint vao_ = 0;
    GLuint planeTex_[3] = {};
    int planeW_[3] = {}, planeH_[3] = {};
    GLuint alphaTex_ = 0;
    GLuint scratchTex_ = 0, scratchFbo_ = 0;
    uint32_t scratchW_ = 0, scratchH_ = 0;

    EGLDisplay egl_ = EGL_NO_DISPLAY;
    bool zeroCopy_ = false;
    std::string renderNode_;

    std::unordered_map<uint16_t, Surface> surfaces_;
    std::unordered_map<uint16_t, CacheEntry> cache_;
    uint32_t desktopW_ = 0, desktopH_ = 0;
    ScaleMode scaleMode_ = ScaleMode::Fit;

    // Video frames the GPU may still be sampling; released once their fence signals.
    struct Retired {
        GLsync fence;
        std::vector<VideoFramePtr> frames;
    };
    std::vector<VideoFramePtr> inFlight_;
    std::deque<Retired> retired_;

    // Last present transform (window pixels).
    float viewX_ = 0, viewY_ = 0, viewScaleX_ = 1, viewScaleY_ = 1;

    RenderStats stats_;
};

} // namespace fastrdp
