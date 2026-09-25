#include "app.hpp"

#include "h264_decoder.hpp"
#include "keymap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
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

uint32_t deviceScaleFor(uint32_t pct) { return pct >= 175 ? 180 : pct >= 125 ? 140 : 100; }

// Effective UI scale of a display. On Wayland SDL reports it as pixel density (the
// compositor's fractional scale) with a content scale of 1; on X11 it's the reverse.
float displayScaleOf(SDL_DisplayID id) {
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
    const float density = mode && mode->pixel_density > 0 ? mode->pixel_density : 1.0f;
    const float content = SDL_GetDisplayContentScale(id);
    return density * (content > 0 ? content : 1.0f);
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
    for (auto& o : outputs_) SDL_DestroyWindow(o.window);
    SDL_Quit();
}

void App::wake() {
    // Coalesce: one pending wake event is enough to drain everything.
    if (wakePending_.exchange(true)) return;
    SDL_Event ev{};
    ev.type = wakeEvent_;
    SDL_PushEvent(&ev);
}

// ---------------------------------------------------------------------------------------
// Windows

bool App::initVideo() {
    SDL_SetHint(SDL_HINT_APP_ID, FASTRDP_APP_ID);
    SDL_SetHint(SDL_HINT_APP_NAME, "fastrdp");
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_SCALE_TO_DISPLAY, "0");
    // Keep every monitor window up when another one gains focus.
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
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

    const char* hostname = freerdp_settings_get_string(session_->settings(), FreeRDP_ServerHostname);
    host_ = hostname ? hostname : "";
    return true;
}

// All windows share one GL context, so every surface texture is visible to each of them.
SDL_Window* App::makeWindow(const char* title, int w, int h, SDL_WindowFlags extra) {
    SDL_Window* win = SDL_CreateWindow(
        title, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN | extra);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return nullptr;
    }
    if (!gl_) {
        gl_ = SDL_GL_CreateContext(win);
        if (!gl_) {
            fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
            SDL_DestroyWindow(win);
            return nullptr;
        }
        SDL_GL_MakeCurrent(win, gl_);
        if (!renderer_.init(SDL_EGL_GetCurrentDisplay())) return nullptr;
        renderer_.setScaleMode(opts_.scale);
        VideoConfig::renderNode() = renderer_.renderNode();
        VideoConfig::hwDisabled() = !opts_.hwDecode;
    } else {
        SDL_GL_MakeCurrent(win, gl_);
    }
    // Swap interval 0 never blocks: the compositor picks up the newest buffer at its next
    // repaint, which is the lowest-latency option on Wayland (no tearing is possible).
    SDL_GL_SetSwapInterval(opts_.vsync ? 1 : 0);
    return win;
}

bool App::createSingleWindow() {
    rdpSettings* s = session_->settings();
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const float scale = displayScaleOf(display);

    int logicalW = 1280, logicalH = 800;
    if (opts_.userSetSize) {
        logicalW = int(std::ceil(freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth) / scale));
        logicalH = int(std::ceil(freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight) / scale));
    } else {
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(display, &usable)) {
            logicalW = usable.w * 8 / 10;
            logicalH = usable.h * 8 / 10;
        }
    }

    const std::string title = "fastrdp — " + (opts_.title.empty() ? host_ : opts_.title);
    SDL_Window* win = makeWindow(title.c_str(), logicalW, logicalH, SDL_WINDOW_RESIZABLE);
    if (!win) return false;
    SDL_ShowWindow(win);
    SDL_SyncWindow(win);

    Output o;
    o.window = win;
    o.display = SDL_GetDisplayForWindow(win);
    SDL_GetWindowSizeInPixels(win, &o.pixelW, &o.pixelH);
    o.density = SDL_GetWindowPixelDensity(win);
    if (o.density <= 0) o.density = 1.0f;
    displayScale_ = SDL_GetWindowDisplayScale(win);
    if (displayScale_ <= 0) displayScale_ = 1.0f;
    outputs_.push_back(o);
    window_ = win;

    fprintf(stderr, "[ui] %s, window %dx%d px, density %.2f, display scale %.2f\n",
            SDL_GetCurrentVideoDriver(), o.pixelW, o.pixelH, o.density, displayScale_);

    if (opts_.fullscreen) toggleFullscreen();
    return true;
}

// Lays the selected local monitors out in remote pixels. Each monitor keeps its native
// pixel size; neighbours are packed edge to edge following the local arrangement, which
// keeps the layout gap- and overlap-free even with mixed scale factors.
std::vector<App::MonitorLayout> App::computeMonitorLayout(rdpSettings* s) const {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    std::vector<SDL_DisplayID> selected;
    const uint32_t numIds = freerdp_settings_get_uint32(s, FreeRDP_NumMonitorIds);
    const auto* wanted = static_cast<const UINT32*>(freerdp_settings_get_pointer(s, FreeRDP_MonitorIds));
    for (int i = 0; i < count; i++) {
        bool use = numIds == 0;
        for (uint32_t k = 0; k < numIds && wanted; k++) use |= wanted[k] == uint32_t(i);
        if (use) selected.push_back(ids[i]);
    }
    SDL_free(ids);

    struct Info {
        SDL_DisplayID id;
        SDL_Rect logical;
        int pw, ph;
        float scale;
        int px = 0, py = 0;
    };
    std::vector<Info> mons;
    for (SDL_DisplayID id : selected) {
        Info m{id, {}, 0, 0, 1.0f};
        if (!SDL_GetDisplayBounds(id, &m.logical)) continue;
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
        const float density = mode && mode->pixel_density > 0 ? mode->pixel_density : 1.0f;
        m.pw = int(std::lround((mode ? mode->w : m.logical.w) * density));
        m.ph = int(std::lround((mode ? mode->h : m.logical.h) * density));
        m.scale = displayScaleOf(id);
        mons.push_back(m);
    }

    auto overlapY = [](const SDL_Rect& a, const SDL_Rect& b) { return a.y < b.y + b.h && b.y < a.y + a.h; };
    auto overlapX = [](const SDL_Rect& a, const SDL_Rect& b) { return a.x < b.x + b.w && b.x < a.x + a.w; };
    std::vector<size_t> order(mons.size());
    std::iota(order.begin(), order.end(), 0);

    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return mons[a].logical.x < mons[b].logical.x; });
    for (size_t oi = 0; oi < order.size(); oi++) {
        Info& m = mons[order[oi]];
        for (size_t oj = 0; oj < oi; oj++) {
            const Info& l = mons[order[oj]];
            if (l.logical.x + l.logical.w <= m.logical.x && overlapY(l.logical, m.logical))
                m.px = std::max(m.px, l.px + l.pw);
        }
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return mons[a].logical.y < mons[b].logical.y; });
    for (size_t oi = 0; oi < order.size(); oi++) {
        Info& m = mons[order[oi]];
        for (size_t oj = 0; oj < oi; oj++) {
            const Info& u = mons[order[oj]];
            if (u.logical.y + u.logical.h <= m.logical.y && overlapX(u.logical, m.logical))
                m.py = std::max(m.py, u.py + u.ph);
        }
    }

    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    std::vector<MonitorLayout> out;
    bool havePrimary = false;
    for (const Info& m : mons) {
        const bool p = m.id == primary;
        havePrimary |= p;
        out.push_back({m.id, {m.px, m.py, m.pw, m.ph}, p, scalePercent(m.scale)});
    }
    if (!out.empty() && !havePrimary) out.front().primary = true;
    return out;
}

void App::applyMonitorLayout(rdpSettings* s, const std::vector<MonitorLayout>& layout) {
    std::vector<rdpMonitor> monitors;
    int32_t right = 0, bottom = 0;
    uint32_t primaryPct = 100;
    for (size_t i = 0; i < layout.size(); i++) {
        const MonitorLayout& m = layout[i];
        rdpMonitor r{};
        r.x = m.rect.x;
        r.y = m.rect.y;
        r.width = m.rect.w;
        r.height = m.rect.h;
        r.is_primary = m.primary;
        r.orig_screen = uint32_t(i);
        r.attributes.orientation = ORIENTATION_LANDSCAPE;
        r.attributes.desktopScaleFactor = m.scalePercent;
        r.attributes.deviceScaleFactor = deviceScaleFor(m.scalePercent);
        monitors.push_back(r);
        right = std::max(right, m.rect.x + m.rect.w);
        bottom = std::max(bottom, m.rect.y + m.rect.h);
        if (m.primary) primaryPct = m.scalePercent;
        fprintf(stderr, "[ui] monitor %zu: %dx%d at %d,%d, %u%%%s\n", i, m.rect.w, m.rect.h,
                m.rect.x, m.rect.y, m.scalePercent, m.primary ? " (primary)" : "");
    }
    freerdp_settings_set_monitor_def_array_sorted(s, monitors.data(), monitors.size());
    freerdp_settings_set_bool(s, FreeRDP_UseMultimon, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_SupportMonitorLayoutPdu, TRUE);
    freerdp_settings_set_uint32(s, FreeRDP_DesktopWidth, uint32_t(right));
    freerdp_settings_set_uint32(s, FreeRDP_DesktopHeight, uint32_t(bottom));
    freerdp_settings_set_uint32(s, FreeRDP_DesktopScaleFactor, primaryPct);
    freerdp_settings_set_uint32(s, FreeRDP_DeviceScaleFactor, deviceScaleFor(primaryPct));
}

bool App::createMonitorWindows(const std::vector<MonitorLayout>& layout) {
    const std::string base = "fastrdp — " + (opts_.title.empty() ? host_ : opts_.title);
    for (const MonitorLayout& m : layout) {
        SDL_Window* win = makeWindow(base.c_str(), 640, 480, 0);
        if (!win) return false;
        // Place the window on its display, then go fullscreen there.
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED_DISPLAY(m.display),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(m.display));
        SDL_SetWindowFullscreenMode(win, nullptr);
        SDL_SetWindowFullscreen(win, true);
        SDL_ShowWindow(win);
        SDL_SyncWindow(win);
        SDL_SetWindowKeyboardGrab(win, true);

        Output o;
        o.window = win;
        o.display = m.display;
        o.region = m.rect;
        SDL_GetWindowSizeInPixels(win, &o.pixelW, &o.pixelH);
        o.density = SDL_GetWindowPixelDensity(win);
        if (o.density <= 0) o.density = 1.0f;
        if (m.primary) window_ = win;
        outputs_.push_back(o);
        fprintf(stderr, "[ui] window on display %u: %dx%d px (monitor %dx%d)\n", m.display,
                o.pixelW, o.pixelH, m.rect.w, m.rect.h);
    }
    if (!window_ && !outputs_.empty()) window_ = outputs_.front().window;
    displayScale_ = SDL_GetWindowDisplayScale(window_);
    fullscreen_ = true;
    return !outputs_.empty();
}

App::Output* App::outputFor(SDL_WindowID id) {
    for (auto& o : outputs_)
        if (SDL_GetWindowID(o.window) == id) return &o;
    return nullptr;
}

void App::presentAll() {
    for (auto& o : outputs_) {
        SDL_GL_MakeCurrent(o.window, gl_);
        o.view = multimon_ ? Renderer::regionView(o.pixelW, o.pixelH, o.region)
                           : renderer_.fitView(o.pixelW, o.pixelH);
        renderer_.present(o.pixelW, o.pixelH, o.view);
        SDL_GL_SwapWindow(o.window);
    }
}

// ---------------------------------------------------------------------------------------

int App::run(int argc, char** argv) {
    session_ = std::make_unique<RdpSession>(queue_, [this] { wake(); });
    int exitCode = 0;
    if (!session_->parseArgs(argc, argv, exitCode)) return exitCode;

    rdpSettings* s = session_->settings();
    opts_.fullscreen = opts_.fullscreen || freerdp_settings_get_bool(s, FreeRDP_Fullscreen);
    freerdp_settings_set_bool(s, FreeRDP_Fullscreen, FALSE); // we manage the windows
    multimon_ = freerdp_settings_get_bool(s, FreeRDP_UseMultimon);
    session_->applyDefaults(opts_.userSetNetwork, opts_.h264);
    if (!opts_.password.empty()) {
        freerdp_settings_set_string(s, FreeRDP_Password, opts_.password.c_str());
        std::fill(opts_.password.begin(), opts_.password.end(), '\0');
    }
    session_->applyGateway(opts_.gateway);

    const bool tty = isatty(STDIN_FILENO) && !opts_.launched;
    session_->setInteractiveTerminal(tty);
    if (!tty) {
        session_->setCertificatePrompt([this](const CertPrompt& p) {
            return runOnUiThread([&] { return showCertificateDialog(p); });
        });
        session_->setGatewayMessagePrompt([this](const GatewayMessage& m) {
            return runOnUiThread([&] { return showGatewayMessageDialog(m); }) == 1;
        });
        session_->setSmartcardChooser([this](const std::vector<std::string>& labels, bool gw) {
            // 0 = cancel, otherwise index + 1
            return int(runOnUiThread([&] { return showSmartcardChooser(labels, gw); })) - 1;
        });
    }

    if (!initVideo()) return 1;

    if (multimon_) {
        const auto layout = computeMonitorLayout(s);
        if (layout.empty()) {
            fprintf(stderr, "[ui] no usable monitors for /multimon\n");
            return 1;
        }
        applyMonitorLayout(s, layout);
        if (!createMonitorWindows(layout)) return 1;
    } else {
        if (!createSingleWindow()) return 1;
        const Output& o = outputs_.front();
        if (!opts_.userSetSize) {
            freerdp_settings_set_uint32(s, FreeRDP_DesktopWidth, uint32_t(o.pixelW) & ~1u);
            freerdp_settings_set_uint32(s, FreeRDP_DesktopHeight, uint32_t(o.pixelH));
        }
        const uint32_t pct = scalePercent(displayScale_);
        freerdp_settings_set_uint32(s, FreeRDP_DesktopScaleFactor, pct);
        freerdp_settings_set_uint32(s, FreeRDP_DeviceScaleFactor, deviceScaleFor(pct));
    }
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
            presentAll();
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
                exitCode = kExitError;
            }
            quit_ = true;
        }
    }
    return exitCode;
}

DWORD App::runOnUiThread(std::function<DWORD()> fn) {
    std::promise<DWORD> result;
    auto fut = result.get_future();
    {
        std::lock_guard lk(promptMu_);
        promptReq_ = &fn;
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
    promptResult_->set_value((*promptReq_)());
    promptReq_ = nullptr;
    promptResult_ = nullptr;
}

DWORD App::showCertificateDialog(const CertPrompt& p) {
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
    return DWORD(std::max(choice, 0));
}

DWORD App::showGatewayMessageDialog(const GatewayMessage& m) {
    std::string msg = m.text;
    if (m.consentMandatory) msg += "\n\nDo you accept these terms?";
    const SDL_MessageBoxButtonData consent[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Decline"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Accept"},
    };
    const SDL_MessageBoxButtonData info[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "OK"},
    };
    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.window = window_;
    data.title = m.consent ? "Remote Desktop Gateway — consent required" : "Remote Desktop Gateway";
    data.message = msg.c_str();
    data.numbuttons = m.consentMandatory ? SDL_arraysize(consent) : SDL_arraysize(info);
    data.buttons = m.consentMandatory ? consent : info;
    int choice = 0;
    if (!SDL_ShowMessageBox(&data, &choice)) choice = 0;
    return choice == 1 ? 1 : 0;
}

DWORD App::showSmartcardChooser(const std::vector<std::string>& labels, bool gateway) {
    // Message boxes lay buttons out in a row, so keep them to a handful.
    const size_t n = std::min<size_t>(labels.size(), 6);
    std::vector<SDL_MessageBoxButtonData> buttons;
    buttons.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"});
    for (size_t i = 0; i < n; i++)
        buttons.push_back({i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0u, int(i + 1),
                           labels[i].c_str()});
    std::string msg = gateway ? "Choose the smart card certificate for the gateway:"
                              : "Choose the smart card certificate to sign in with:";
    for (size_t i = 0; i < n; i++) msg += "\n  " + std::to_string(i + 1) + ". " + labels[i];
    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.window = window_;
    data.title = "Smart card";
    data.message = msg.c_str();
    data.numbuttons = int(buttons.size());
    data.buttons = buttons.data();
    int choice = 0;
    if (!SDL_ShowMessageBox(&data, &choice)) choice = 0;
    return DWORD(std::max(choice, 0));
}

bool App::toDesktop(SDL_WindowID id, float x, float y, int& dx, int& dy) {
    const Output* o = outputFor(id);
    if (!o) return false;
    return o->view.toDesktop(x * o->density, y * o->density, renderer_.desktopWidth(),
                             renderer_.desktopHeight(), dx, dy);
}

void App::flushMotion() {
    if (!motionPending_) return;
    motionPending_ = false;
    session_->sendMouseMove(motionX_, motionY_);
}

void App::toggleFullscreen() {
    if (multimon_) return; // every monitor window is already fullscreen
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
        if (Output* o = outputFor(ev.window.windowID)) {
            o->pixelW = ev.window.data1;
            o->pixelH = ev.window.data2;
        }
        if (!multimon_) {
            lastResizeMs_ = SDL_GetTicks();
            resizePending_ = true;
        }
        needPresent_ = true; // show the old frame scaled while the server catches up
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        if (Output* o = outputFor(ev.window.windowID)) {
            o->density = SDL_GetWindowPixelDensity(o->window);
            if (!multimon_) {
                displayScale_ = SDL_GetWindowDisplayScale(o->window);
                lastResizeMs_ = SDL_GetTicks();
                resizePending_ = true;
            }
        }
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
        // Moving between our own monitor windows isn't a real focus loss.
        if (!SDL_GetKeyboardFocus()) session_->releaseAllKeys();
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
        if (toDesktop(ev.motion.windowID, ev.motion.x, ev.motion.y, x, y)) {
            motionPending_ = true;
            motionX_ = x;
            motionY_ = y;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int x = 0, y = 0;
        if (!toDesktop(ev.button.windowID, ev.button.x, ev.button.y, x, y)) break;
        flushMotion();
        session_->sendMouseButton(ev.button.button, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        flushMotion();
        float dy = ev.wheel.y, dx = ev.wheel.x;
        // SDL reports FLIPPED when the local desktop uses natural scrolling; undo that so
        // the remote PC gets the physical direction, then apply the connection's setting.
        if ((ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) != opts_.reverseScroll) {
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
    // Multi-monitor layouts and explicit /size resolutions stay fixed; the view scales.
    if (multimon_ || opts_.userSetSize || outputs_.empty()) return;
    if (!session_->connected() || !session_->canResize()) return;
    if (resizePending_ && nowMs - lastResizeMs_ < kResizeDebounceMs) return;
    const Output& o = outputs_.front();
    const uint32_t w = uint32_t(o.pixelW) & ~1u, h = uint32_t(o.pixelH);
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
    if (!full || outputs_.empty()) {
        SDL_DestroySurface(full);
        return;
    }

    // The cursor image is in remote pixels. Show it at the size it has on the remote
    // screen as displayed here: base image in window points, full-res alternate for HiDPI.
    const Output& o = outputs_.front();
    const float toPoints = std::min(o.view.scaleX, 1.0f) / o.density;
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
    for (auto& o : outputs_) SDL_SetWindowTitle(o.window, title);
    if (opts_.printStats && session_->connected())
        fprintf(stderr, "[stats] %.0f gfx fps, %.0f presents/s, decode %.2f ms/frame, gpu submit %.2f ms/frame, %s%s, %.1f Mbps\n",
                fps, presents, avgDecodeMs, gpuMs, codec, hw ? " hw" : "", mbps);
}

} // namespace fastrdp
