#pragma once

#include "commands.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
struct AVCodecContext;
struct AVPacket;
struct AVBufferRef;
}

namespace fastrdp {

// Global video decode configuration, set once at startup / by the renderer.
struct VideoConfig {
    // DRM render node the compositor's EGL display lives on (e.g. /dev/dri/renderD128).
    // VAAPI must decode on the same GPU for zero-copy dmabuf import to work.
    static std::string& renderNode();
    static std::atomic<bool>& hwDisabled();
    // Set by the renderer when importing a dmabuf failed; decoders then download frames.
    static std::atomic<bool>& zeroCopyBroken();
};

// One H.264 decoder per GFX surface (AVC444 feeds both of its views through the same
// decoder, matching how the server encodes them as one stream).
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Decodes one access unit. Returns nullptr when no picture was produced.
    // Hardware frames are returned as AV_PIX_FMT_DRM_PRIME (mapped, synced) unless
    // zero-copy is broken, in which case they're downloaded to NV12.
    VideoFramePtr decode(const uint8_t* data, size_t size);

    bool hardware() const { return hw_; }

private:
    bool open(bool tryHw);

    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    bool hw_ = false;
    bool triedHw_ = false;
    std::vector<uint8_t> padded_;
    uint32_t consecutiveErrors_ = 0;
};

} // namespace fastrdp
