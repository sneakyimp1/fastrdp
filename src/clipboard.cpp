#include "clipboard.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/utils/cliprdr_utils.h>
#include <winpr/path.h>
#include <winpr/shell.h>
#include <winpr/user.h>

namespace fastrdp {

namespace {

constexpr auto kServerTimeout = std::chrono::seconds(3);
const char kHtmlFormat[] = "HTML Format";
const char kFileGroupFormat[] = "FileGroupDescriptorW";

// Local text mime types, most preferred first.
const char* const kTextMimes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
                                  "STRING", "TEXT"};
// Local image mime types we can turn into a DIB, most preferred first.
const char* const kImageMimes[] = {"image/png", "image/bmp", "image/jpeg", "image/webp"};

// Local file-list mime types, most preferred first. KDE and most toolkits use text/uri-list;
// GNOME's file manager also wants its own type to paste files rather than their paths.
const char* const kFileMimes[] = {"text/uri-list", "x-special/gnome-copied-files"};

bool isText(const char* m) {
    return std::any_of(std::begin(kTextMimes), std::end(kTextMimes),
                       [&](const char* t) { return strcmp(m, t) == 0; });
}
bool isImage(const char* m) {
    return std::any_of(std::begin(kImageMimes), std::end(kImageMimes),
                       [&](const char* t) { return strcmp(m, t) == 0; });
}

bool isFileList(const char* m) {
    return std::any_of(std::begin(kFileMimes), std::end(kFileMimes),
                       [&](const char* t) { return strcmp(m, t) == 0; });
}

// True if a text/uri-list or x-special/gnome-copied-files payload names only local files
// (a copied web link also travels as text/uri-list, but isn't a file copy).
bool isLocalFileList(const char* data, size_t size) {
    size_t pos = 0, count = 0;
    bool first = true;
    while (pos < size) {
        size_t end = pos;
        while (end < size && data[end] != '\n' && data[end] != '\r' && data[end] != '\0') end++;
        const std::string_view line(data + pos, end - pos);
        pos = end + 1;
        const bool wasFirst = first;
        first = false;
        if (line.empty() || line[0] == '#') continue;
        if (wasFirst && (line == "copy" || line == "cut")) continue; // gnome-copied-files header
        if (line.rfind("file:///", 0) != 0 && line.rfind("file://localhost/", 0) != 0)
            return false;
        count++;
    }
    return count > 0;
}

// wClipboard's name for UTF-8 text is "text/plain".
const char* convName(const std::string& mime) {
    if (isText(mime.c_str())) return "text/plain";
    return mime.c_str();
}

} // namespace

ClipboardBridge::ClipboardBridge(std::function<void()> wake) : wake_(std::move(wake)) {
    conv_ = ClipboardCreate();
    if (conv_) {
        htmlLocalId_ = ClipboardRegisterFormat(conv_, kHtmlFormat);
        // winpr registers these only when it was built with file-list support.
        if (ClipboardGetFormatId(conv_, kFileGroupFormat) &&
            ClipboardGetFormatId(conv_, kFileMimes[0]))
            fileGroupId_ = ClipboardGetFormatId(conv_, kFileGroupFormat);
    }

    outFiles_ = ClipboardCreate();
    if (outFiles_) {
        wClipboardDelegate* d = ClipboardGetDelegate(outFiles_);
        d->custom = this;
        d->ClipboardFileSizeSuccess = fileSizeOk;
        d->ClipboardFileSizeFailure = fileSizeFailed;
        d->ClipboardFileRangeSuccess = fileRangeOk;
        d->ClipboardFileRangeFailure = fileRangeFailed;
    }

    // Starts the FUSE thread that will expose remote files under fuseMount_.
    files_ = cliprdr_file_context_new(this);
    if (!files_) {
        fprintf(stderr, "[clipboard] file helper unavailable; clipboard redirection is off\n");
        return;
    }
    if (fileGroupId_) (void)cliprdr_file_context_set_locally_available(files_, TRUE);
    char name[64];
    snprintf(name, sizeof(name), "com.freerdp.client.cliprdr.%d", int(getpid()));
    if (char* path = GetKnownSubPath(KNOWN_PATH_TEMP, name)) {
        fuseMount_ = path;
        free(path);
    }
}

ClipboardBridge::~ClipboardBridge() {
    detach();
    // SDL holds `this` as clipboard userdata; take it back before we disappear.
    if (ownsLocalClipboard_) SDL_ClearClipboardData();
    if (files_) cliprdr_file_context_free(files_); // unmounts the FUSE folder
    if (outFiles_) ClipboardDestroy(outFiles_);
    if (conv_) ClipboardDestroy(conv_);
}

void ClipboardBridge::attach(CliprdrClientContext* ctx) {
    if (!files_) return;
    std::lock_guard lk(mu_);
    ctx_ = ctx;
    // Sets ctx->custom to the helper and takes the lock / file-contents callbacks; we
    // answer the server's reads of local files ourselves (see answerFileListRequest).
    cliprdr_file_context_init(files_, ctx);
    ctx->ServerFileContentsRequest = onServerFileContentsRequest;
    ctx->MonitorReady = onMonitorReady;
    ctx->ServerCapabilities = onServerCapabilities;
    ctx->ServerFormatList = onServerFormatList;
    ctx->ServerFormatListResponse = onServerFormatListResponse;
    ctx->ServerFormatDataRequest = onServerFormatDataRequest;
    ctx->ServerFormatDataResponse = onServerFormatDataResponse;
}

void ClipboardBridge::detach() {
    std::lock_guard lk(mu_);
    if (ctx_) {
        cliprdr_file_context_uninit(files_, ctx_);
        ctx_->custom = nullptr;
    }
    ctx_ = nullptr;
    serverFormats_.clear();
    if (awaiting_) {
        awaiting_ = false;
        response_.reset();
        responseCv_.notify_all();
    }
}

void ClipboardBridge::post(std::function<void()> fn) {
    {
        std::lock_guard lk(mu_);
        tasks_.push_back(std::move(fn));
    }
    wake_();
}

void ClipboardBridge::service() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard lk(mu_);
            if (tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

// ---------------------------------------------------------------------------------------
// Channel thread

ClipboardBridge* ClipboardBridge::from(CliprdrClientContext* ctx) {
    if (!ctx->custom) return nullptr;
    return static_cast<ClipboardBridge*>(
        cliprdr_file_context_get_context(static_cast<CliprdrFileContext*>(ctx->custom)));
}

UINT ClipboardBridge::onMonitorReady(CliprdrClientContext* ctx, const CLIPRDR_MONITOR_READY*) {
    auto* self = from(ctx);
    if (!self) return CHANNEL_RC_OK;

    CLIPRDR_GENERAL_CAPABILITY_SET general{};
    general.capabilitySetType = CB_CAPSTYPE_GENERAL;
    general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    general.version = CB_CAPS_VERSION_2;
    // The helper adds the file-streaming flags when both sides can do it (server caps
    // arrive before Monitor Ready).
    general.generalFlags = CB_USE_LONG_FORMAT_NAMES | cliprdr_file_context_current_flags(self->files_);
    CLIPRDR_CAPABILITIES caps{};
    caps.cCapabilitiesSets = 1;
    caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);
    const UINT rc = ctx->ClientCapabilities(ctx, &caps);
    if (rc != CHANNEL_RC_OK) return rc;

    // The protocol requires an initial format list; build it from the local clipboard.
    self->post([self] { self->sendLocalFormatList(); });
    return CHANNEL_RC_OK;
}

UINT ClipboardBridge::onServerCapabilities(CliprdrClientContext* ctx,
                                           const CLIPRDR_CAPABILITIES* caps) {
    auto* self = from(ctx);
    if (!self) return CHANNEL_RC_OK;
    UINT32 flags = 0;
    const auto* p = reinterpret_cast<const BYTE*>(caps->capabilitySets);
    for (UINT32 i = 0; p && i < caps->cCapabilitiesSets; i++) {
        const auto* set = reinterpret_cast<const CLIPRDR_CAPABILITY_SET*>(p);
        if (set->capabilitySetType == CB_CAPSTYPE_GENERAL)
            flags = reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET*>(set)->generalFlags;
        p += set->capabilitySetLength;
    }
    (void)cliprdr_file_context_remote_set_flags(self->files_, flags);
    return CHANNEL_RC_OK;
}

UINT ClipboardBridge::onServerFormatList(CliprdrClientContext* ctx, const CLIPRDR_FORMAT_LIST* list) {
    auto* self = from(ctx);
    if (!self) return CHANNEL_RC_OK;

    uint64_t gen = 0;
    {
        std::lock_guard lk(self->mu_);
        self->serverFormats_.clear();
        for (UINT32 i = 0; i < list->numFormats; i++)
            self->serverFormats_.push_back(
                {list->formats[i].formatId,
                 list->formats[i].formatName ? list->formats[i].formatName : ""});
        gen = ++self->serverGeneration_;
        self->localListSinceServerList_ = false;
    }
    // Retires the previous remote file folder and prepares one for this list.
    if (const UINT rc = cliprdr_file_context_notify_new_server_format_list(self->files_); rc)
        return rc;

    CLIPRDR_FORMAT_LIST_RESPONSE resp{};
    resp.common.msgType = CB_FORMAT_LIST_RESPONSE;
    resp.common.msgFlags = CB_RESPONSE_OK;
    const UINT rc = ctx->ClientFormatListResponse(ctx, &resp);

    self->post([self, gen] { self->announceServerFormats(gen); });
    return rc;
}

UINT ClipboardBridge::onServerFormatListResponse(CliprdrClientContext*,
                                                 const CLIPRDR_FORMAT_LIST_RESPONSE*) {
    return CHANNEL_RC_OK;
}

UINT ClipboardBridge::onServerFormatDataRequest(CliprdrClientContext* ctx,
                                                const CLIPRDR_FORMAT_DATA_REQUEST* req) {
    auto* self = from(ctx);
    if (!self) return CHANNEL_RC_OK;
    const uint32_t fmt = req->requestedFormatId;
    // Reading the local clipboard must happen on the UI thread; answer from there.
    self->post([self, fmt] { self->answerServerRequest(fmt); });
    return CHANNEL_RC_OK;
}

UINT ClipboardBridge::onServerFormatDataResponse(CliprdrClientContext* ctx,
                                                 const CLIPRDR_FORMAT_DATA_RESPONSE* resp) {
    auto* self = from(ctx);
    if (!self) return CHANNEL_RC_OK;
    std::lock_guard lk(self->mu_);
    if (!self->awaiting_) return CHANNEL_RC_OK;
    if ((resp->common.msgFlags & CB_RESPONSE_OK) && resp->requestedFormatData)
        self->response_.emplace(resp->requestedFormatData,
                                resp->requestedFormatData + resp->common.dataLen);
    else
        self->response_.reset();
    self->awaiting_ = false;
    self->responseCv_.notify_all();
    return CHANNEL_RC_OK;
}

// ---------------------------------------------------------------------------------------
// UI thread: local -> remote

void ClipboardBridge::onLocalClipboardUpdate(const SDL_ClipboardEvent& ev) {
    if (ev.owner) return; // our own SDL_SetClipboardData
    ownsLocalClipboard_ = false;
    sendLocalFormatList();
}

void ClipboardBridge::sendLocalFormatList() {
    CliprdrClientContext* ctx = nullptr;
    {
        std::lock_guard lk(mu_);
        ctx = ctx_;
    }
    if (!ctx || ownsLocalClipboard_) return;

    size_t n = 0;
    char** mimes = SDL_GetClipboardMimeTypes(&n);
    bool text = false, html = false, image = false;
    const char* fileMime = nullptr;
    for (size_t i = 0; mimes && i < n; i++) {
        text |= isText(mimes[i]);
        html |= strcmp(mimes[i], "text/html") == 0;
        image |= isImage(mimes[i]);
    }
    SDL_free(mimes);
    for (const char* m : kFileMimes)
        if (!fileMime && SDL_HasClipboardData(m)) fileMime = m;

    // Offer files only if the server can stream them and the clipboard really holds local
    // files (browsers put web links under text/uri-list too).
    bool files = false;
    if (fileMime && fileGroupId_ &&
        (cliprdr_file_context_remote_get_flags(files_) & CB_STREAM_FILECLIP_ENABLED)) {
        size_t size = 0;
        void* data = SDL_GetClipboardData(fileMime, &size);
        files = data && isLocalFileList(static_cast<const char*>(data), size);
        SDL_free(data);
    }

    std::vector<CLIPRDR_FORMAT> formats;
    char htmlName[] = "HTML Format";
    char fileGroupName[] = "FileGroupDescriptorW";
    if (files) formats.push_back({fileGroupId_, fileGroupName});
    if (text) formats.push_back({CF_UNICODETEXT, nullptr});
    if (html && htmlLocalId_) formats.push_back({htmlLocalId_, htmlName});
    if (image) formats.push_back({CF_DIB, nullptr});

    // Announcing a client list makes the helper drop the remote file folder. outFiles_ keeps
    // serving until the server asks for a new file list, so a paste already running on the
    // server side isn't cut off.
    (void)cliprdr_file_context_notify_new_client_format_list(files_);
    {
        std::lock_guard lk(mu_);
        localListSinceServerList_ = true;
    }

    CLIPRDR_FORMAT_LIST list{};
    list.common.msgType = CB_FORMAT_LIST;
    list.numFormats = uint32_t(formats.size());
    list.formats = formats.empty() ? nullptr : formats.data();
    (void)ctx->ClientFormatList(ctx, &list);
}

void ClipboardBridge::answerServerRequest(uint32_t formatId) {
    CliprdrClientContext* ctx = nullptr;
    {
        std::lock_guard lk(mu_);
        ctx = ctx_;
    }
    if (!ctx) return;

    auto fail = [&] {
        CLIPRDR_FORMAT_DATA_RESPONSE r{};
        r.common.msgType = CB_FORMAT_DATA_RESPONSE;
        r.common.msgFlags = CB_RESPONSE_FAIL;
        (void)ctx->ClientFormatDataResponse(ctx, &r);
    };

    if (formatId && formatId == fileGroupId_) {
        if (!answerFileListRequest(ctx)) fail();
        return;
    }

    // Pick what to read locally for the requested remote format.
    const char* const* candidates = nullptr;
    size_t ncand = 0;
    static const char* const kHtml[] = {"text/html"};
    if (formatId == CF_UNICODETEXT || formatId == CF_TEXT || formatId == CF_OEMTEXT) {
        candidates = kTextMimes;
        ncand = std::size(kTextMimes);
    } else if (formatId == htmlLocalId_) {
        candidates = kHtml;
        ncand = 1;
    } else if (formatId == CF_DIB || formatId == CF_DIBV5) {
        candidates = kImageMimes;
        ncand = std::size(kImageMimes);
    }
    const char* mime = nullptr;
    for (size_t i = 0; i < ncand && !mime; i++)
        if (SDL_HasClipboardData(candidates[i])) mime = candidates[i];
    if (!mime || !conv_) return fail();

    size_t size = 0;
    void* local = SDL_GetClipboardData(mime, &size);
    if (!local || size == 0) {
        SDL_free(local);
        return fail();
    }

    ClipboardEmpty(conv_);
    const uint32_t srcId = ClipboardRegisterFormat(conv_, convName(mime));
    const bool ok = ClipboardSetData(conv_, srcId, local, uint32_t(size));
    SDL_free(local);
    if (!ok) return fail();

    const uint32_t dstId = formatId == htmlLocalId_ ? ClipboardGetFormatId(conv_, kHtmlFormat) : formatId;
    UINT32 outSize = 0;
    void* out = ClipboardGetData(conv_, dstId, &outSize);
    if (!out) return fail();

    CLIPRDR_FORMAT_DATA_RESPONSE r{};
    r.common.msgType = CB_FORMAT_DATA_RESPONSE;
    r.common.msgFlags = CB_RESPONSE_OK;
    r.common.dataLen = outSize;
    r.requestedFormatData = static_cast<const BYTE*>(out);
    (void)ctx->ClientFormatDataResponse(ctx, &r);
    free(out);
}

// Converts the local file list to a CLIPRDR_FILELIST; outFiles_ keeps the expanded list
// (folders are walked recursively) to serve the server's file-contents requests from.
bool ClipboardBridge::answerFileListRequest(CliprdrClientContext* ctx) {
    const char* mime = nullptr;
    for (const char* m : kFileMimes)
        if (!mime && SDL_HasClipboardData(m)) mime = m;
    if (!mime || !outFiles_) return false;

    size_t size = 0;
    void* local = SDL_GetClipboardData(mime, &size);
    if (!local || size == 0 || !isLocalFileList(static_cast<const char*>(local), size)) {
        SDL_free(local);
        return false;
    }

    // winpr fills in names, sizes and times, and remembers the list for later reads.
    ClipboardLock(outFiles_);
    UINT32 descSize = 0;
    FILEDESCRIPTORW* descs = nullptr;
    if (ClipboardSetData(outFiles_, ClipboardGetFormatId(outFiles_, mime), local, uint32_t(size)))
        descs = static_cast<FILEDESCRIPTORW*>(
            ClipboardGetData(outFiles_, ClipboardGetFormatId(outFiles_, kFileGroupFormat), &descSize));
    ClipboardUnlock(outFiles_);
    SDL_free(local);
    if (!descs) {
        fprintf(stderr, "[clipboard] couldn't read the copied files\n");
        return false;
    }

    BYTE* list = nullptr;
    UINT32 listSize = 0;
    const UINT rc = cliprdr_serialize_file_list_ex(cliprdr_file_context_remote_get_flags(files_),
                                                   descs, descSize / sizeof(FILEDESCRIPTORW),
                                                   &list, &listSize);
    free(descs);
    if (rc != CHANNEL_RC_OK) {
        free(list);
        return false;
    }

    CLIPRDR_FORMAT_DATA_RESPONSE r{};
    r.common.msgType = CB_FORMAT_DATA_RESPONSE;
    r.common.msgFlags = CB_RESPONSE_OK;
    r.common.dataLen = listSize;
    r.requestedFormatData = list;
    (void)ctx->ClientFormatDataResponse(ctx, &r);
    free(list);
    return true;
}

// Channel thread: the server reading a local file (its size, or a chunk of it).
UINT ClipboardBridge::onServerFileContentsRequest(CliprdrClientContext* ctx,
                                                  const CLIPRDR_FILE_CONTENTS_REQUEST* req) {
    auto* self = from(ctx);
    if (!self || !self->outFiles_) return CHANNEL_RC_OK;

    ClipboardLock(self->outFiles_);
    wClipboardDelegate* d = ClipboardGetDelegate(self->outFiles_);
    self->serving_ = ctx;
    UINT rc = ERROR_INVALID_PARAMETER;
    if (req->dwFlags & FILECONTENTS_SIZE) {
        const wClipboardFileSizeRequest q{req->streamId, req->listIndex};
        rc = d->ClientRequestFileSize(d, &q);
    } else if (req->dwFlags & FILECONTENTS_RANGE) {
        const wClipboardFileRangeRequest q{req->streamId, req->listIndex, req->nPositionLow,
                                           req->nPositionHigh, req->cbRequested};
        rc = d->ClientRequestFileRange(d, &q);
    }
    // Without an answer from winpr (stale list, bad index) the server still needs one.
    if (rc != NO_ERROR) self->sendFileContents(req->streamId, nullptr, 0, false);
    self->serving_ = nullptr;
    ClipboardUnlock(self->outFiles_);
    return CHANNEL_RC_OK;
}

UINT ClipboardBridge::fileSizeOk(wClipboardDelegate* d, const wClipboardFileSizeRequest* q,
                                 UINT64 size) {
    BYTE le[8];
    for (int i = 0; i < 8; i++) le[i] = BYTE(size >> (8 * i));
    static_cast<ClipboardBridge*>(d->custom)->sendFileContents(q->streamId, le, sizeof(le), true);
    return NO_ERROR;
}

UINT ClipboardBridge::fileSizeFailed(wClipboardDelegate* d, const wClipboardFileSizeRequest* q, UINT) {
    static_cast<ClipboardBridge*>(d->custom)->sendFileContents(q->streamId, nullptr, 0, false);
    return NO_ERROR;
}

UINT ClipboardBridge::fileRangeOk(wClipboardDelegate* d, const wClipboardFileRangeRequest* q,
                                  const BYTE* data, UINT32 size) {
    static_cast<ClipboardBridge*>(d->custom)->sendFileContents(q->streamId, data, size, true);
    return NO_ERROR;
}

UINT ClipboardBridge::fileRangeFailed(wClipboardDelegate* d, const wClipboardFileRangeRequest* q, UINT) {
    static_cast<ClipboardBridge*>(d->custom)->sendFileContents(q->streamId, nullptr, 0, false);
    return NO_ERROR;
}

void ClipboardBridge::sendFileContents(uint32_t streamId, const BYTE* data, uint32_t size, bool ok) {
    CliprdrClientContext* ctx = serving_;
    if (!ctx) return;
    CLIPRDR_FILE_CONTENTS_RESPONSE r{};
    r.common.msgType = CB_FILECONTENTS_RESPONSE;
    r.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    r.streamId = streamId;
    r.cbRequested = ok ? size : 0;
    r.requestedData = ok ? data : nullptr;
    (void)ctx->ClientFileContentsResponse(ctx, &r);
}

// ---------------------------------------------------------------------------------------
// UI thread: remote -> local

// True when the helper's FUSE folder is actually mounted (it needs /dev/fuse and
// fusermount3); otherwise local apps would be handed paths to an empty directory.
bool ClipboardBridge::remoteFilesLocallyVisible() const {
    if (!fileGroupId_ || fuseMount_.empty() || !cliprdr_file_context_has_local_support(files_))
        return false;
    struct stat mount{}, parent{};
    const std::string up = fuseMount_.substr(0, fuseMount_.find_last_of('/'));
    return stat(fuseMount_.c_str(), &mount) == 0 && stat(up.c_str(), &parent) == 0 &&
           mount.st_dev != parent.st_dev;
}

void ClipboardBridge::announceServerFormats(uint64_t generation) {
    bool text = false, html = false, image = false, files = false;
    {
        std::lock_guard lk(mu_);
        if (generation != serverGeneration_) return; // superseded
        for (const ServerFormat& f : serverFormats_) {
            text |= f.id == CF_UNICODETEXT || f.id == CF_TEXT || f.id == CF_OEMTEXT;
            html |= f.name == kHtmlFormat;
            image |= f.id == CF_DIB || f.id == CF_DIBV5;
            files |= f.name == kFileGroupFormat;
        }
    }
    if (files && !remoteFilesLocallyVisible()) {
        fprintf(stderr, "[clipboard] remote files can't be pasted here: the FUSE folder "
                        "isn't mounted (is fuse3 installed?)\n");
        files = false;
    }

    std::vector<const char*> mimes;
    if (files) mimes.insert(mimes.end(), std::begin(kFileMimes), std::end(kFileMimes));
    if (text) mimes.insert(mimes.end(), std::begin(kTextMimes), std::end(kTextMimes));
    if (html) mimes.push_back("text/html");
    if (image) {
        mimes.push_back("image/png");
        mimes.push_back("image/bmp");
    }
    if (mimes.empty()) return;

    // Data is fetched lazily from the server when a local app actually pastes.
    if (!SDL_SetClipboardData(sdlData, sdlCleanup, this, mimes.data(), mimes.size())) {
        fprintf(stderr, "[clipboard] couldn't take the local clipboard: %s\n", SDL_GetError());
        return;
    }
    provided_.clear();
    ownedGeneration_ = generation;
    ownsLocalClipboard_ = true;
}

const void* ClipboardBridge::sdlData(void* userdata, const char* mime, size_t* size) {
    return static_cast<ClipboardBridge*>(userdata)->provideToLocal(mime, size);
}

void ClipboardBridge::sdlCleanup(void* userdata) {
    auto* self = static_cast<ClipboardBridge*>(userdata);
    self->provided_.clear();
    self->ownsLocalClipboard_ = false;
}

const void* ClipboardBridge::provideToLocal(const char* mime, size_t* size) {
    *size = 0;
    if (!mime || !conv_) return nullptr;
    if (auto it = provided_.find(mime); it != provided_.end()) {
        *size = it->second.size();
        return it->second.data();
    }
    if (isFileList(mime)) return provideFilesToLocal(mime, size);

    // Map the requested local type to the server format we'll fetch.
    uint32_t fetch = 0;
    uint32_t srcConvId = 0;
    {
        std::lock_guard lk(mu_);
        auto has = [&](uint32_t id) {
            return std::any_of(serverFormats_.begin(), serverFormats_.end(),
                               [&](const ServerFormat& f) { return f.id == id; });
        };
        if (isText(mime)) {
            fetch = has(CF_UNICODETEXT) ? CF_UNICODETEXT : has(CF_TEXT) ? CF_TEXT : CF_OEMTEXT;
            srcConvId = fetch;
        } else if (strcmp(mime, "text/html") == 0) {
            for (const ServerFormat& f : serverFormats_)
                if (f.name == kHtmlFormat) fetch = f.id;
            srcConvId = ClipboardGetFormatId(conv_, kHtmlFormat);
        } else if (isImage(mime)) {
            fetch = has(CF_DIB) ? CF_DIB : CF_DIBV5;
            srcConvId = fetch;
        }
    }
    if (!fetch) return nullptr;

    auto data = requestFromServer(fetch);
    if (!data || data->empty()) return nullptr;

    ClipboardEmpty(conv_);
    if (!ClipboardSetData(conv_, srcConvId, data->data(), uint32_t(data->size()))) return nullptr;
    const uint32_t dstId = ClipboardRegisterFormat(conv_, convName(mime));
    UINT32 outSize = 0;
    auto* out = static_cast<uint8_t*>(ClipboardGetData(conv_, dstId, &outSize));
    if (!out) return nullptr;

    std::vector<uint8_t> buf(out, out + outSize);
    free(out);
    if (isText(mime) || strcmp(mime, "text/html") == 0)
        while (!buf.empty() && buf.back() == 0) buf.pop_back(); // local apps don't want the NUL

    auto& stored = provided_[mime];
    stored = std::move(buf);
    *size = stored.size();
    return stored.data();
}

const void* ClipboardBridge::provideFilesToLocal(const char* mime, size_t* size) {
    uint32_t fetch = 0;
    {
        std::lock_guard lk(mu_);
        // A local copy since then made the helper drop the remote folder.
        if (localListSinceServerList_ || ownedGeneration_ != serverGeneration_) return nullptr;
        for (const ServerFormat& f : serverFormats_)
            if (f.name == kFileGroupFormat) fetch = f.id;
    }
    if (!fetch) return nullptr;

    if (fileListGeneration_ != ownedGeneration_) {
        auto data = requestFromServer(fetch);
        if (!data || data->empty()) return nullptr;
        {
            std::lock_guard lk(mu_);
            if (localListSinceServerList_ || ownedGeneration_ != serverGeneration_) return nullptr;
        }
        // Builds the FUSE folder and points conv_'s base path at it.
        if (!cliprdr_file_context_update_server_data(files_, conv_, data->data(), data->size())) {
            fprintf(stderr, "[clipboard] couldn't expose the remote files\n");
            return nullptr;
        }
        fileList_ = std::move(*data);
        fileListGeneration_ = ownedGeneration_;
    }

    ClipboardEmpty(conv_);
    if (!ClipboardSetData(conv_, fileGroupId_, fileList_.data(), uint32_t(fileList_.size())))
        return nullptr;
    UINT32 outSize = 0;
    auto* out = static_cast<uint8_t*>(ClipboardGetData(conv_, ClipboardGetFormatId(conv_, mime), &outSize));
    if (!out) return nullptr;
    std::vector<uint8_t> buf(out, out + outSize);
    free(out);
    while (!buf.empty() && buf.back() == 0) buf.pop_back();

    auto& stored = provided_[mime];
    stored = std::move(buf);
    *size = stored.size();
    return stored.data();
}

std::optional<std::vector<uint8_t>> ClipboardBridge::requestFromServer(uint32_t formatId) {
    std::unique_lock lk(mu_);
    CliprdrClientContext* ctx = ctx_;
    if (!ctx) return std::nullopt;
    awaiting_ = true;
    response_.reset();
    lk.unlock();

    CLIPRDR_FORMAT_DATA_REQUEST req{};
    req.common.msgType = CB_FORMAT_DATA_REQUEST;
    req.requestedFormatId = formatId;
    if (ctx->ClientFormatDataRequest(ctx, &req) != CHANNEL_RC_OK) {
        lk.lock();
        awaiting_ = false;
        return std::nullopt;
    }

    lk.lock();
    if (!responseCv_.wait_for(lk, kServerTimeout, [&] { return !awaiting_; })) {
        awaiting_ = false;
        fprintf(stderr, "[clipboard] server didn't answer the paste request in time\n");
        return std::nullopt;
    }
    return std::move(response_);
}

} // namespace fastrdp
