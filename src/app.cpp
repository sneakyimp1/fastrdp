#include "app.hpp"

#include "h264_decoder.hpp"
#include "keymap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unistd.h>

#include <freerdp/settings.h>

namespace fastrdp {

namespace {

constexpr uint64_t kResizeDebounceMs = 120;

uint32_t scalePercent(float scale) {
    // Windows expects common steps; snap to the nearest 25%.
    const int pct = int(std::lround(scale * 4.0f)) * 25;
    return uint32_t(std::clamp(pct, 100, 500));
}

} // namespace

App::App(AppOptions opts) : opts_(opts) {}

App::~App() {
    cancelPrompt();
    if (session_) session_->stop();
    session_.reset();
    clipboard_.reset(); // releases SDL's clipboard userdata before SDL_Quit
    if (gl_) {
        renderer_.shutdown();
        SDL_GL_DestroyContext(gl_);
    }
    if (cursor_) SDL_DestroyCursor(cursor_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void App::wake() {
    // Coalesce: one pending wake event is enough to drain everything.
    if (wakePending_.exchange(true)) return;
    SDL_Event ev{};
    ev.type = wakeEvent_;
    SDL_PushEvent(&ev);
}

bool App::createWindow() {
    SDL_SetHint(SDL_HINT_APP_ID, "fastrdp");
    SDL_SetHint(SDL_HINT_APP_NAME, "fastrdp");
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    wakeEvent_ = SDL_RegisterEvents(1);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);

    rdpSettings* s = session_->settings();
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    float contentScale = SDL_GetDisplayContentScale(display);
    if (contentScale <= 0) contentScale = 1.0f;

    int logicalW = 1280, logicalH = 800;
    if (opts_.userSetSize) {
        logicalW = int(freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth) / contentScale);
        logicalH = int(freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight) / contentScale);
    } else {
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(display, &usable)) {
            logicalW = usable.w * 8 / 10;
            logicalH = usable.h * 8 / 10;
        }
    }

    const char* hostname = freerdp_settings_get_string(s, FreeRDP_ServerHostname);
    host_ = hostname ? hostname : "";
    const std::string title = "fastrdp — " + (opts_.title.empty() ? host_ : opts_.title);
    window_ = SDL_CreateWindow(title.c_str(), logicalW, logicalH,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window_) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    gl_ = SDL_GL_CreateContext(window_);
    if (!gl_) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_);
    // Swap interval 0 never blocks: the compositor picks up the newest buffer at its next
    // repaint, which is the lowest-latency option on Wayland (no tearing is possible).
    SDL_GL_SetSwapInterval(opts_.vsync ? 1 : 0);

    if (!renderer_.init(SDL_EGL_GetCurrentDisplay())) return false;
    renderer_.setScaleMode(opts_.scale);
    VideoConfig::renderNode() = renderer_.renderNode();
    VideoConfig::hwDisabled() = !opts_.hwDecode;

    SDL_ShowWindow(window_);
    SDL_SyncWindow(window_);
    SDL_GetWindowSizeInPixels(window_, &pixelW_, &pixelH_);
    density_ = SDL_GetWindowPixelDensity(window_);
    displayScale_ = SDL_GetWindowDisplayScale(window_);
    if (density_ <= 0) density_ = 1.0f;
    if (displayScale_ <= 0) displayScale_ = 1.0f;

    fprintf(stderr, "[ui] %s, window %dx%d px, density %.2f, display scale %.2f\n",
            SDL_GetCurrentVideoDriver(), pixelW_, pixelH_, density_, displayScale_);

    if (opts_.fullscreen) toggleFullscreen();
    return true;
}

int App::run(int argc, char** argv) {
    session_ = std::make_unique<RdpSession>(queue_, [this] { wake(); });
    int exitCode = 0;
    if (!session_->parseArgs(argc, argv, exitCode)) return exitCode;

    rdpSettings* s = session_->settings();
    opts_.fullscreen = opts_.fullscreen || freerdp_settings_get_bool(s, FreeRDP_Fullscreen);
    freerdp_settings_set_bool(s, FreeRDP_Fullscreen, FALSE); // we manage the window
    session_->applyDefaults(opts_.userSetNetwork, opts_.h264);
    if (!opts_.password.empty()) {
        freerdp_settings_set_string(s, FreeRDP_Password, opts_.password.c_str());
        std::fill(opts_.password.begin(), opts_.password.end(), '\0');
    }
    session_->applyGateway(opts_.gateway);

    const bool tty = isatty(STDIN_FILENO) && !opts_.launched;
    session_->setInteractiveTerminal(tty);
    if (!tty) session_->setCertificatePrompt([this](const CertPrompt& p) { return askCertificate(p); });

    if (!createWindow()) return 1;

    if (!opts_.userSetSize) {
        freerdp_settings_set_uint32(s, FreeRDP_DesktopWidth, uint32_t(pixelW_) & ~1u);
        freerdp_settings_set_uint32(s, FreeRDP_DesktopHeight, uint32_t(pixelH_));
    }
    const uint32_t pct = scalePercent(displayScale_);
    freerdp_settings_set_uint32(s, FreeRDP_DesktopScaleFactor, pct);
    freerdp_settings_set_uint32(s, FreeRDP_DeviceScaleFactor,
                                pct >= 175 ? 180 : pct >= 125 ? 140 : 100);
    requestedW_ = freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth);
    requestedH_ = freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight);

    clipboard_ = std::make_unique<ClipboardBridge>([this] { wake(); });
    session_->setClipboard(clipboard_.get());
    session_->start();
    statsMs_ = SDL_GetTicks();

    while (!quit_) {
        const uint64_t now = SDL_GetTicks();
        int timeout = 500;
        if (resizePending_)
            timeout = int(std::max<int64_t>(1, int64_t(lastResizeMs_ + kResizeDebounceMs) - int64_t(now)));

        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, timeout)) {
            handleEvent(ev);
            while (SDL_PollEvent(&ev)) handleEvent(ev);
        }
        flushMotion();

        auto cmds = queue_.drain();
        if (renderer_.execute(cmds)) needPresent_ = true;

        if (auto c = session_->takeCursor()) applyCursor(std::move(*c));
        servicePrompt();
        clipboard_->service();

        const uint64_t t = SDL_GetTicks();
        maybeRequestResize(t);

        if (needPresent_) {
            renderer_.present(pixelW_, pixelH_);
            SDL_GL_SwapWindow(window_);
            needPresent_ = false;
        }

        updateTitle(t);

        if (session_->finished()) {
            const std::string err = session_->errorMessage();
            if (session_->authFailed() && opts_.launched) {
                fprintf(stderr, "%s\n", err.c_str());
                exitCode = kExitAuthFailed;
            } else if (!err.empty()) {
                fprintf(stderr, "%s\n", err.c_str());
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "fastrdp", err.c_str(), window_);
                exitCode = 1;
            }
            quit_ = true;
        }
    }
    return exitCode;
}

DWORD App::askCertificate(const CertPrompt& p) {
    std::promise<DWORD> result;
    auto fut = result.get_future();
    {
        std::lock_guard lk(promptMu_);
        promptReq_ = &p;
        promptResult_ = &result;
    }
    wake();
    return fut.get();
}

void App::cancelPrompt() {
    std::lock_guard lk(promptMu_);
    if (!promptResult_) return;
    promptResult_->set_value(0);
    promptReq_ = nullptr;
    promptResult_ = nullptr;
}

void App::servicePrompt() {
    std::lock_guard lk(promptMu_);
    if (!promptReq_) return;
    const CertPrompt& p = *promptReq_;

    std::string msg;
    std::string title;
    if (p.changed) {
        title = "Certificate changed";
        msg = "WARNING: the certificate of " + std::string(p.gateway ? "the gateway " : "") + p.host +
              " has changed since you last connected.\n\n"
              "This can happen after a reinstall or certificate renewal, but it can also mean "
              "someone is intercepting the connection.\n\n"
              "New fingerprint:\n" + p.fingerprint + "\n\nPrevious fingerprint:\n" + p.oldFingerprint +
              "\n\nSubject: " + p.subject + "\nIssuer: " + p.issuer;
    } else {
        title = "Unverified certificate";
        msg = "The identity of " + std::string(p.gateway ? "the gateway " : "") + p.host +
              " can't be verified (self-signed or untrusted certificate).\n\n"
              "Common name: " + p.commonName + "\nIssuer: " + p.issuer +
              "\n\nFingerprint:\n" + p.fingerprint;
    }
    if (p.mismatch) msg += "\n\nThe certificate name does not match the address you connected to.";
    msg += "\n\nConnect anyway?";

    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {0, 2, "Connect once"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Always trust"},
    };
    SDL_MessageBoxData data{};
    data.flags = p.changed ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION;
    data.window = window_;
    data.title = title.c_str();
    data.message = msg.c_str();
    data.numbuttons = SDL_arraysize(buttons);
    data.buttons = buttons;
    int choice = 0;
    if (!SDL_ShowMessageBox(&data, &choice)) choice = 0;
    promptResult_->set_value(DWORD(std::max(choice, 0)));
    promptReq_ = nullptr;
    promptResult_ = nullptr;
}

bool App::toDesktop(float x, float y, int& dx, int& dy) const {
    return renderer_.windowToDesktop(x * density_, y * density_, dx, dy);
}

void App::flushMotion() {
    if (!motionPending_) return;
    motionPending_ = false;
    session_->sendMouseMove(motionX_, motionY_);
}

void App::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_);
    // Like mstsc: Windows key combos go to the remote only while fullscreen.
    SDL_SetWindowKeyboardGrab(window_, fullscreen_);
}

void App::handleEvent(const SDL_Event& ev) {
    if (ev.type == wakeEvent_) {
        wakePending_ = false;
        return;
    }
    switch (ev.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        quit_ = true;
        break;

    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        pixelW_ = ev.window.data1;
        pixelH_ = ev.window.data2;
        lastResizeMs_ = SDL_GetTicks();
        resizePending_ = true;
        needPresent_ = true; // show the old frame scaled while the server catches up
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        density_ = SDL_GetWindowPixelDensity(window_);
        displayScale_ = SDL_GetWindowDisplayScale(window_);
        lastResizeMs_ = SDL_GetTicks();
        resizePending_ = true;
        break;
    case SDL_EVENT_CLIPBOARD_UPDATE:
        if (clipboard_) clipboard_->onLocalClipboardUpdate(ev.clipboard);
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        needPresent_ = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: {
        const SDL_Keymod m = SDL_GetModState();
        session_->sendFocusIn(m & SDL_KMOD_CAPS, m & SDL_KMOD_NUM, m & SDL_KMOD_SCROLL);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        session_->releaseAllKeys();
        break;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = ev.type == SDL_EVENT_KEY_DOWN;
        // Ctrl+Alt+Enter toggles fullscreen locally.
        if (down && ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_CTRL) &&
            (ev.key.mod & SDL_KMOD_ALT)) {
            if (!ev.key.repeat) toggleFullscreen();
            break;
        }
        flushMotion();
        session_->sendKey(sdlToRdpScancode(ev.key.scancode), down, ev.key.repeat);
        break;
    }

    case SDL_EVENT_MOUSE_MOTION: {
        int x = 0, y = 0;
        if (toDesktop(ev.motion.x, ev.motion.y, x, y)) {
            motionPending_ = true;
            motionX_ = x;
            motionY_ = y;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int x = 0, y = 0;
        if (!toDesktop(ev.button.x, ev.button.y, x, y)) break;
        flushMotion();
        session_->sendMouseButton(ev.button.button, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        flushMotion();
        float dy = ev.wheel.y, dx = ev.wheel.x;
        if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            dy = -dy;
            dx = -dx;
        }
        session_->sendWheel(dy, dx);
        break;
    }
    default:
        break;
    }
}

void App::maybeRequestResize(uint64_t nowMs) {
    if (!session_->connected() || !session_->canResize()) return;
    if (resizePending_ && nowMs - lastResizeMs_ < kResizeDebounceMs) return;
    const uint32_t w = uint32_t(pixelW_) & ~1u, h = uint32_t(pixelH_);
    const bool differs = renderer_.desktopWidth() && renderer_.desktopHeight() &&
                         (renderer_.desktopWidth() != w || renderer_.desktopHeight() != h);
    if (resizePending_ || (differs && (requestedW_ != w || requestedH_ != h))) {
        resizePending_ = false;
        if (w == requestedW_ && h == requestedH_ && !differs) return;
        if (session_->requestResize(w, h, scalePercent(displayScale_))) {
            requestedW_ = w;
            requestedH_ = h;
            fprintf(stderr, "[ui] requested remote size %ux%u\n", w, h);
        }
    }
}

void App::applyCursor(CursorUpdate&& c) {
    switch (c.kind) {
    case CursorUpdate::Kind::Hidden:
        SDL_HideCursor();
        return;
    case CursorUpdate::Kind::Default:
        SDL_SetCursor(SDL_GetDefaultCursor());
        SDL_ShowCursor();
        return;
    case CursorUpdate::Kind::Image:
        break;
    }

    SDL_Surface* full = SDL_CreateSurfaceFrom(int(c.width), int(c.height), SDL_PIXELFORMAT_BGRA32,
                                              c.bgra.data(), int(c.width * 4));
    if (!full) return;

    // The cursor image is in remote pixels. Show it at the size it has on the remote
    // screen as displayed here: base image in window points, full-res alternate for HiDPI.
    const float viewScale = renderer_.desktopWidth()
                                ? float(pixelW_) / float(renderer_.desktopWidth())
                                : 1.0f;
    const float toPoints = std::min(viewScale, 1.0f) / density_;
    SDL_Surface* base = full;
    int hotX = int(c.hotX), hotY = int(c.hotY);
    if (std::fabs(toPoints - 1.0f) > 0.01f) {
        const int bw = std::max(1, int(std::lround(c.width * toPoints)));
        const int bh = std::max(1, int(std::lround(c.height * toPoints)));
        base = SDL_ScaleSurface(full, bw, bh, SDL_SCALEMODE_LINEAR);
        if (base) {
            SDL_AddSurfaceAlternateImage(base, full);
            hotX = int(c.hotX * toPoints);
            hotY = int(c.hotY * toPoints);
        } else {
            base = full;
        }
    }
    SDL_Cursor* cur = SDL_CreateColorCursor(base, hotX, hotY);
    if (base != full) SDL_DestroySurface(base);
    SDL_DestroySurface(full);
    if (!cur) return;
    SDL_SetCursor(cur);
    SDL_ShowCursor();
    if (cursor_) SDL_DestroyCursor(cursor_);
    cursor_ = cur;
}

void App::updateTitle(uint64_t nowMs) {
    if (nowMs - statsMs_ < 1000) return;
    const double secs = (nowMs - statsMs_) / 1000.0;
    statsMs_ = nowMs;

    const RenderStats& rs = renderer_.stats();
    GfxStats* gs = session_->gfxStats();
    const uint64_t frames = gs ? gs->frames.load() : 0;
    const uint64_t decodeUs = gs ? gs->decodeUsTotal.load() : 0;
    const uint64_t bytes = gs ? gs->bytesIn.load() : 0;

    const double fps = (frames - statsFrames_) / secs;
    const double presents = (rs.presents - statsPresents_) / secs;
    const double avgDecodeMs =
        frames > statsFrames_ ? (decodeUs - statsDecodeUs_) / 1000.0 / double(frames - statsFrames_) : 0.0;
    const double gpuMs = rs.gfxFrames ? (rs.renderUsTotal - statsRenderUs_) / 1000.0 /
                                            std::max<double>(1.0, double(frames - statsFrames_))
                                      : 0.0;
    const double mbps = (bytes - statsBytes_) * 8.0 / 1e6 / secs;

    statsFrames_ = frames;
    statsPresents_ = rs.presents;
    statsDecodeUs_ = decodeUs;
    statsBytes_ = bytes;
    statsRenderUs_ = rs.renderUsTotal;

    const char* codec = "—";
    if (gs) {
        switch (gs->lastCodec.load()) {
        case 0x000B: codec = "AVC420"; break;
        case 0x000E: codec = "AVC444"; break;
        case 0x000F: codec = "AVC444v2"; break;
        case 0x0009:
        case 0x000D: codec = "Progressive"; break;
        case 0x0008: codec = "ClearCodec"; break;
        case 0x000A: codec = "Planar"; break;
        case 0x0003: codec = "RemoteFX"; break;
        case 0x0000: codec = "Raw"; break;
        }
    }
    const bool hw = gs && gs->hwVideo.load();

    char title[256];
    snprintf(title, sizeof title, "fastrdp — %s — %ux%u · %.0f fps · decode %.1f ms · %s%s · %.1f Mbps",
             (opts_.title.empty() ? host_ : opts_.title).c_str(), renderer_.desktopWidth(), renderer_.desktopHeight(), fps, avgDecodeMs,
             codec, hw ? " (GPU)" : "", mbps);
    SDL_SetWindowTitle(window_, title);
    if (opts_.printStats && session_->connected())
        fprintf(stderr, "[stats] %.0f gfx fps, %.0f presents/s, decode %.2f ms/frame, gpu submit %.2f ms/frame, %s%s, %.1f Mbps\n",
                fps, presents, avgDecodeMs, gpuMs, codec, hw ? " hw" : "", mbps);
}

} // namespace fastrdp
