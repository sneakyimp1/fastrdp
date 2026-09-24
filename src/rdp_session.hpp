#pragma once

#include "clipboard.hpp"
#include "commands.hpp"
#include "gfx_pipeline.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <freerdp/client.h>
#include <freerdp/client/disp.h>
#include <freerdp/freerdp.h>

namespace fastrdp {

// Remote cursor image, BGRA bytes, handed to the UI thread.
struct CursorUpdate {
    enum class Kind { Image, Hidden, Default } kind = Kind::Default;
    std::vector<uint8_t> bgra;
    uint32_t width = 0, height = 0, hotX = 0, hotY = 0;
};

struct CertPrompt {
    std::string host;
    uint16_t port = 0;
    std::string commonName, subject, issuer, fingerprint;
    std::string oldSubject, oldIssuer, oldFingerprint;
    bool changed = false;  // differs from the certificate we trusted before
    bool gateway = false;  // certificate belongs to the RD Gateway
    bool mismatch = false; // name doesn't match the host we dialed
};

struct GatewayMessage {
    bool consent = false; // consent message (vs. service message)
    bool displayMandatory = false;
    bool consentMandatory = false;
    std::string text;
};

struct GatewayOptions {
    std::string host;
    uint32_t port = 443;
    std::string username, domain, password;
    bool sameCredentials = true;
};

// Owns the FreeRDP instance and its network thread. Input methods are called from the
// UI thread (FreeRDP serialises writes to the transport internally).
class RdpSession {
public:
    RdpSession(CommandQueue& queue, std::function<void()> wake);
    ~RdpSession();

    // Parses FreeRDP-style arguments (/v:host /u:user ...). Returns false and prints help
    // or an error when the process should exit; exitCode is set accordingly.
    bool parseArgs(int argc, char** argv, int& exitCode);
    rdpSettings* settings();

    // Applies our defaults on top of the user's settings (GFX + AVC444, display control...).
    void applyDefaults(bool userSetNetwork, bool enableH264);

    void applyGateway(const GatewayOptions& gw);

    // Certificate decisions: 0 = reject, 1 = trust permanently, 2 = trust this session.
    // Called on the network thread; the handler may block. Without a handler (or when
    // running from a terminal) FreeRDP's terminal prompt is used.
    void setCertificatePrompt(std::function<DWORD(const CertPrompt&)> fn) { certPrompt_ = std::move(fn); }
    // RD Gateway consent/service messages. Return false to abort the connection.
    void setGatewayMessagePrompt(std::function<bool(const GatewayMessage&)> fn) { gatewayPrompt_ = std::move(fn); }
    // When false, never read from the terminal (launched from the connection manager).
    void setInteractiveTerminal(bool v) { interactive_ = v; }
    // True when the connection failed because of bad or missing credentials.
    bool authFailed() const { return authFailed_; }
    // Clipboard redirection is wired to this bridge when the server opens the channel.
    void setClipboard(ClipboardBridge* c) { clipboard_ = c; }

    bool start();
    void stop();

    bool finished() const { return finished_; }
    bool connected() const { return connected_; }
    std::string errorMessage() const;

    // --- Input (UI thread) ---
    void sendKey(uint32_t rdpScancode, bool down, bool repeat);
    void sendMouseMove(int x, int y);
    void sendMouseButton(uint8_t sdlButton, bool down, int x, int y);
    void sendWheel(float dy, float dx);
    void sendFocusIn(bool caps, bool num, bool scroll);
    void releaseAllKeys();

    // Asks the server to change the desktop size (Display Control channel).
    bool requestResize(uint32_t width, uint32_t height, uint32_t scalePercent);
    bool canResize() const { return disp_ != nullptr && dispReady_; }

    std::optional<CursorUpdate> takeCursor();
    GfxStats* gfxStats() { return gfx_ ? &gfx_->stats() : nullptr; }

private:
    struct Context {
        rdpClientContext common;
        RdpSession* self;
    };
    struct Pointer {
        rdpPointer base;
        uint8_t* bgra;
    };

    static BOOL clientNew(freerdp* instance, rdpContext* context);
    static void clientFree(freerdp* instance, rdpContext* context);
    static BOOL authenticate(freerdp* instance, char** username, char** password,
                             char** domain, rdp_auth_reason reason);
    static DWORD verifyCertificate(freerdp* instance, const char* host, UINT16 port,
                                   const char* commonName, const char* subject,
                                   const char* issuer, const char* fingerprint, DWORD flags);
    static DWORD verifyChangedCertificate(freerdp* instance, const char* host, UINT16 port,
                                          const char* commonName, const char* subject,
                                          const char* issuer, const char* fingerprint,
                                          const char* oldSubject, const char* oldIssuer,
                                          const char* oldFingerprint, DWORD flags);
    static BOOL presentGatewayMessage(freerdp* instance, UINT32 type, BOOL isDisplayMandatory,
                                      BOOL isConsentMandatory, size_t length, const WCHAR* message);
    static BOOL preConnect(freerdp* instance);
    static BOOL postConnect(freerdp* instance);
    static void postDisconnect(freerdp* instance);
    static void onChannelConnected(void* ctx, const ChannelConnectedEventArgs* e);
    static void onChannelDisconnected(void* ctx, const ChannelDisconnectedEventArgs* e);
    static UINT onDisplayControlCaps(DispClientContext* disp, UINT32 maxMonitors,
                                     UINT32 factorA, UINT32 factorB);

    static BOOL beginPaint(rdpContext* ctx);
    static BOOL endPaint(rdpContext* ctx);
    static BOOL desktopResize(rdpContext* ctx);

    static BOOL pointerNew(rdpContext* ctx, rdpPointer* ptr);
    static void pointerFree(rdpContext* ctx, rdpPointer* ptr);
    static BOOL pointerSet(rdpContext* ctx, rdpPointer* ptr);
    static BOOL pointerSetNull(rdpContext* ctx);
    static BOOL pointerSetDefault(rdpContext* ctx);
    static BOOL pointerSetPosition(rdpContext* ctx, UINT32 x, UINT32 y);

    static RdpSession* from(rdpContext* ctx) { return reinterpret_cast<Context*>(ctx)->self; }

    void run();
    void setCursor(CursorUpdate&& c);

    CommandQueue& queue_;
    std::function<void()> wake_;
    rdpContext* context_ = nullptr;
    std::unique_ptr<GfxPipeline> gfx_;
    DispClientContext* disp_ = nullptr;
    std::atomic<bool> dispReady_{false};

    std::function<DWORD(const CertPrompt&)> certPrompt_;
    std::function<bool(const GatewayMessage&)> gatewayPrompt_;
    bool interactive_ = true;
    std::atomic<bool> authFailed_{false};
    ClipboardBridge* clipboard_ = nullptr;

    std::thread thread_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> connected_{false};
    mutable std::mutex errMu_;
    std::string error_;

    std::mutex cursorMu_;
    std::optional<CursorUpdate> cursor_;

    std::vector<uint32_t> pressedKeys_;
    uint32_t buttonsDown_ = 0;
    int lastX_ = 0, lastY_ = 0;
    float wheelAccumY_ = 0, wheelAccumX_ = 0;
};

} // namespace fastrdp
