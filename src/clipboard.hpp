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
#include <freerdp/client/client_cliprdr_file.h>
#include <freerdp/client/cliprdr.h>
#include <winpr/clipboard.h>

namespace fastrdp {

// Bridges the RDP clipboard channel (cliprdr) and the local Wayland/X11 clipboard (SDL3).
// Text, HTML, images and files go both ways; winpr's wClipboard does the format conversions
// (CF_UNICODETEXT <-> UTF-8, "HTML Format" <-> text/html, CF_DIB <-> PNG/BMP,
// FileGroupDescriptorW <-> text/uri-list).
//
// Remote files use FreeRDP's cliprdr file helper: they show up locally as a FUSE folder,
// read on demand from the server as the local app copies them. The helper owns cliprdr's
// `custom` pointer (which points back at us) and the lock / file-contents-response PDUs.
// Local files are served to the server by us, from the same winpr file list that produced
// the descriptors we sent, so list indexes can't drift.
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
    static ClipboardBridge* from(CliprdrClientContext* ctx);
    static UINT onMonitorReady(CliprdrClientContext*, const CLIPRDR_MONITOR_READY*);
    static UINT onServerCapabilities(CliprdrClientContext*, const CLIPRDR_CAPABILITIES*);
    static UINT onServerFormatList(CliprdrClientContext*, const CLIPRDR_FORMAT_LIST*);
    static UINT onServerFormatListResponse(CliprdrClientContext*, const CLIPRDR_FORMAT_LIST_RESPONSE*);
    static UINT onServerFormatDataRequest(CliprdrClientContext*, const CLIPRDR_FORMAT_DATA_REQUEST*);
    static UINT onServerFormatDataResponse(CliprdrClientContext*, const CLIPRDR_FORMAT_DATA_RESPONSE*);
    static UINT onServerFileContentsRequest(CliprdrClientContext*, const CLIPRDR_FILE_CONTENTS_REQUEST*);

    // winpr delegate results for those requests (channel thread, under outFiles_'s lock)
    static UINT fileSizeOk(wClipboardDelegate*, const wClipboardFileSizeRequest*, UINT64);
    static UINT fileSizeFailed(wClipboardDelegate*, const wClipboardFileSizeRequest*, UINT);
    static UINT fileRangeOk(wClipboardDelegate*, const wClipboardFileRangeRequest*, const BYTE*, UINT32);
    static UINT fileRangeFailed(wClipboardDelegate*, const wClipboardFileRangeRequest*, UINT);
    void sendFileContents(uint32_t streamId, const BYTE* data, uint32_t size, bool ok);

    // SDL callbacks (UI thread)
    static const void* sdlData(void* userdata, const char* mime, size_t* size);
    static void sdlCleanup(void* userdata);

    void post(std::function<void()> fn);
    void sendLocalFormatList();
    void announceServerFormats(uint64_t generation);
    void answerServerRequest(uint32_t formatId);
    std::optional<std::vector<uint8_t>> requestFromServer(uint32_t formatId);
    const void* provideToLocal(const char* mime, size_t* size);
    const void* provideFilesToLocal(const char* mime, size_t* size);
    bool answerFileListRequest(CliprdrClientContext* ctx);
    bool remoteFilesLocallyVisible() const;

    std::function<void()> wake_;
    CliprdrClientContext* ctx_ = nullptr;
    wClipboard* conv_ = nullptr; // UI thread only
    uint32_t htmlLocalId_ = 0;   // our id for "HTML Format" in the format lists we send
    uint32_t fileGroupId_ = 0;   // our id for "FileGroupDescriptorW"; 0 if winpr lacks file support
    CliprdrFileContext* files_ = nullptr;
    // The local files last offered to the server. Its own wClipboard so other conversions
    // don't invalidate it; shared with the channel thread under ClipboardLock.
    wClipboard* outFiles_ = nullptr;
    CliprdrClientContext* serving_ = nullptr; // set while outFiles_ answers a request
    std::string fuseMount_;      // where the file helper mounts the remote files

    std::mutex mu_;
    std::deque<std::function<void()>> tasks_;
    std::vector<ServerFormat> serverFormats_;
    uint64_t serverGeneration_ = 0;
    // Set once we announce a local format list: the file helper then drops the remote files.
    bool localListSinceServerList_ = false;

    // Outstanding request from the UI thread to the server.
    std::condition_variable responseCv_;
    bool awaiting_ = false;
    std::optional<std::vector<uint8_t>> response_;

    // Data handed to SDL must stay valid until the next callback / cleanup.
    uint64_t ownedGeneration_ = 0;
    std::map<std::string, std::vector<uint8_t>> provided_;
    // Remote file list for ownedGeneration_, fetched once and shared by all file mime types
    // (each fetch would rebuild the FUSE folder under a copy that's already reading it).
    uint64_t fileListGeneration_ = 0;
    std::vector<uint8_t> fileList_;
    bool ownsLocalClipboard_ = false;
};

} // namespace fastrdp
