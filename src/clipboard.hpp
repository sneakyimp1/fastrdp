#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>
#include <freerdp/client/cliprdr.h>
#include <winpr/clipboard.h>

namespace fastrdp {

// Bridges the RDP clipboard channel (cliprdr) and the local Wayland/X11 clipboard (SDL3).
// Text, HTML and images go both ways; winpr's wClipboard does the format conversions
// (CF_UNICODETEXT <-> UTF-8, "HTML Format" <-> text/html, CF_DIB <-> PNG/BMP).
//
// Threading: cliprdr callbacks arrive on FreeRDP's channel thread. Anything touching SDL's
// clipboard is queued and run on the UI thread from service(). The one blocking wait is
// the UI thread waiting for the server's data when a local app pastes remote content.
class ClipboardBridge {
public:
    explicit ClipboardBridge(std::function<void()> wake);
    ~ClipboardBridge();

    // Channel thread.
    void attach(CliprdrClientContext* ctx);
    void detach();

    // UI thread.
    void onLocalClipboardUpdate(const SDL_ClipboardEvent& ev);
    void service();

private:
    struct ServerFormat {
        uint32_t id;
        std::string name;
    };

    // cliprdr callbacks (channel thread)
    static UINT onMonitorReady(CliprdrClientContext*, const CLIPRDR_MONITOR_READY*);
    static UINT onServerCapabilities(CliprdrClientContext*, const CLIPRDR_CAPABILITIES*);
    static UINT onServerFormatList(CliprdrClientContext*, const CLIPRDR_FORMAT_LIST*);
    static UINT onServerFormatListResponse(CliprdrClientContext*, const CLIPRDR_FORMAT_LIST_RESPONSE*);
    static UINT onServerFormatDataRequest(CliprdrClientContext*, const CLIPRDR_FORMAT_DATA_REQUEST*);
    static UINT onServerFormatDataResponse(CliprdrClientContext*, const CLIPRDR_FORMAT_DATA_RESPONSE*);

    // SDL callbacks (UI thread)
    static const void* sdlData(void* userdata, const char* mime, size_t* size);
    static void sdlCleanup(void* userdata);

    void post(std::function<void()> fn);
    void sendLocalFormatList();
    void announceServerFormats(uint64_t generation);
    void answerServerRequest(uint32_t formatId);
    std::optional<std::vector<uint8_t>> requestFromServer(uint32_t formatId);
    const void* provideToLocal(const char* mime, size_t* size);

    std::function<void()> wake_;
    CliprdrClientContext* ctx_ = nullptr;
    wClipboard* conv_ = nullptr; // UI thread only
    uint32_t htmlLocalId_ = 0;   // our id for "HTML Format" in the format lists we send

    std::mutex mu_;
    std::deque<std::function<void()>> tasks_;
    std::vector<ServerFormat> serverFormats_;
    uint64_t serverGeneration_ = 0;

    // Outstanding request from the UI thread to the server.
    std::condition_variable responseCv_;
    bool awaiting_ = false;
    std::optional<std::vector<uint8_t>> response_;

    // Data handed to SDL must stay valid until the next callback / cleanup.
    uint64_t ownedGeneration_ = 0;
    std::map<std::string, std::vector<uint8_t>> provided_;
    bool ownsLocalClipboard_ = false;
};

} // namespace fastrdp
