// Render commands produced by the GFX channel thread and executed on the render thread.
//
// The GFX thread does all CPU-side decoding (planar, ClearCodec, progressive, and the
// H.264 bitstream parse/decode) and emits these in wire order. The render thread owns
// every GL object and turns each command into GPU work on the surface textures.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <variant>
#include <vector>

extern "C" {
struct AVFrame;
}

namespace fastrdp {

struct Rect {
    int32_t x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

struct Point {
    int32_t x = 0, y = 0;
};

// Owns an AVFrame reference (hardware VAAPI frame or software YUV420P frame).
struct VideoFrame {
    AVFrame* frame = nullptr;
    ~VideoFrame();
};
using VideoFramePtr = std::shared_ptr<VideoFrame>;

enum class VideoPass : uint8_t {
    Avc420,     // plain 4:2:0 -> RGB for the given rects
    Avc444Luma, // AVC444 main view: writes Y + subsampled chroma into the YUV444 plane
    Avc444ChromaV1,
    Avc444ChromaV2,
};

namespace cmd {

struct ResetGraphics { uint32_t width, height; };
struct CreateSurface { uint16_t id; uint32_t width, height; bool alpha; };
struct DeleteSurface { uint16_t id; };
struct MapSurfaceToOutput { uint16_t id; int32_t x, y; uint32_t targetW, targetH; };
struct SolidFill { uint16_t id; float rgba[4]; std::vector<Rect> rects; };
struct SurfaceToSurface { uint16_t src, dst; Rect rect; std::vector<Point> dests; };
struct SurfaceToCache { uint16_t id; uint16_t slot; Rect rect; };
struct CacheToSurface { uint16_t slot; uint16_t id; std::vector<Point> dests; };
struct EvictCache { uint16_t slot; };

// Tightly packed BGRA pixels (stride = rect.w * 4) to be written at rect.
struct UploadBGRA { uint16_t id; Rect rect; std::vector<uint8_t> pixels; bool keepAlpha; };

// Tightly packed 8-bit alpha (stride = rect.w) replacing the alpha channel in rect.
struct UploadAlpha { uint16_t id; Rect rect; std::vector<uint8_t> alpha; };

struct Video {
    uint16_t id;
    VideoPass pass;
    VideoFramePtr frame;
    std::vector<Rect> rects;
    // For AVC444 the RGB conversion from the YUV444 plane runs after the chroma pass
    // (or after the luma pass when no chroma follows). convertRects is what to convert.
    std::vector<Rect> convertRects;
};

struct EndFrame { uint32_t frameId; uint64_t decodeUs; };

// Legacy (non-GFX) path: pixels from the GDI primary buffer.
struct LegacyUpdate { uint32_t width, height; Rect rect; std::vector<uint8_t> pixels; };

} // namespace cmd

using Command = std::variant<cmd::ResetGraphics, cmd::CreateSurface, cmd::DeleteSurface,
                             cmd::MapSurfaceToOutput, cmd::SolidFill, cmd::SurfaceToSurface,
                             cmd::SurfaceToCache, cmd::CacheToSurface, cmd::EvictCache,
                             cmd::UploadBGRA, cmd::UploadAlpha, cmd::Video, cmd::EndFrame,
                             cmd::LegacyUpdate>;

// Multi-producer (GFX thread, RDP thread), single-consumer (render thread).
class CommandQueue {
public:
    void push(Command&& c) {
        std::lock_guard lk(mu_);
        pending_.push_back(std::move(c));
    }
    void pushBatch(std::vector<Command>&& batch) {
        std::lock_guard lk(mu_);
        if (pending_.empty())
            pending_ = std::move(batch);
        else
            for (auto& c : batch) pending_.push_back(std::move(c));
    }
    std::vector<Command> drain() {
        std::lock_guard lk(mu_);
        std::vector<Command> out;
        out.swap(pending_);
        return out;
    }

private:
    std::mutex mu_;
    std::vector<Command> pending_;
};

} // namespace fastrdp
