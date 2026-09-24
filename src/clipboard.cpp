#include "clipboard.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <freerdp/channels/cliprdr.h>
#include <winpr/user.h>

namespace fastrdp {

namespace {

constexpr auto kServerTimeout = std::chrono::seconds(3);
const char kHtmlFormat[] = "HTML Format";

// Local text mime types, most preferred first.
const char* const kTextMimes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
                                  "STRING", "TEXT"};
// Local image mime types we can turn into a DIB, most preferred first.
const char* const kImageMimes[] = {"image/png", "image/bmp", "image/jpeg", "image/webp"};

bool isText(const char* m) {
    return std::any_of(std::begin(kTextMimes), std::end(kTextMimes),
                       [&](const char* t) { return strcmp(m, t) == 0; });
}
bool isImage(const char* m) {
    return std::any_of(std::begin(kImageMimes), std::end(kImageMimes),
                       [&](const char* t) { return strcmp(m, t) == 0; });
}

// wClipboard's name for UTF-8 text is "text/plain".
const char* convName(const std::string& mime) {
    if (isText(mime.c_str())) return "text/plain";
    return mime.c_str();
}

} // namespace

ClipboardBridge::ClipboardBridge(std::function<void()> wake) : wake_(std::move(wake)) {
    conv_ = ClipboardCreate();
    if (conv_) htmlLocalId_ = ClipboardRegisterFormat(conv_, kHtmlFormat);
}

ClipboardBridge::~ClipboardBridge() {
    detach();
    // SDL holds `this` as clipboard userdata; take it back before we disappear.
    if (ownsLocalClipboard_) SDL_ClearClipboardData();
    if (conv_) ClipboardDestroy(conv_);
}

void ClipboardBridge::attach(CliprdrClientContext* ctx) {
    std::lock_guard lk(mu_);
    ctx_ = ctx;
    ctx->custom = this;
    ctx->MonitorReady = onMonitorReady;
    ctx->ServerCapabilities = onServerCapabilities;
    ctx->ServerFormatList = onServerFormatList;
    ctx->ServerFormatListResponse = onServerFormatListResponse;
    ctx->ServerFormatDataRequest = onServerFormatDataRequest;
    ctx->ServerFormatDataResponse = onServerFormatDataResponse;
}

void ClipboardBridge::detach() {
    std::lock_guard lk(mu_);
    if (ctx_) ctx_->custom = nullptr;
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

UINT ClipboardBridge::onMonitorReady(CliprdrClientContext* ctx, const CLIPRDR_MONITOR_READY*) {
    auto* self = static_cast<ClipboardBridge*>(ctx->custom);
    if (!self) return CHANNEL_RC_OK;

    CLIPRDR_GENERAL_CAPABILITY_SET general{};
    general.capabilitySetType = CB_CAPSTYPE_GENERAL;
    general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    general.version = CB_CAPS_VERSION_2;
    general.generalFlags = CB_USE_LONG_FORMAT_NAMES;
    CLIPRDR_CAPABILITIES caps{};
    caps.cCapabilitiesSets = 1;
    caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);
    const UINT rc = ctx->ClientCapabilities(ctx, &caps);
    if (rc != CHANNEL_RC_OK) return rc;

    // The protocol requires an initial format list; build it from the local clipboard.
    self->post([self] { self->sendLocalFormatList(); });
    return CHANNEL_RC_OK;
}

UINT ClipboardBridge::onServerCapabilities(CliprdrClientContext*, const CLIPRDR_CAPABILITIES*) {
    return CHANNEL_RC_OK;
}

UINT ClipboardBridge::onServerFormatList(CliprdrClientContext* ctx, const CLIPRDR_FORMAT_LIST* list) {
    auto* self = static_cast<ClipboardBridge*>(ctx->custom);
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
    }

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
    auto* self = static_cast<ClipboardBridge*>(ctx->custom);
    if (!self) return CHANNEL_RC_OK;
    const uint32_t fmt = req->requestedFormatId;
    // Reading the local clipboard must happen on the UI thread; answer from there.
    self->post([self, fmt] { self->answerServerRequest(fmt); });
    return CHANNEL_RC_OK;
}

UINT ClipboardBridge::onServerFormatDataResponse(CliprdrClientContext* ctx,
                                                 const CLIPRDR_FORMAT_DATA_RESPONSE* resp) {
    auto* self = static_cast<ClipboardBridge*>(ctx->custom);
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
    for (size_t i = 0; mimes && i < n; i++) {
        text |= isText(mimes[i]);
        html |= strcmp(mimes[i], "text/html") == 0;
        image |= isImage(mimes[i]);
    }
    SDL_free(mimes);

    std::vector<CLIPRDR_FORMAT> formats;
    char htmlName[] = "HTML Format";
    if (text) formats.push_back({CF_UNICODETEXT, nullptr});
    if (html && htmlLocalId_) formats.push_back({htmlLocalId_, htmlName});
    if (image) formats.push_back({CF_DIB, nullptr});

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

// ---------------------------------------------------------------------------------------
// UI thread: remote -> local

void ClipboardBridge::announceServerFormats(uint64_t generation) {
    bool text = false, html = false, image = false;
    {
        std::lock_guard lk(mu_);
        if (generation != serverGeneration_) return; // superseded
        for (const ServerFormat& f : serverFormats_) {
            text |= f.id == CF_UNICODETEXT || f.id == CF_TEXT || f.id == CF_OEMTEXT;
            html |= f.name == kHtmlFormat;
            image |= f.id == CF_DIB || f.id == CF_DIBV5;
        }
    }

    std::vector<const char*> mimes;
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
