#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace fastrdp {

struct Bookmark {
    enum class Display { Window, Fullscreen, Fixed };

    QString id;
    QString name;
    QString host;
    int port = 3389;
    QString username;
    QString domain;
    bool savePassword = false;

    Display display = Display::Window;
    int width = 1920;
    int height = 1080;
    QString scale = QStringLiteral("fit"); // fit | stretch | native

    QString network = QStringLiteral("auto");
    bool h264 = true;
    bool gpuDecode = true;
    bool vsync = false;

    QString audio = QStringLiteral("local"); // local | remote | off
    bool clipboard = true;
    bool microphone = false;
    bool shareHome = false;

    bool gateway = false;
    QString gatewayHost;
    int gatewayPort = 443;
    bool gatewaySameCreds = true;
    QString gatewayUser;
    QString gatewayDomain;
    bool gatewaySavePassword = false;

    QString extraArgs;
    QDateTime lastConnected;

    QString label() const { return name.isEmpty() ? host : name; }
    QString address() const;
    QString passwordKey() const { return id + QStringLiteral("/password"); }
    QString gatewayPasswordKey() const { return id + QStringLiteral("/gateway-password"); }

    QJsonObject toJson() const;
    static Bookmark fromJson(const QJsonObject& o);
    static Bookmark create();

    // Session arguments (NUL-free strings) for `fastrdp --session-stdin`.
    QStringList sessionArgs(const QString& password, const QString& gatewayPassword) const;
};

// Parses "host", "host:port", "[v6]:port".
void splitHostPort(const QString& in, QString& host, int& port, int defaultPort);

// Bookmarks persisted as JSON in ~/.config/fastrdp/bookmarks.json (mode 0600).
// Passwords are never written here; they live in the system keyring.
class BookmarkStore {
public:
    bool load();
    bool save() const;

    const QList<Bookmark>& items() const { return items_; }
    const Bookmark* find(const QString& id) const;
    void upsert(const Bookmark& b);
    void remove(const QString& id);

    static QString path();

private:
    QList<Bookmark> items_;
};

} // namespace fastrdp
