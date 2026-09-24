#include "bookmark.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace fastrdp {

namespace {

const char* displayName(Bookmark::Display d) {
    switch (d) {
    case Bookmark::Display::Fullscreen: return "fullscreen";
    case Bookmark::Display::Fixed: return "fixed";
    case Bookmark::Display::AllMonitors: return "multimon";
    default: return "window";
    }
}

Bookmark::Display displayFrom(const QString& s) {
    if (s == QLatin1String("fullscreen")) return Bookmark::Display::Fullscreen;
    if (s == QLatin1String("fixed")) return Bookmark::Display::Fixed;
    if (s == QLatin1String("multimon")) return Bookmark::Display::AllMonitors;
    return Bookmark::Display::Window;
}

QString hostPort(const QString& host, int port, int defaultPort) {
    const QString h = host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('['))
                          ? QStringLiteral("[%1]").arg(host)
                          : host;
    return port == defaultPort ? h : QStringLiteral("%1:%2").arg(h).arg(port);
}

} // namespace

void splitHostPort(const QString& in, QString& host, int& port, int defaultPort) {
    const QString s = in.trimmed();
    port = defaultPort;
    host = s;
    if (s.startsWith(QLatin1Char('['))) {
        const int close = s.indexOf(QLatin1Char(']'));
        if (close > 0) {
            host = s.mid(1, close - 1);
            if (s.mid(close + 1).startsWith(QLatin1Char(':'))) port = s.mid(close + 2).toInt();
        }
    } else if (s.count(QLatin1Char(':')) == 1) {
        const int colon = s.indexOf(QLatin1Char(':'));
        host = s.left(colon);
        bool ok = false;
        const int p = s.mid(colon + 1).toInt(&ok);
        if (ok) port = p;
    }
    if (port <= 0 || port > 65535) port = defaultPort;
}

QString Bookmark::address() const { return hostPort(host, port, 3389); }

Bookmark Bookmark::create() {
    Bookmark b;
    b.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return b;
}

QJsonObject Bookmark::toJson() const {
    QJsonObject o;
    o[QStringLiteral("id")] = id;
    o[QStringLiteral("name")] = name;
    o[QStringLiteral("host")] = host;
    o[QStringLiteral("port")] = port;
    o[QStringLiteral("username")] = username;
    o[QStringLiteral("domain")] = domain;
    o[QStringLiteral("savePassword")] = savePassword;
    o[QStringLiteral("display")] = QString::fromLatin1(displayName(display));
    o[QStringLiteral("width")] = width;
    o[QStringLiteral("height")] = height;
    o[QStringLiteral("scale")] = scale;
    o[QStringLiteral("network")] = network;
    o[QStringLiteral("h264")] = h264;
    o[QStringLiteral("gpuDecode")] = gpuDecode;
    o[QStringLiteral("vsync")] = vsync;
    o[QStringLiteral("audio")] = audio;
    o[QStringLiteral("clipboard")] = clipboard;
    o[QStringLiteral("microphone")] = microphone;
    o[QStringLiteral("shareHome")] = shareHome;
    o[QStringLiteral("gateway")] = gateway;
    o[QStringLiteral("gatewayHost")] = gatewayHost;
    o[QStringLiteral("gatewayPort")] = gatewayPort;
    o[QStringLiteral("gatewaySameCreds")] = gatewaySameCreds;
    o[QStringLiteral("gatewayUser")] = gatewayUser;
    o[QStringLiteral("gatewayDomain")] = gatewayDomain;
    o[QStringLiteral("gatewaySavePassword")] = gatewaySavePassword;
    o[QStringLiteral("extraArgs")] = extraArgs;
    if (lastConnected.isValid())
        o[QStringLiteral("lastConnected")] = lastConnected.toString(Qt::ISODate);
    return o;
}

Bookmark Bookmark::fromJson(const QJsonObject& o) {
    Bookmark b;
    b.id = o.value(QStringLiteral("id")).toString();
    if (b.id.isEmpty()) b.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    b.name = o.value(QStringLiteral("name")).toString();
    b.host = o.value(QStringLiteral("host")).toString();
    b.port = o.value(QStringLiteral("port")).toInt(3389);
    b.username = o.value(QStringLiteral("username")).toString();
    b.domain = o.value(QStringLiteral("domain")).toString();
    b.savePassword = o.value(QStringLiteral("savePassword")).toBool();
    b.display = displayFrom(o.value(QStringLiteral("display")).toString());
    b.width = o.value(QStringLiteral("width")).toInt(1920);
    b.height = o.value(QStringLiteral("height")).toInt(1080);
    b.scale = o.value(QStringLiteral("scale")).toString(QStringLiteral("fit"));
    b.network = o.value(QStringLiteral("network")).toString(QStringLiteral("auto"));
    b.h264 = o.value(QStringLiteral("h264")).toBool(true);
    b.gpuDecode = o.value(QStringLiteral("gpuDecode")).toBool(true);
    b.vsync = o.value(QStringLiteral("vsync")).toBool(false);
    b.audio = o.value(QStringLiteral("audio")).toString(QStringLiteral("local"));
    b.clipboard = o.value(QStringLiteral("clipboard")).toBool(true);
    b.microphone = o.value(QStringLiteral("microphone")).toBool();
    b.shareHome = o.value(QStringLiteral("shareHome")).toBool();
    b.gateway = o.value(QStringLiteral("gateway")).toBool();
    b.gatewayHost = o.value(QStringLiteral("gatewayHost")).toString();
    b.gatewayPort = o.value(QStringLiteral("gatewayPort")).toInt(443);
    b.gatewaySameCreds = o.value(QStringLiteral("gatewaySameCreds")).toBool(true);
    b.gatewayUser = o.value(QStringLiteral("gatewayUser")).toString();
    b.gatewayDomain = o.value(QStringLiteral("gatewayDomain")).toString();
    b.gatewaySavePassword = o.value(QStringLiteral("gatewaySavePassword")).toBool();
    b.extraArgs = o.value(QStringLiteral("extraArgs")).toString();
    b.lastConnected =
        QDateTime::fromString(o.value(QStringLiteral("lastConnected")).toString(), Qt::ISODate);
    return b;
}

QStringList Bookmark::sessionArgs(const QString& password, const QString& gatewayPassword) const {
    QStringList a;
    a << QStringLiteral("--title=%1").arg(label());
    a << QStringLiteral("--scale=%1").arg(scale);
    if (!h264) a << QStringLiteral("--no-h264");
    if (!gpuDecode) a << QStringLiteral("--sw-decode");
    if (vsync) a << QStringLiteral("--vsync");

    a << QStringLiteral("/v:%1").arg(address());
    if (!username.isEmpty()) a << QStringLiteral("/u:%1").arg(username);
    if (!domain.isEmpty()) a << QStringLiteral("/d:%1").arg(domain);
    if (!password.isEmpty()) a << QStringLiteral("--password=%1").arg(password);

    switch (display) {
    case Display::Fullscreen: a << QStringLiteral("/f"); break;
    case Display::Fixed: a << QStringLiteral("/size:%1x%2").arg(width).arg(height); break;
    case Display::AllMonitors: a << QStringLiteral("/multimon"); break;
    case Display::Window: break;
    }

    a << QStringLiteral("/network:%1").arg(network);

    if (audio == QLatin1String("local")) a << QStringLiteral("/sound");
    else if (audio == QLatin1String("remote")) a << QStringLiteral("/audio-mode:1");
    else a << QStringLiteral("/audio-mode:2");
    a << (clipboard ? QStringLiteral("+clipboard") : QStringLiteral("-clipboard"));
    if (microphone) a << QStringLiteral("/microphone");
    if (shareHome) a << QStringLiteral("+home-drive");

    if (gateway && !gatewayHost.isEmpty()) {
        a << QStringLiteral("--gw-host=%1").arg(hostPort(gatewayHost, gatewayPort, 443));
        if (gatewaySameCreds) {
            a << QStringLiteral("--gw-same-creds");
        } else {
            if (!gatewayUser.isEmpty()) a << QStringLiteral("--gw-user=%1").arg(gatewayUser);
            if (!gatewayDomain.isEmpty()) a << QStringLiteral("--gw-domain=%1").arg(gatewayDomain);
            if (!gatewayPassword.isEmpty())
                a << QStringLiteral("--gw-password=%1").arg(gatewayPassword);
        }
    }

    a << QProcess::splitCommand(extraArgs);
    return a;
}

// ---------------------------------------------------------------------------------------

QString BookmarkStore::path() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/bookmarks.json");
}

bool BookmarkStore::load() {
    items_.clear();
    QFile f(path());
    if (!f.exists()) return true;
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QJsonArray arr = doc.object().value(QStringLiteral("bookmarks")).toArray();
    for (const QJsonValue& v : arr) items_.append(Bookmark::fromJson(v.toObject()));
    return true;
}

bool BookmarkStore::save() const {
    const QString p = path();
    QDir().mkpath(QFileInfo(p).absolutePath());
    QJsonArray arr;
    for (const Bookmark& b : items_) arr.append(b.toJson());
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("bookmarks")] = arr;

    QSaveFile f(p);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

const Bookmark* BookmarkStore::find(const QString& id) const {
    for (const Bookmark& b : items_)
        if (b.id == id) return &b;
    return nullptr;
}

void BookmarkStore::upsert(const Bookmark& b) {
    for (Bookmark& existing : items_) {
        if (existing.id == b.id) {
            existing = b;
            return;
        }
    }
    items_.append(b);
}

void BookmarkStore::remove(const QString& id) {
    items_.removeIf([&](const Bookmark& b) { return b.id == id; });
}

} // namespace fastrdp
