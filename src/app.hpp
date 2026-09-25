#pragma once

#include "clipboard.hpp"
#include "commands.hpp"
#include "rdp_session.hpp"
#include "renderer.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

namespace fastrdp {

// What the local pointer looks like over the remote desktop.
enum class CursorMode {
    Remote, // the remote PC's pointer image (default)
    Local,  // this computer's default arrow
    Dot,    // a small dot, for precise pointing
    Hidden, // no pointer at all
};

struct AppOptions {
    bool vsync = false;
    bool h264 = true;
    bool hwDecode = true;
    bool printStats = false;
    bool reverseScroll = false;
    CursorMode cursor = CursorMode::Remote;
    ScaleMode scale = ScaleMode::Fit;
    bool userSetSize = false;
    bool userSetNetwork = false;
    bool fullscreen = false;
    // Started by the connection manager: no terminal prompts, GUI certificate dialogs,
    // exit code 2 on bad credentials so the manager can re-ask.
    bool launched = false;
    std::string title;
    std::string password;
    GatewayOptions gateway;
};

// Exit codes understood by the connection manager.
enum ExitCode { kExitOk = 0, kExitError = 1, kExitAuthFailed = 2 };

class App {
public:
    App(AppOptions opts);
    ~App();

    // FreeRDP-style arguments (our own --options already removed).
    int run(int argc, char** argv);

private:
    // One window. In single-window mode it shows the whole desktop; with multiple
    // monitors each fullscreen window shows its monitor's region of the desktop.
    struct Output {
        SDL_Window* window = nullptr;
        SDL_DisplayID display = 0;
        int pixelW = 0, pixelH = 0;
        float density = 1.0f;
        Rect region; // desktop region shown (monitor mode only)
        View view;   // last view used to present, for input mapping
    };

    // A local monitor laid out in remote-desktop pixels.
    struct MonitorLayout {
        SDL_DisplayID display;
        Rect rect; // relative to the bounding box of all monitors
        bool primary;
        uint32_t scalePercent;
    };

    bool initVideo();
    bool createSingleWindow();
    bool createMonitorWindows(const std::vector<MonitorLayout>& layout);
    SDL_Window* makeWindow(const char* title, int w, int h, SDL_WindowFlags extra);
    std::vector<MonitorLayout> computeMonitorLayout(rdpSettings* s) const;
    void applyMonitorLayout(rdpSettings* s, const std::vector<MonitorLayout>& layout);
    Output* outputFor(SDL_WindowID id);
    void presentAll();

    void wake();
    void handleEvent(const SDL_Event& ev);
    void flushMotion();
    void applyCursor(CursorUpdate&& c);
    void showDotCursor();
    void maybeRequestResize(uint64_t nowMs);
    void updateTitle(uint64_t nowMs);
    bool toDesktop(SDL_WindowID id, float x, float y, int& dx, int& dy);
    void toggleFullscreen();
    // Runs fn on the UI thread and returns its result; called from the network thread.
    DWORD runOnUiThread(std::function<DWORD()> fn);
    void servicePrompt(); // UI thread
    void cancelPrompt();
    DWORD showCertificateDialog(const CertPrompt& p);        // UI thread
    DWORD showGatewayMessageDialog(const GatewayMessage& m); // UI thread
    DWORD showSmartcardChooser(const std::vector<std::string>& labels, bool gateway); // UI thread

    AppOptions opts_;
    CommandQueue queue_;
    std::unique_ptr<ClipboardBridge> clipboard_;
    std::unique_ptr<RdpSession> session_;
    Renderer renderer_;

    std::vector<Output> outputs_;
    SDL_Window* window_ = nullptr; // primary output; parent for dialogs
    bool multimon_ = false;
    SDL_GLContext gl_ = nullptr;
    SDL_Cursor* cursor_ = nullptr;
    uint32_t wakeEvent_ = 0;
    std::atomic<bool> wakePending_{false};
    bool quit_ = false;
    bool needPresent_ = true;
    bool fullscreen_ = false;
    float displayScale_ = 1.0f;

    // Pointer motion is coalesced per event batch; only the latest position matters.
    bool motionPending_ = false;
    int motionX_ = 0, motionY_ = 0;

    uint64_t lastResizeMs_ = 0;
    bool resizePending_ = false;
    uint32_t requestedW_ = 0, requestedH_ = 0;

    std::mutex promptMu_;
    std::function<DWORD()>* promptReq_ = nullptr;
    std::promise<DWORD>* promptResult_ = nullptr;

    std::string host_;
    uint64_t statsMs_ = 0;
    uint64_t statsPresents_ = 0, statsFrames_ = 0, statsDecodeUs_ = 0, statsBytes_ = 0,
             statsRenderUs_ = 0;
};

} // namespace fastrdp
