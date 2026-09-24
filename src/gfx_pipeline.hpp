#pragma once

#include "commands.hpp"
#include "h264_decoder.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <freerdp/client/rdpgfx.h>
#include <freerdp/codec/clear.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/planar.h>
#include <freerdp/codec/progressive.h>
#include <freerdp/codec/rfx.h>

namespace fastrdp {

struct GfxStats {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> decodeUsTotal{0};
    std::atomic<uint64_t> bytesIn{0};
    std::atomic<uint64_t> h264Frames{0};
    std::atomic<uint32_t> lastCodec{0};
    std::atomic<bool> hwVideo{false};
    std::atomic<uint32_t> capsVersion{0};
};

// Implements the RDPGFX client callbacks. Runs on FreeRDP's dynamic-channel thread.
// Decodes everything that needs the CPU there and forwards GPU work to the renderer.
class GfxPipeline {
public:
    GfxPipeline(rdpContext* context, CommandQueue& queue, std::function<void()> wake);
    ~GfxPipeline();

    void attach(RdpgfxClientContext* gfx);
    void detach();

    GfxStats& stats() { return stats_; }

private:
    struct Surface {
        uint16_t id = 0;
        uint32_t width = 0, height = 0;
        bool alpha = false;
        std::unique_ptr<H264Decoder> h264;
        // CPU shadow for codecs that decode into a full framebuffer (progressive, RFX).
        std::vector<uint8_t> shadow;
        uint32_t shadowStride = 0;
        bool progressiveCtx = false;
    };

    // RDPGFX callbacks
    static UINT cbResetGraphics(RdpgfxClientContext*, const RDPGFX_RESET_GRAPHICS_PDU*);
    static UINT cbStartFrame(RdpgfxClientContext*, const RDPGFX_START_FRAME_PDU*);
    static UINT cbEndFrame(RdpgfxClientContext*, const RDPGFX_END_FRAME_PDU*);
    static UINT cbSurfaceCommand(RdpgfxClientContext*, const RDPGFX_SURFACE_COMMAND*);
    static UINT cbDeleteEncodingContext(RdpgfxClientContext*,
                                        const RDPGFX_DELETE_ENCODING_CONTEXT_PDU*);
    static UINT cbCreateSurface(RdpgfxClientContext*, const RDPGFX_CREATE_SURFACE_PDU*);
    static UINT cbDeleteSurface(RdpgfxClientContext*, const RDPGFX_DELETE_SURFACE_PDU*);
    static UINT cbSolidFill(RdpgfxClientContext*, const RDPGFX_SOLID_FILL_PDU*);
    static UINT cbSurfaceToSurface(RdpgfxClientContext*, const RDPGFX_SURFACE_TO_SURFACE_PDU*);
    static UINT cbSurfaceToCache(RdpgfxClientContext*, const RDPGFX_SURFACE_TO_CACHE_PDU*);
    static UINT cbCacheToSurface(RdpgfxClientContext*, const RDPGFX_CACHE_TO_SURFACE_PDU*);
    static UINT cbCacheImportReply(RdpgfxClientContext*, const RDPGFX_CACHE_IMPORT_REPLY_PDU*);
    static UINT cbEvictCacheEntry(RdpgfxClientContext*, const RDPGFX_EVICT_CACHE_ENTRY_PDU*);
    static UINT cbMapSurfaceToOutput(RdpgfxClientContext*, const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU*);
    static UINT cbMapSurfaceToScaledOutput(RdpgfxClientContext*,
                                           const RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU*);
    static UINT cbCapsConfirm(RdpgfxClientContext*, const RDPGFX_CAPS_CONFIRM_PDU*);

    UINT surfaceCommand(const RDPGFX_SURFACE_COMMAND* cmd);
    UINT cmdUncompressed(Surface& s, const RDPGFX_SURFACE_COMMAND* cmd);
    UINT cmdPlanar(Surface& s, const RDPGFX_SURFACE_COMMAND* cmd);
    UINT cmdClear(Surface& s, const RDPGFX_SURFACE_COMMAND* cmd);
    UINT cmdProgressive(Surface& s, const RDPGFX_SURFACE_COMMAND* cmd);
    UINT cmdRemoteFx(Surface& s, const RDPGFX_SURFACE_COMMAND* cmd);
    UINT cmdAvc420(Surface& s, const RDPGFX_SURFACE_COMMAND* cmd);
    UINT cmdAvc444(Surface& s, const RDPGFX_SURFACE_COMMAND* cmd);
    UINT cmdAlpha(Surface& s, const RDPGFX_SURFACE_COMMAND* cmd);

    void ensureShadow(Surface& s);
    void uploadFromShadow(Surface& s, const RECTANGLE_16* rects, uint32_t count);
    std::vector<Rect> clampRects(const Surface& s, const RECTANGLE_16* rects, uint32_t n) const;
    Surface* surface(uint32_t id);

    void emit(Command&& c);
    void flush();

    static GfxPipeline* self(RdpgfxClientContext* ctx) {
        return static_cast<GfxPipeline*>(ctx->custom);
    }

    rdpContext* context_;
    CommandQueue& queue_;
    std::function<void()> wake_;
    RdpgfxClientContext* gfx_ = nullptr;

    std::unordered_map<uint16_t, Surface> surfaces_;
    std::vector<Command> batch_;
    bool inFrame_ = false;
    uint32_t frameId_ = 0;
    uint64_t frameStartUs_ = 0;
    uint64_t frameDecodeUs_ = 0;

    CLEAR_CONTEXT* clear_ = nullptr;
    BITMAP_PLANAR_CONTEXT* planar_ = nullptr;
    PROGRESSIVE_CONTEXT* progressive_ = nullptr;
    RFX_CONTEXT* rfx_ = nullptr;
    gdiPalette palette_{};

    GfxStats stats_;
};

} // namespace fastrdp
