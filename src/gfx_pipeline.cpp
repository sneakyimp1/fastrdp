#include "gfx_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <freerdp/channels/rdpgfx.h>
#include <freerdp/codec/region.h>
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <winpr/stream.h>

namespace fastrdp {

namespace {

uint64_t nowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* capsName(uint32_t v) {
    switch (v) {
    case RDPGFX_CAPVERSION_8: return "8.0";
    case RDPGFX_CAPVERSION_81: return "8.1";
    case RDPGFX_CAPVERSION_10: return "10.0";
    case RDPGFX_CAPVERSION_101: return "10.1";
    case RDPGFX_CAPVERSION_102: return "10.2";
    case RDPGFX_CAPVERSION_103: return "10.3";
    case RDPGFX_CAPVERSION_104: return "10.4";
    case RDPGFX_CAPVERSION_105: return "10.5";
    case RDPGFX_CAPVERSION_106:
    case RDPGFX_CAPVERSION_106_ERR: return "10.6";
    case RDPGFX_CAPVERSION_107: return "10.7";
    default: return "?";
    }
}

} // namespace

GfxPipeline::GfxPipeline(rdpContext* context, CommandQueue& queue, std::function<void()> wake)
    : context_(context), queue_(queue), wake_(std::move(wake)) {
    clear_ = clear_context_new(FALSE);
    planar_ = freerdp_bitmap_planar_context_new(0, 64, 64);
    progressive_ = progressive_context_new(FALSE);
    rfx_ = rfx_context_new(FALSE);
    if (rfx_) rfx_context_set_pixel_format(rfx_, PIXEL_FORMAT_BGRX32);
    palette_.format = PIXEL_FORMAT_BGRX32;
}

GfxPipeline::~GfxPipeline() {
    detach();
    if (clear_) clear_context_free(clear_);
    if (planar_) freerdp_bitmap_planar_context_free(planar_);
    if (progressive_) progressive_context_free(progressive_);
    if (rfx_) rfx_context_free(rfx_);
}

void GfxPipeline::attach(RdpgfxClientContext* gfx) {
    gfx_ = gfx;
    gfx->custom = this;
    gfx->ResetGraphics = cbResetGraphics;
    gfx->StartFrame = cbStartFrame;
    gfx->EndFrame = cbEndFrame;
    gfx->SurfaceCommand = cbSurfaceCommand;
    gfx->DeleteEncodingContext = cbDeleteEncodingContext;
    gfx->CreateSurface = cbCreateSurface;
    gfx->DeleteSurface = cbDeleteSurface;
    gfx->SolidFill = cbSolidFill;
    gfx->SurfaceToSurface = cbSurfaceToSurface;
    gfx->SurfaceToCache = cbSurfaceToCache;
    gfx->CacheToSurface = cbCacheToSurface;
    gfx->CacheImportReply = cbCacheImportReply;
    gfx->EvictCacheEntry = cbEvictCacheEntry;
    gfx->MapSurfaceToOutput = cbMapSurfaceToOutput;
    gfx->MapSurfaceToScaledOutput = cbMapSurfaceToScaledOutput;
    gfx->CapsConfirm = cbCapsConfirm;
}

void GfxPipeline::detach() {
    if (!gfx_) return;
    gfx_->custom = nullptr;
    gfx_->ResetGraphics = nullptr;
    gfx_->StartFrame = nullptr;
    gfx_->EndFrame = nullptr;
    gfx_->SurfaceCommand = nullptr;
    gfx_->DeleteEncodingContext = nullptr;
    gfx_->CreateSurface = nullptr;
    gfx_->DeleteSurface = nullptr;
    gfx_->SolidFill = nullptr;
    gfx_->SurfaceToSurface = nullptr;
    gfx_->SurfaceToCache = nullptr;
    gfx_->CacheToSurface = nullptr;
    gfx_->CacheImportReply = nullptr;
    gfx_->EvictCacheEntry = nullptr;
    gfx_->MapSurfaceToOutput = nullptr;
    gfx_->MapSurfaceToScaledOutput = nullptr;
    gfx_->CapsConfirm = nullptr;
    gfx_ = nullptr;
    surfaces_.clear();
    batch_.clear();
}

// ---------------------------------------------------------------------------------------
// Command batching: everything between StartFrame and EndFrame goes to the renderer as
// one batch so a frame is never presented half-applied.

void GfxPipeline::emit(Command&& c) {
    batch_.push_back(std::move(c));
    if (!inFrame_) flush();
}

void GfxPipeline::flush() {
    if (batch_.empty()) return;
    queue_.pushBatch(std::move(batch_));
    batch_.clear();
    wake_();
}

GfxPipeline::Surface* GfxPipeline::surface(uint32_t id) {
    auto it = surfaces_.find(static_cast<uint16_t>(id));
    return it == surfaces_.end() ? nullptr : &it->second;
}

std::vector<Rect> GfxPipeline::clampRects(const Surface& s, const RECTANGLE_16* rects,
                                          uint32_t n) const {
    std::vector<Rect> out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = std::min<int32_t>(rects[i].left, s.width);
        int32_t t = std::min<int32_t>(rects[i].top, s.height);
        int32_t r = std::min<int32_t>(rects[i].right, s.width);
        int32_t b = std::min<int32_t>(rects[i].bottom, s.height);
        if (r > l && b > t) out.push_back({l, t, r - l, b - t});
    }
    return out;
}

// ---------------------------------------------------------------------------------------

UINT GfxPipeline::cbCapsConfirm(RdpgfxClientContext* ctx, const RDPGFX_CAPS_CONFIRM_PDU* pdu) {
    auto* p = self(ctx);
    if (!p || !pdu || !pdu->capsSet) return CHANNEL_RC_OK;
    p->stats_.capsVersion = pdu->capsSet->version;
    const uint32_t f = pdu->capsSet->flags;
    fprintf(stderr, "[gfx] server confirmed caps %s flags=0x%x%s%s%s\n",
            capsName(pdu->capsSet->version), f,
            (f & RDPGFX_CAPS_FLAG_AVC_DISABLED) ? " AVC_DISABLED" : "",
            (f & RDPGFX_CAPS_FLAG_THINCLIENT) ? " THINCLIENT" : "",
            (f & RDPGFX_CAPS_FLAG_SMALL_CACHE) ? " SMALL_CACHE" : "");
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbResetGraphics(RdpgfxClientContext* ctx,
                                  const RDPGFX_RESET_GRAPHICS_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    fprintf(stderr, "[gfx] reset graphics %ux%u (%u monitors)\n", pdu->width, pdu->height,
            pdu->monitorCount);
    rdpSettings* settings = p->context_->settings;
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, pdu->width);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, pdu->height);
    p->emit(cmd::ResetGraphics{pdu->width, pdu->height});
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbStartFrame(RdpgfxClientContext* ctx, const RDPGFX_START_FRAME_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    p->inFrame_ = true;
    p->frameId_ = pdu->frameId;
    p->frameStartUs_ = nowUs();
    p->frameDecodeUs_ = 0;
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbEndFrame(RdpgfxClientContext* ctx, const RDPGFX_END_FRAME_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    const uint64_t took = nowUs() - p->frameStartUs_;
    p->stats_.frames++;
    p->stats_.decodeUsTotal += took;
    p->batch_.push_back(cmd::EndFrame{pdu->frameId, took});
    p->inFrame_ = false;
    p->flush();
    // Returning here lets the channel send the frame acknowledge right away: all CPU
    // decoding for the frame is finished, the GPU work is cheap and already queued.
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbCreateSurface(RdpgfxClientContext* ctx,
                                  const RDPGFX_CREATE_SURFACE_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    Surface s;
    s.id = pdu->surfaceId;
    s.width = pdu->width;
    s.height = pdu->height;
    s.alpha = pdu->pixelFormat == GFX_PIXEL_FORMAT_ARGB_8888;
    p->surfaces_.erase(s.id);
    auto [it, _] = p->surfaces_.emplace(s.id, std::move(s));

    if (p->planar_) freerdp_bitmap_planar_context_reset(p->planar_, pdu->width, pdu->height);

    // Let the channel track the surface id (used when it tears down).
    if (ctx->SetSurfaceData) (void)ctx->SetSurfaceData(ctx, pdu->surfaceId, &it->second);

    p->emit(cmd::CreateSurface{pdu->surfaceId, pdu->width, pdu->height, it->second.alpha});
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbDeleteSurface(RdpgfxClientContext* ctx,
                                  const RDPGFX_DELETE_SURFACE_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    if (auto* s = p->surface(pdu->surfaceId); s && s->progressiveCtx)
        progressive_delete_surface_context(p->progressive_, pdu->surfaceId);
    p->surfaces_.erase(pdu->surfaceId);
    if (ctx->SetSurfaceData) (void)ctx->SetSurfaceData(ctx, pdu->surfaceId, nullptr);
    p->emit(cmd::DeleteSurface{pdu->surfaceId});
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbMapSurfaceToOutput(RdpgfxClientContext* ctx,
                                       const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    auto* s = p->surface(pdu->surfaceId);
    if (!s) return ERROR_NOT_FOUND;
    p->emit(cmd::MapSurfaceToOutput{pdu->surfaceId, static_cast<int32_t>(pdu->outputOriginX),
                                    static_cast<int32_t>(pdu->outputOriginY), s->width,
                                    s->height});
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbMapSurfaceToScaledOutput(
    RdpgfxClientContext* ctx, const RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    p->emit(cmd::MapSurfaceToOutput{pdu->surfaceId, static_cast<int32_t>(pdu->outputOriginX),
                                    static_cast<int32_t>(pdu->outputOriginY), pdu->targetWidth,
                                    pdu->targetHeight});
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbSolidFill(RdpgfxClientContext* ctx, const RDPGFX_SOLID_FILL_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    auto* s = p->surface(pdu->surfaceId);
    if (!s) return ERROR_NOT_FOUND;
    cmd::SolidFill f;
    f.id = pdu->surfaceId;
    // Surface textures hold BGRA bytes, so channel r=B, g=G, b=R.
    f.rgba[0] = pdu->fillPixel.B / 255.0f;
    f.rgba[1] = pdu->fillPixel.G / 255.0f;
    f.rgba[2] = pdu->fillPixel.R / 255.0f;
    f.rgba[3] = s->alpha ? pdu->fillPixel.XA / 255.0f : 1.0f;
    f.rects = p->clampRects(*s, pdu->fillRects, pdu->fillRectCount);
    if (!f.rects.empty()) p->emit(std::move(f));
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbSurfaceToSurface(RdpgfxClientContext* ctx,
                                     const RDPGFX_SURFACE_TO_SURFACE_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    auto* src = p->surface(pdu->surfaceIdSrc);
    auto* dst = p->surface(pdu->surfaceIdDest);
    if (!src || !dst) return ERROR_NOT_FOUND;
    auto r = p->clampRects(*src, &pdu->rectSrc, 1);
    if (r.empty()) return CHANNEL_RC_OK;
    cmd::SurfaceToSurface c{pdu->surfaceIdSrc, pdu->surfaceIdDest, r[0], {}};
    c.dests.reserve(pdu->destPtsCount);
    for (uint16_t i = 0; i < pdu->destPtsCount; i++)
        c.dests.push_back({pdu->destPts[i].x, pdu->destPts[i].y});
    p->emit(std::move(c));
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbSurfaceToCache(RdpgfxClientContext* ctx,
                                   const RDPGFX_SURFACE_TO_CACHE_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    auto* s = p->surface(pdu->surfaceId);
    if (!s) return ERROR_NOT_FOUND;
    auto r = p->clampRects(*s, &pdu->rectSrc, 1);
    if (r.empty()) return ERROR_INVALID_DATA;
    // The channel only needs a non-null marker to know the slot is occupied.
    if (ctx->SetCacheSlotData)
        (void)ctx->SetCacheSlotData(ctx, pdu->cacheSlot, reinterpret_cast<void*>(uintptr_t{1}));
    p->emit(cmd::SurfaceToCache{pdu->surfaceId, pdu->cacheSlot, r[0]});
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbCacheToSurface(RdpgfxClientContext* ctx,
                                   const RDPGFX_CACHE_TO_SURFACE_PDU* pdu) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    cmd::CacheToSurface c{pdu->cacheSlot, pdu->surfaceId, {}};
    c.dests.reserve(pdu->destPtsCount);
    for (uint16_t i = 0; i < pdu->destPtsCount; i++)
        c.dests.push_back({pdu->destPts[i].x, pdu->destPts[i].y});
    p->emit(std::move(c));
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbCacheImportReply(RdpgfxClientContext* ctx,
                                     const RDPGFX_CACHE_IMPORT_REPLY_PDU* pdu) {
    // We never offer a persistent cache, so there is nothing to import.
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbEvictCacheEntry(RdpgfxClientContext* ctx,
                                    const RDPGFX_EVICT_CACHE_ENTRY_PDU* pdu) {
    auto* p = self(ctx);
    if (ctx->SetCacheSlotData) (void)ctx->SetCacheSlotData(ctx, pdu->cacheSlot, nullptr);
    if (p) p->emit(cmd::EvictCache{pdu->cacheSlot});
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cbDeleteEncodingContext(RdpgfxClientContext* ctx,
                                          const RDPGFX_DELETE_ENCODING_CONTEXT_PDU* pdu) {
    return CHANNEL_RC_OK;
}

// ---------------------------------------------------------------------------------------
// Surface commands

UINT GfxPipeline::cbSurfaceCommand(RdpgfxClientContext* ctx, const RDPGFX_SURFACE_COMMAND* c) {
    auto* p = self(ctx);
    if (!p) return CHANNEL_RC_OK;
    const uint64_t t0 = nowUs();
    UINT rc = p->surfaceCommand(c);
    p->frameDecodeUs_ += nowUs() - t0;
    return rc;
}

UINT GfxPipeline::surfaceCommand(const RDPGFX_SURFACE_COMMAND* c) {
    Surface* s = surface(c->surfaceId);
    if (!s) {
        fprintf(stderr, "[gfx] surface command for unknown surface %u\n", c->surfaceId);
        return ERROR_NOT_FOUND;
    }
    stats_.bytesIn += c->length;
    stats_.lastCodec = c->codecId;

    // WireToSurface1 codecs carry a destination rect that must lie inside the surface.
    // (Progressive arrives via WireToSurface2 with a zero rect.)
    if (c->codecId != RDPGFX_CODECID_CAPROGRESSIVE &&
        c->codecId != RDPGFX_CODECID_CAPROGRESSIVE_V2) {
        if (c->right > s->width || c->bottom > s->height || c->left > c->right ||
            c->top > c->bottom) {
            fprintf(stderr, "[gfx] command rect outside surface %u, ignoring\n", s->id);
            return ERROR_INVALID_DATA;
        }
    }

    switch (c->codecId) {
    case RDPGFX_CODECID_UNCOMPRESSED: return cmdUncompressed(*s, c);
    case RDPGFX_CODECID_PLANAR: return cmdPlanar(*s, c);
    case RDPGFX_CODECID_CLEARCODEC: return cmdClear(*s, c);
    case RDPGFX_CODECID_CAPROGRESSIVE:
    case RDPGFX_CODECID_CAPROGRESSIVE_V2: return cmdProgressive(*s, c);
    case RDPGFX_CODECID_CAVIDEO: return cmdRemoteFx(*s, c);
    case RDPGFX_CODECID_AVC420: return cmdAvc420(*s, c);
    case RDPGFX_CODECID_AVC444:
    case RDPGFX_CODECID_AVC444v2: return cmdAvc444(*s, c);
    case RDPGFX_CODECID_ALPHA: return cmdAlpha(*s, c);
    default:
        fprintf(stderr, "[gfx] unsupported codec 0x%04x\n", c->codecId);
        return CHANNEL_RC_OK;
    }
}

UINT GfxPipeline::cmdUncompressed(Surface& s, const RDPGFX_SURFACE_COMMAND* c) {
    const size_t need = size_t(c->width) * c->height * 4;
    if (c->length < need) return ERROR_INVALID_DATA;
    cmd::UploadBGRA u{s.id,
                      {int32_t(c->left), int32_t(c->top), int32_t(c->width), int32_t(c->height)},
                      std::vector<uint8_t>(c->data, c->data + need),
                      s.alpha && c->format == PIXEL_FORMAT_BGRA32};
    if (!u.keepAlpha)
        for (size_t i = 3; i < need; i += 4) u.pixels[i] = 0xFF;
    emit(std::move(u));
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cmdPlanar(Surface& s, const RDPGFX_SURFACE_COMMAND* c) {
    std::vector<uint8_t> px(size_t(c->width) * c->height * 4);
    if (!freerdp_bitmap_decompress_planar(planar_, c->data, c->length, c->width, c->height,
                                          px.data(), PIXEL_FORMAT_BGRA32, c->width * 4, 0, 0,
                                          c->width, c->height, FALSE)) {
        fprintf(stderr, "[gfx] planar decode failed\n");
        return ERROR_INTERNAL_ERROR;
    }
    emit(cmd::UploadBGRA{s.id,
                         {int32_t(c->left), int32_t(c->top), int32_t(c->width), int32_t(c->height)},
                         std::move(px), s.alpha});
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cmdClear(Surface& s, const RDPGFX_SURFACE_COMMAND* c) {
    std::vector<uint8_t> px(size_t(c->width) * c->height * 4, 0xFF);
    const INT32 rc = clear_decompress(clear_, c->data, c->length, c->width, c->height, px.data(),
                                      PIXEL_FORMAT_BGRX32, c->width * 4, 0, 0, c->width,
                                      c->height, &palette_);
    if (rc < 0) {
        fprintf(stderr, "[gfx] ClearCodec decode failed (%d)\n", rc);
        return ERROR_INTERNAL_ERROR;
    }
    emit(cmd::UploadBGRA{s.id,
                         {int32_t(c->left), int32_t(c->top), int32_t(c->width), int32_t(c->height)},
                         std::move(px), false});
    return CHANNEL_RC_OK;
}

void GfxPipeline::ensureShadow(Surface& s) {
    if (!s.shadow.empty()) return;
    // Progressive/RFX decode in 64x64 tiles; pad so edge tiles never write out of bounds.
    const uint32_t w = (s.width + 63) & ~63u;
    const uint32_t h = (s.height + 63) & ~63u;
    s.shadowStride = w * 4;
    s.shadow.assign(size_t(s.shadowStride) * h, 0);
}

void GfxPipeline::uploadFromShadow(Surface& s, const RECTANGLE_16* rects, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        auto r = clampRects(s, &rects[i], 1);
        if (r.empty()) continue;
        const Rect& rc = r[0];
        std::vector<uint8_t> px(size_t(rc.w) * rc.h * 4);
        for (int32_t y = 0; y < rc.h; y++)
            memcpy(px.data() + size_t(y) * rc.w * 4,
                   s.shadow.data() + size_t(rc.y + y) * s.shadowStride + size_t(rc.x) * 4,
                   size_t(rc.w) * 4);
        emit(cmd::UploadBGRA{s.id, rc, std::move(px), false});
    }
}

UINT GfxPipeline::cmdProgressive(Surface& s, const RDPGFX_SURFACE_COMMAND* c) {
    ensureShadow(s);
    if (!s.progressiveCtx) {
        if (progressive_create_surface_context(progressive_, s.id, s.width, s.height) < 0)
            return ERROR_INTERNAL_ERROR;
        s.progressiveCtx = true;
    }
    REGION16 invalid;
    region16_init(&invalid);
    const INT32 rc =
        progressive_decompress(progressive_, c->data, c->length, s.shadow.data(),
                               PIXEL_FORMAT_BGRX32, s.shadowStride, 0, 0, &invalid, s.id, frameId_);
    if (rc < 0) {
        region16_uninit(&invalid);
        fprintf(stderr, "[gfx] progressive decode failed (%d)\n", rc);
        return ERROR_INTERNAL_ERROR;
    }
    UINT32 n = 0;
    const RECTANGLE_16* rects = region16_rects(&invalid, &n);
    uploadFromShadow(s, rects, n);
    region16_uninit(&invalid);
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cmdRemoteFx(Surface& s, const RDPGFX_SURFACE_COMMAND* c) {
    ensureShadow(s);
    REGION16 invalid;
    region16_init(&invalid);
    const uint32_t shadowH = uint32_t(s.shadow.size() / s.shadowStride);
    if (!rfx_process_message(rfx_, c->data, c->length, c->left, c->top, s.shadow.data(),
                             PIXEL_FORMAT_BGRX32, s.shadowStride, shadowH, &invalid)) {
        region16_uninit(&invalid);
        fprintf(stderr, "[gfx] RemoteFX decode failed\n");
        return ERROR_INTERNAL_ERROR;
    }
    UINT32 n = 0;
    const RECTANGLE_16* rects = region16_rects(&invalid, &n);
    uploadFromShadow(s, rects, n);
    region16_uninit(&invalid);
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cmdAvc420(Surface& s, const RDPGFX_SURFACE_COMMAND* c) {
    auto* bs = static_cast<RDPGFX_AVC420_BITMAP_STREAM*>(c->extra);
    if (!bs) return ERROR_INTERNAL_ERROR;
    if (!s.h264) s.h264 = std::make_unique<H264Decoder>();

    VideoFramePtr frame = s.h264->decode(bs->data, bs->length);
    stats_.hwVideo = s.h264->hardware();
    // A decode failure must not kill the session; the next IDR repairs the picture.
    if (!frame) return CHANNEL_RC_OK;
    stats_.h264Frames++;

    auto rects = clampRects(s, bs->meta.regionRects, bs->meta.numRegionRects);
    if (rects.empty()) return CHANNEL_RC_OK;
    emit(cmd::Video{s.id, VideoPass::Avc420, std::move(frame), rects, {}});
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cmdAvc444(Surface& s, const RDPGFX_SURFACE_COMMAND* c) {
    auto* bs = static_cast<RDPGFX_AVC444_BITMAP_STREAM*>(c->extra);
    if (!bs) return ERROR_INTERNAL_ERROR;
    if (!s.h264) s.h264 = std::make_unique<H264Decoder>();

    const VideoPass chromaPass =
        c->codecId == RDPGFX_CODECID_AVC444 ? VideoPass::Avc444ChromaV1 : VideoPass::Avc444ChromaV2;

    auto& b0 = bs->bitstream[0];
    auto& b1 = bs->bitstream[1];
    auto rects0 = clampRects(s, b0.meta.regionRects, b0.meta.numRegionRects);

    // LC: 0 = luma in stream 1 + chroma in stream 2, 1 = luma only, 2 = chroma only.
    switch (bs->LC) {
    case 0: {
        VideoFramePtr luma = s.h264->decode(b0.data, b0.length);
        VideoFramePtr chroma = s.h264->decode(b1.data, b1.length);
        stats_.hwVideo = s.h264->hardware();
        auto rects1 = clampRects(s, b1.meta.regionRects, b1.meta.numRegionRects);
        std::vector<Rect> convert = rects0;
        convert.insert(convert.end(), rects1.begin(), rects1.end());
        if (luma && chroma) {
            stats_.h264Frames++;
            emit(cmd::Video{s.id, VideoPass::Avc444Luma, std::move(luma), rects0, {}});
            emit(cmd::Video{s.id, chromaPass, std::move(chroma), rects1, std::move(convert)});
        } else if (luma) {
            emit(cmd::Video{s.id, VideoPass::Avc444Luma, std::move(luma), rects0, rects0});
        }
        break;
    }
    case 1: {
        VideoFramePtr luma = s.h264->decode(b0.data, b0.length);
        stats_.hwVideo = s.h264->hardware();
        if (luma) {
            stats_.h264Frames++;
            emit(cmd::Video{s.id, VideoPass::Avc444Luma, std::move(luma), rects0, rects0});
        }
        break;
    }
    case 2: {
        VideoFramePtr chroma = s.h264->decode(b0.data, b0.length);
        stats_.hwVideo = s.h264->hardware();
        if (chroma) {
            stats_.h264Frames++;
            emit(cmd::Video{s.id, chromaPass, std::move(chroma), rects0, rects0});
        }
        break;
    }
    default: break;
    }
    return CHANNEL_RC_OK;
}

UINT GfxPipeline::cmdAlpha(Surface& s, const RDPGFX_SURFACE_COMMAND* c) {
    wStream sbuf;
    wStream* st = Stream_StaticConstInit(&sbuf, c->data, c->length);
    if (Stream_GetRemainingLength(st) < 4) return ERROR_INVALID_DATA;
    UINT16 sig = 0, compressed = 0;
    Stream_Read_UINT16(st, sig);
    Stream_Read_UINT16(st, compressed);
    if (sig != 0x414C) return ERROR_INVALID_DATA;

    const size_t total = size_t(c->width) * c->height;
    std::vector<uint8_t> a(total, 0xFF);
    if (compressed == 0) {
        if (Stream_GetRemainingLength(st) < total) return ERROR_INVALID_DATA;
        Stream_Read(st, a.data(), total);
    } else {
        size_t pos = 0;
        while (pos < total) {
            if (Stream_GetRemainingLength(st) < 2) return ERROR_INVALID_DATA;
            UINT8 val = 0, c8 = 0;
            Stream_Read_UINT8(st, val);
            Stream_Read_UINT8(st, c8);
            UINT32 count = c8;
            if (count >= 0xFF) {
                if (Stream_GetRemainingLength(st) < 2) return ERROR_INVALID_DATA;
                UINT16 c16 = 0;
                Stream_Read_UINT16(st, c16);
                count = c16;
                if (count >= 0xFFFF) {
                    if (Stream_GetRemainingLength(st) < 4) return ERROR_INVALID_DATA;
                    Stream_Read_UINT32(st, count);
                }
            }
            const size_t n = std::min<size_t>(count, total - pos);
            memset(a.data() + pos, val, n);
            pos += n;
        }
    }
    emit(cmd::UploadAlpha{s.id,
                          {int32_t(c->left), int32_t(c->top), int32_t(c->width), int32_t(c->height)},
                          std::move(a)});
    return CHANNEL_RC_OK;
}

} // namespace fastrdp
