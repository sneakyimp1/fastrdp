#include "rdp_session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <freerdp/channels/channels.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/disp.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/codec/color.h>
#include <freerdp/event.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/graphics.h>
#include <freerdp/input.h>
#include <freerdp/scancode.h>
#include <freerdp/settings.h>
#include <winpr/string.h>
#include <winpr/synch.h>

#include <SDL3/SDL_mouse.h>

namespace fastrdp {

RdpSession::RdpSession(CommandQueue& queue, std::function<void()> wake)
    : queue_(queue), wake_(std::move(wake)) {
    RDP_CLIENT_ENTRY_POINTS ep{};
    ep.Version = RDP_CLIENT_INTERFACE_VERSION;
    ep.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
    ep.ContextSize = sizeof(Context);
    ep.ClientNew = clientNew;
    ep.ClientFree = clientFree;
    context_ = freerdp_client_context_new(&ep);
    if (context_) reinterpret_cast<Context*>(context_)->self = this;
}

RdpSession::~RdpSession() {
    stop();
    if (context_) freerdp_client_context_free(context_);
}

rdpSettings* RdpSession::settings() { return context_ ? context_->settings : nullptr; }

bool RdpSession::parseArgs(int argc, char** argv, int& exitCode) {
    if (!context_) {
        exitCode = 1;
        return false;
    }
    const int status =
        freerdp_client_settings_parse_command_line(context_->settings, argc, argv, FALSE);
    if (status != 0) {
        exitCode = freerdp_client_settings_command_line_status_print(context_->settings, status,
                                                                     argc, argv);
        return false;
    }
    return true;
}

void RdpSession::applyDefaults(bool userSetNetwork, bool enableH264) {
    rdpSettings* s = context_->settings;
    freerdp_settings_set_uint32(s, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
    freerdp_settings_set_uint32(s, FreeRDP_OsMinorType, OSMINORTYPE_NATIVE_WAYLAND);

    // Graphics pipeline with every codec we decode; the server picks the best it has.
    freerdp_settings_set_bool(s, FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_GfxH264, enableH264);
    freerdp_settings_set_bool(s, FreeRDP_GfxAVC444, enableH264);
    freerdp_settings_set_bool(s, FreeRDP_GfxAVC444v2, enableH264);
    freerdp_settings_set_bool(s, FreeRDP_GfxProgressive, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_GfxProgressiveV2, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_GfxPlanar, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_GfxThinClient, FALSE);
    freerdp_settings_set_bool(s, FreeRDP_GfxSmallCache, FALSE);

    // Live resizing.
    freerdp_settings_set_bool(s, FreeRDP_SupportDisplayControl, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_DynamicResolutionUpdate, TRUE);

    // A LAN profile turns on full visual fidelity; autodetect lets the server tune itself.
    if (!userSetNetwork) freerdp_set_connection_type(s, CONNECTION_TYPE_LAN);
    freerdp_settings_set_bool(s, FreeRDP_NetworkAutoDetect, TRUE);
}

void RdpSession::applyGateway(const GatewayOptions& gw) {
    rdpSettings* s = context_->settings;
    if (gw.host.empty()) return;
    freerdp_settings_set_bool(s, FreeRDP_GatewayEnabled, TRUE);
    freerdp_settings_set_uint32(s, FreeRDP_GatewayUsageMethod, TSC_PROXY_MODE_DIRECT);
    freerdp_settings_set_string(s, FreeRDP_GatewayHostname, gw.host.c_str());
    freerdp_settings_set_uint32(s, FreeRDP_GatewayPort, gw.port);
    freerdp_settings_set_bool(s, FreeRDP_GatewayHttpTransport, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_GatewayRpcTransport, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_GatewayUseSameCredentials, gw.sameCredentials);
    if (gw.sameCredentials) {
        freerdp_settings_set_string(s, FreeRDP_GatewayUsername,
                                    freerdp_settings_get_string(s, FreeRDP_Username));
        freerdp_settings_set_string(s, FreeRDP_GatewayDomain,
                                    freerdp_settings_get_string(s, FreeRDP_Domain));
        freerdp_settings_set_string(s, FreeRDP_GatewayPassword,
                                    freerdp_settings_get_string(s, FreeRDP_Password));
    } else {
        if (!gw.username.empty()) freerdp_settings_set_string(s, FreeRDP_GatewayUsername, gw.username.c_str());
        if (!gw.domain.empty()) freerdp_settings_set_string(s, FreeRDP_GatewayDomain, gw.domain.c_str());
        if (!gw.password.empty()) freerdp_settings_set_string(s, FreeRDP_GatewayPassword, gw.password.c_str());
    }
}

bool RdpSession::start() {
    finished_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void RdpSession::stop() {
    if (context_) freerdp_abort_connect_context(context_);
    if (thread_.joinable()) thread_.join();
}

std::string RdpSession::errorMessage() const {
    std::lock_guard lk(errMu_);
    return error_;
}

void RdpSession::run() {
    freerdp* instance = context_->instance;
    if (!freerdp_connect(instance)) {
        const UINT32 err = freerdp_get_last_error(context_);
        switch (err) {
        case FREERDP_ERROR_AUTHENTICATION_FAILED:
        case FREERDP_ERROR_CONNECT_LOGON_FAILURE:
        case FREERDP_ERROR_CONNECT_WRONG_PASSWORD:
        case FREERDP_ERROR_CONNECT_NO_OR_MISSING_CREDENTIALS:
            authFailed_ = true;
            break;
        default:
            break;
        }
        std::lock_guard lk(errMu_);
        error_ = std::string("Connection failed: ") + freerdp_get_last_error_string(err);
        fprintf(stderr, "[rdp] %s (%s)\n", error_.c_str(), freerdp_get_last_error_name(err));
        finished_ = true;
        wake_();
        return;
    }
    connected_ = true;
    fprintf(stderr, "[rdp] connected\n");
    wake_();

    while (!freerdp_shall_disconnect_context(context_)) {
        HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
        const DWORD n = freerdp_get_event_handles(context_, handles, ARRAYSIZE(handles));
        if (n == 0) break;
        const DWORD st = WaitForMultipleObjects(n, handles, FALSE, 100);
        if (st == WAIT_FAILED) break;
        if (!freerdp_check_event_handles(context_)) {
            // A user-initiated close aborts the context; don't try to reconnect then.
            if (freerdp_shall_disconnect_context(context_)) break;
            if (client_auto_reconnect_ex(instance, nullptr)) continue;
            const UINT32 err = freerdp_error_info(instance);
            std::lock_guard lk(errMu_);
            if (err) error_ = freerdp_get_error_info_string(err);
            break;
        }
    }

    freerdp_disconnect(instance);
    connected_ = false;
    finished_ = true;
    fprintf(stderr, "[rdp] disconnected\n");
    wake_();
}

// ---------------------------------------------------------------------------------------
// FreeRDP instance callbacks

BOOL RdpSession::clientNew(freerdp* instance, rdpContext* context) {
    instance->PreConnect = preConnect;
    instance->PostConnect = postConnect;
    instance->PostDisconnect = postDisconnect;
    instance->AuthenticateEx = authenticate;
    instance->VerifyCertificateEx = verifyCertificate;
    instance->VerifyChangedCertificateEx = verifyChangedCertificate;
    instance->PresentGatewayMessage = presentGatewayMessage;
    instance->LogonErrorInfo = client_cli_logon_error_info;
    return TRUE;
}

void RdpSession::clientFree(freerdp* instance, rdpContext* context) {}

BOOL RdpSession::authenticate(freerdp* instance, char** username, char** password,
                              char** domain, rdp_auth_reason reason) {
    RdpSession* self = from(instance->context);
    if (self->interactive_)
        return client_cli_authenticate_ex(instance, username, password, domain, reason);
    // Launched from the connection manager: it already supplied whatever it had. Missing
    // credentials become an auth failure so the manager can ask and relaunch.
    const bool complete = username && *username && password && *password;
    if (!complete) self->authFailed_ = true;
    return complete ? TRUE : FALSE;
}

BOOL RdpSession::presentGatewayMessage(freerdp* instance, UINT32 type, BOOL isDisplayMandatory,
                                       BOOL isConsentMandatory, size_t length,
                                       const WCHAR* message) {
    RdpSession* self = from(instance->context);
    if (self->interactive_ || !self->gatewayPrompt_)
        return client_cli_present_gateway_message(instance, type, isDisplayMandatory,
                                                  isConsentMandatory, length, message);
    if (!isDisplayMandatory && !isConsentMandatory) return TRUE;

    GatewayMessage m;
    m.consent = type == GATEWAY_MESSAGE_CONSENT;
    m.displayMandatory = isDisplayMandatory;
    m.consentMandatory = isConsentMandatory;
    if (message && length) {
        char* utf8 = ConvertWCharNToUtf8Alloc(message, length / sizeof(WCHAR), nullptr);
        if (utf8) {
            m.text = utf8;
            free(utf8);
        }
    }
    const bool ok = self->gatewayPrompt_(m);
    // Declining consent is the user's choice, not a failure worth an error dialog.
    if (!ok) fprintf(stderr, "[rdp] gateway message declined\n");
    return ok ? TRUE : FALSE;
}

DWORD RdpSession::verifyCertificate(freerdp* instance, const char* host, UINT16 port,
                                    const char* commonName, const char* subject,
                                    const char* issuer, const char* fingerprint, DWORD flags) {
    RdpSession* self = from(instance->context);
    if (!self->certPrompt_)
        return client_cli_verify_certificate_ex(instance, host, port, commonName, subject, issuer,
                                                fingerprint, flags);
    CertPrompt p;
    p.host = host ? host : "";
    p.port = port;
    p.commonName = commonName ? commonName : "";
    p.subject = subject ? subject : "";
    p.issuer = issuer ? issuer : "";
    p.fingerprint = fingerprint ? fingerprint : "";
    p.gateway = flags & VERIFY_CERT_FLAG_GATEWAY;
    p.mismatch = flags & VERIFY_CERT_FLAG_MISMATCH;
    return self->certPrompt_(p);
}

DWORD RdpSession::verifyChangedCertificate(freerdp* instance, const char* host, UINT16 port,
                                           const char* commonName, const char* subject,
                                           const char* issuer, const char* fingerprint,
                                           const char* oldSubject, const char* oldIssuer,
                                           const char* oldFingerprint, DWORD flags) {
    RdpSession* self = from(instance->context);
    if (!self->certPrompt_)
        return client_cli_verify_changed_certificate_ex(instance, host, port, commonName, subject,
                                                        issuer, fingerprint, oldSubject, oldIssuer,
                                                        oldFingerprint, flags);
    CertPrompt p;
    p.host = host ? host : "";
    p.port = port;
    p.commonName = commonName ? commonName : "";
    p.subject = subject ? subject : "";
    p.issuer = issuer ? issuer : "";
    p.fingerprint = fingerprint ? fingerprint : "";
    p.oldSubject = oldSubject ? oldSubject : "";
    p.oldIssuer = oldIssuer ? oldIssuer : "";
    p.oldFingerprint = oldFingerprint ? oldFingerprint : "";
    p.changed = true;
    p.gateway = flags & VERIFY_CERT_FLAG_GATEWAY;
    p.mismatch = flags & VERIFY_CERT_FLAG_MISMATCH;
    return self->certPrompt_(p);
}

BOOL RdpSession::preConnect(freerdp* instance) {
    rdpContext* ctx = instance->context;
    if (PubSub_SubscribeChannelConnected(ctx->pubSub, onChannelConnected) < 0) return FALSE;
    if (PubSub_SubscribeChannelDisconnected(ctx->pubSub, onChannelDisconnected) < 0) return FALSE;
    return TRUE;
}

BOOL RdpSession::postConnect(freerdp* instance) {
    rdpContext* ctx = instance->context;

    // GDI stays around only as the fallback for servers that refuse the graphics pipeline.
    if (!gdi_init(instance, PIXEL_FORMAT_BGRX32)) return FALSE;

    rdpPointer pointer{};
    pointer.size = sizeof(Pointer);
    pointer.New = pointerNew;
    pointer.Free = pointerFree;
    pointer.Set = pointerSet;
    pointer.SetNull = pointerSetNull;
    pointer.SetDefault = pointerSetDefault;
    pointer.SetPosition = pointerSetPosition;
    graphics_register_pointer(ctx->graphics, &pointer);

    ctx->update->BeginPaint = beginPaint;
    ctx->update->EndPaint = endPaint;
    ctx->update->DesktopResize = desktopResize;
    return TRUE;
}

void RdpSession::postDisconnect(freerdp* instance) {
    rdpContext* ctx = instance->context;
    PubSub_UnsubscribeChannelConnected(ctx->pubSub, onChannelConnected);
    PubSub_UnsubscribeChannelDisconnected(ctx->pubSub, onChannelDisconnected);
    gdi_free(instance);
}

void RdpSession::onChannelConnected(void* ctx, const ChannelConnectedEventArgs* e) {
    auto* context = static_cast<rdpContext*>(ctx);
    RdpSession* self = from(context);
    if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
        auto* gfx = static_cast<RdpgfxClientContext*>(e->pInterface);
        self->gfx_ = std::make_unique<GfxPipeline>(context, self->queue_, self->wake_);
        self->gfx_->attach(gfx);
        fprintf(stderr, "[rdp] graphics pipeline channel open\n");
    } else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
        if (self->clipboard_)
            self->clipboard_->attach(static_cast<CliprdrClientContext*>(e->pInterface));
    } else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
        self->disp_ = static_cast<DispClientContext*>(e->pInterface);
        self->disp_->custom = self;
        self->disp_->DisplayControlCaps = onDisplayControlCaps;
    } else {
        freerdp_client_OnChannelConnectedEventHandler(ctx, e);
    }
}

void RdpSession::onChannelDisconnected(void* ctx, const ChannelDisconnectedEventArgs* e) {
    auto* context = static_cast<rdpContext*>(ctx);
    RdpSession* self = from(context);
    if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
        if (self->gfx_) self->gfx_->detach();
    } else if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
        if (self->clipboard_) self->clipboard_->detach();
    } else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
        self->dispReady_ = false;
        self->disp_ = nullptr;
    } else {
        freerdp_client_OnChannelDisconnectedEventHandler(ctx, e);
    }
}

UINT RdpSession::onDisplayControlCaps(DispClientContext* disp, UINT32 maxMonitors,
                                      UINT32 factorA, UINT32 factorB) {
    auto* self = static_cast<RdpSession*>(disp->custom);
    self->dispReady_ = true;
    fprintf(stderr, "[rdp] display control ready (dynamic resize enabled)\n");
    self->wake_();
    return CHANNEL_RC_OK;
}

// ---------------------------------------------------------------------------------------
// Legacy (non-GFX) drawing: forward GDI's dirty rects to the renderer.

BOOL RdpSession::beginPaint(rdpContext* ctx) {
    rdpGdi* gdi = ctx->gdi;
    if (!gdi || !gdi->primary || !gdi->primary->hdc->hwnd) return TRUE;
    HGDI_WND hwnd = gdi->primary->hdc->hwnd;
    hwnd->invalid->null = TRUE;
    hwnd->ninvalid = 0;
    return TRUE;
}

BOOL RdpSession::endPaint(rdpContext* ctx) {
    rdpGdi* gdi = ctx->gdi;
    if (!gdi || !gdi->primary || !gdi->primary->hdc->hwnd) return TRUE;
    HGDI_WND hwnd = gdi->primary->hdc->hwnd;
    if (hwnd->invalid->null || hwnd->ninvalid < 1) return TRUE;
    RdpSession* self = from(ctx);
    const uint32_t gw = uint32_t(gdi->width), gh = uint32_t(gdi->height);
    for (INT32 i = 0; i < hwnd->ninvalid; i++) {
        const GDI_RGN& r = hwnd->cinvalid[i];
        const int x = std::clamp<int>(r.x, 0, gw), y = std::clamp<int>(r.y, 0, gh);
        const int w = std::min<int>(r.w, int(gw) - x), h = std::min<int>(r.h, int(gh) - y);
        if (w <= 0 || h <= 0) continue;
        std::vector<uint8_t> px(size_t(w) * h * 4);
        for (int row = 0; row < h; row++)
            memcpy(px.data() + size_t(row) * w * 4,
                   gdi->primary_buffer + size_t(y + row) * gdi->stride + size_t(x) * 4, size_t(w) * 4);
        for (size_t k = 3; k < px.size(); k += 4) px[k] = 0xFF;
        self->queue_.push(cmd::LegacyUpdate{gw, gh, {x, y, w, h}, std::move(px)});
    }
    self->wake_();
    return TRUE;
}

BOOL RdpSession::desktopResize(rdpContext* ctx) {
    rdpGdi* gdi = ctx->gdi;
    if (!gdi) return TRUE;
    return gdi_resize(gdi, freerdp_settings_get_uint32(ctx->settings, FreeRDP_DesktopWidth),
                      freerdp_settings_get_uint32(ctx->settings, FreeRDP_DesktopHeight));
}

// ---------------------------------------------------------------------------------------
// Pointer: the remote cursor becomes a local hardware cursor, so it moves with zero
// network latency.

BOOL RdpSession::pointerNew(rdpContext* ctx, rdpPointer* ptr) {
    auto* p = reinterpret_cast<Pointer*>(ptr);
    const size_t size = size_t(ptr->width) * ptr->height * 4;
    p->bgra = static_cast<uint8_t*>(calloc(1, size ? size : 4));
    if (!p->bgra) return FALSE;
    if (!size) return TRUE;
    gdiPalette palette{};
    palette.format = PIXEL_FORMAT_BGRA32;
    if (ctx->gdi) palette = ctx->gdi->palette;
    return freerdp_image_copy_from_pointer_data(p->bgra, PIXEL_FORMAT_BGRA32, 0, 0, 0,
                                                ptr->width, ptr->height, ptr->xorMaskData,
                                                ptr->lengthXorMask, ptr->andMaskData,
                                                ptr->lengthAndMask, ptr->xorBpp, &palette);
}

void RdpSession::pointerFree(rdpContext* ctx, rdpPointer* ptr) {
    auto* p = reinterpret_cast<Pointer*>(ptr);
    free(p->bgra);
    p->bgra = nullptr;
}

BOOL RdpSession::pointerSet(rdpContext* ctx, rdpPointer* ptr) {
    auto* p = reinterpret_cast<Pointer*>(ptr);
    CursorUpdate c;
    c.kind = CursorUpdate::Kind::Image;
    c.width = ptr->width;
    c.height = ptr->height;
    c.hotX = ptr->xPos;
    c.hotY = ptr->yPos;
    if (c.width == 0 || c.height == 0 || !p->bgra) {
        c.kind = CursorUpdate::Kind::Hidden;
    } else {
        c.bgra.assign(p->bgra, p->bgra + size_t(c.width) * c.height * 4);
    }
    from(ctx)->setCursor(std::move(c));
    return TRUE;
}

BOOL RdpSession::pointerSetNull(rdpContext* ctx) {
    from(ctx)->setCursor({CursorUpdate::Kind::Hidden});
    return TRUE;
}

BOOL RdpSession::pointerSetDefault(rdpContext* ctx) {
    from(ctx)->setCursor({CursorUpdate::Kind::Default});
    return TRUE;
}

BOOL RdpSession::pointerSetPosition(rdpContext* ctx, UINT32 x, UINT32 y) { return TRUE; }

void RdpSession::setCursor(CursorUpdate&& c) {
    {
        std::lock_guard lk(cursorMu_);
        cursor_ = std::move(c);
    }
    wake_();
}

std::optional<CursorUpdate> RdpSession::takeCursor() {
    std::lock_guard lk(cursorMu_);
    std::optional<CursorUpdate> out;
    out.swap(cursor_);
    return out;
}

// ---------------------------------------------------------------------------------------
// Input

void RdpSession::sendKey(uint32_t sc, bool down, bool repeat) {
    if (!connected_ || !sc) return;
    freerdp_input_send_keyboard_event_ex(context_->input, down, repeat, sc);
    auto it = std::find(pressedKeys_.begin(), pressedKeys_.end(), sc);
    if (down && it == pressedKeys_.end()) pressedKeys_.push_back(sc);
    if (!down && it != pressedKeys_.end()) pressedKeys_.erase(it);
}

void RdpSession::releaseAllKeys() {
    if (!connected_) return;
    for (uint32_t sc : pressedKeys_) freerdp_input_send_keyboard_event_ex(context_->input, FALSE, FALSE, sc);
    pressedKeys_.clear();
    for (uint8_t b = 1; b <= 5; b++)
        if (buttonsDown_ & (1u << b)) sendMouseButton(b, false, lastX_, lastY_);
}

void RdpSession::sendMouseMove(int x, int y) {
    if (!connected_) return;
    lastX_ = x;
    lastY_ = y;
    freerdp_input_send_mouse_event(context_->input, PTR_FLAGS_MOVE, UINT16(x), UINT16(y));
}

void RdpSession::sendMouseButton(uint8_t sdlButton, bool down, int x, int y) {
    if (!connected_) return;
    lastX_ = x;
    lastY_ = y;
    if (down) buttonsDown_ |= 1u << sdlButton;
    else buttonsDown_ &= ~(1u << sdlButton);
    switch (sdlButton) {
    case SDL_BUTTON_LEFT:
    case SDL_BUTTON_RIGHT:
    case SDL_BUTTON_MIDDLE: {
        UINT16 flags = sdlButton == SDL_BUTTON_LEFT    ? PTR_FLAGS_BUTTON1
                       : sdlButton == SDL_BUTTON_RIGHT ? PTR_FLAGS_BUTTON2
                                                       : PTR_FLAGS_BUTTON3;
        if (down) flags |= PTR_FLAGS_DOWN;
        freerdp_input_send_mouse_event(context_->input, flags, UINT16(x), UINT16(y));
        break;
    }
    case SDL_BUTTON_X1:
    case SDL_BUTTON_X2: {
        UINT16 flags = sdlButton == SDL_BUTTON_X1 ? PTR_XFLAGS_BUTTON1 : PTR_XFLAGS_BUTTON2;
        if (down) flags |= PTR_XFLAGS_DOWN;
        freerdp_input_send_extended_mouse_event(context_->input, flags, UINT16(x), UINT16(y));
        break;
    }
    default: break;
    }
}

// Wheel deltas are in notches (1.0 = 120 units). Touchpads give fractional notches;
// sending sub-notch rotation lets Windows apps that support it scroll smoothly.
void RdpSession::sendWheel(float dy, float dx) {
    if (!connected_) return;
    auto emit = [this](float& accum, float delta, UINT16 axisFlag, bool invert) {
        accum += delta * 120.0f;
        int units = int(accum);
        if (units == 0) return;
        accum -= float(units);
        if (invert) units = -units;
        while (units != 0) {
            const int step = std::clamp(units, -255, 255);
            UINT16 flags = axisFlag;
            if (step < 0) flags |= PTR_FLAGS_WHEEL_NEGATIVE | UINT16((0x100 - (-step)) & 0xFF);
            else flags |= UINT16(step & 0xFF);
            freerdp_input_send_mouse_event(context_->input, flags, 0, 0);
            units -= step;
        }
    };
    if (dy != 0) emit(wheelAccumY_, dy, PTR_FLAGS_WHEEL, false);
    // RDP's horizontal wheel is positive to the left; SDL's is positive to the right.
    if (dx != 0) emit(wheelAccumX_, dx, PTR_FLAGS_HWHEEL, true);
}

void RdpSession::sendFocusIn(bool caps, bool num, bool scroll) {
    if (!connected_) return;
    UINT16 flags = 0;
    if (caps) flags |= KBD_SYNC_CAPS_LOCK;
    if (num) flags |= KBD_SYNC_NUM_LOCK;
    if (scroll) flags |= KBD_SYNC_SCROLL_LOCK;
    freerdp_input_send_focus_in_event(context_->input, flags);
}

bool RdpSession::requestResize(uint32_t width, uint32_t height, uint32_t scalePercent) {
    if (!canResize()) return false;
    // [MS-RDPEDISP]: width 200..8192 and even, height 200..8192.
    width = std::clamp<uint32_t>(width & ~1u, 200, 8192);
    height = std::clamp<uint32_t>(height, 200, 8192);
    DISPLAY_CONTROL_MONITOR_LAYOUT layout{};
    layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
    layout.Width = width;
    layout.Height = height;
    layout.Orientation = ORIENTATION_LANDSCAPE;
    layout.DesktopScaleFactor = std::clamp<uint32_t>(scalePercent, 100, 500);
    layout.DeviceScaleFactor = scalePercent >= 175 ? 180 : scalePercent >= 125 ? 140 : 100;
    return disp_->SendMonitorLayout(disp_, 1, &layout) == CHANNEL_RC_OK;
}

} // namespace fastrdp
