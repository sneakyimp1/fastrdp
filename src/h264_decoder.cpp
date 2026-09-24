#include "h264_decoder.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
}

namespace fastrdp {

VideoFrame::~VideoFrame() {
    if (frame) av_frame_free(&frame);
}

std::string& VideoConfig::renderNode() {
    static std::string node;
    return node;
}
std::atomic<bool>& VideoConfig::hwDisabled() {
    static std::atomic<bool> v{false};
    return v;
}
std::atomic<bool>& VideoConfig::zeroCopyBroken() {
    static std::atomic<bool> v{false};
    return v;
}

namespace {

std::mutex g_devMutex;
AVBufferRef* g_vaapiDevice = nullptr;
bool g_vaapiTried = false;

AVBufferRef* sharedVaapiDevice() {
    std::lock_guard lk(g_devMutex);
    if (g_vaapiTried) return g_vaapiDevice;
    g_vaapiTried = true;

    const std::string& node = VideoConfig::renderNode();
    const char* dev = node.empty() ? nullptr : node.c_str();
    int err = av_hwdevice_ctx_create(&g_vaapiDevice, AV_HWDEVICE_TYPE_VAAPI, dev, nullptr, 0);
    if (err < 0) {
        char buf[128];
        av_strerror(err, buf, sizeof buf);
        fprintf(stderr, "[video] VAAPI device %s unavailable (%s); using software H.264\n",
                dev ? dev : "(default)", buf);
        g_vaapiDevice = nullptr;
    } else {
        fprintf(stderr, "[video] VAAPI decoding on %s\n", dev ? dev : "(default device)");
    }
    return g_vaapiDevice;
}

AVPixelFormat pickFormat(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    const bool wantHw = ctx->hw_device_ctx != nullptr;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (wantHw && *p == AV_PIX_FMT_VAAPI) return *p;
    // Fall back to the first software format.
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

H264Decoder::H264Decoder() {
    pkt_ = av_packet_alloc();
    open(!VideoConfig::hwDisabled().load());
}

H264Decoder::~H264Decoder() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (pkt_) av_packet_free(&pkt_);
}

bool H264Decoder::open(bool tryHw) {
    if (ctx_) avcodec_free_context(&ctx_);
    hw_ = false;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "[video] no H.264 decoder in libavcodec\n");
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return false;

    // RDP's encoder never reorders frames: every access unit must come out immediately.
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx_->err_recognition = 0;

    AVBufferRef* dev = tryHw ? sharedVaapiDevice() : nullptr;
    if (dev) {
        ctx_->hw_device_ctx = av_buffer_ref(dev);
        ctx_->get_format = pickFormat;
        ctx_->thread_count = 1;
        // Frames stay referenced while queued for the render thread.
        ctx_->extra_hw_frames = 12;
        hw_ = true;
    } else {
        // Slice threads only: frame threading adds a frame of latency per thread.
        ctx_->thread_type = FF_THREAD_SLICE;
        ctx_->thread_count = 0;
    }
    triedHw_ = tryHw;

    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        fprintf(stderr, "[video] avcodec_open2 failed (hw=%d)\n", hw_);
        avcodec_free_context(&ctx_);
        hw_ = false;
        return false;
    }
    return true;
}

VideoFramePtr H264Decoder::decode(const uint8_t* data, size_t size) {
    if (!ctx_ || !data || size == 0) return nullptr;

    padded_.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(padded_.data(), data, size);
    memset(padded_.data() + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    pkt_->data = padded_.data();
    pkt_->size = static_cast<int>(size);

    int err = avcodec_send_packet(ctx_, pkt_);
    av_packet_unref(pkt_);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        if (++consecutiveErrors_ == 3 && hw_) {
            fprintf(stderr, "[video] hardware decode failing, switching to software\n");
            open(false);
        }
        return nullptr;
    }

    AVFrame* last = nullptr;
    for (;;) {
        AVFrame* f = av_frame_alloc();
        err = avcodec_receive_frame(ctx_, f);
        if (err < 0) {
            av_frame_free(&f);
            break;
        }
        if (last) av_frame_free(&last);
        last = f;
    }
    if (!last) return nullptr;
    consecutiveErrors_ = 0;

    auto out = std::make_shared<VideoFrame>();

    if (last->format == AV_PIX_FMT_VAAPI) {
        if (!VideoConfig::zeroCopyBroken().load()) {
            AVFrame* drm = av_frame_alloc();
            drm->format = AV_PIX_FMT_DRM_PRIME;
            // Mapping syncs the VA surface, so decode is complete when this returns.
            if (av_hwframe_map(drm, last, AV_HWFRAME_MAP_READ) == 0) {
                drm->width = last->width;
                drm->height = last->height;
                av_frame_free(&last);
                out->frame = drm;
                return out;
            }
            av_frame_free(&drm);
            fprintf(stderr, "[video] DRM PRIME export failed, downloading frames instead\n");
            VideoConfig::zeroCopyBroken() = true;
        }
        AVFrame* sw = av_frame_alloc();
        if (av_hwframe_transfer_data(sw, last, 0) < 0) {
            av_frame_free(&sw);
            av_frame_free(&last);
            return nullptr;
        }
        sw->width = last->width;
        sw->height = last->height;
        av_frame_free(&last);
        out->frame = sw;
        return out;
    }

    out->frame = last;
    return out;
}

} // namespace fastrdp
