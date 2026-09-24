#include "launcher.hpp"

#include "main_window.hpp"

#include <QApplication>
#include <QStandardPaths>

namespace fastrdp {

int runLauncher(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("fastrdp"));
    QApplication::setApplicationDisplayName(QStringLiteral("fastrdp"));
    // Only claim the app id when the .desktop file is installed; otherwise the desktop
    // portal rejects the registration and Qt logs a warning.
    const QString appId = QStringLiteral(FASTRDP_APP_ID);
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, appId + QStringLiteral(".desktop")).isEmpty())
        QApplication::setDesktopFileName(appId);
    QApplication::setQuitOnLastWindowClosed(false);

    MainWindow w;
    w.show();
    return app.exec();
}

} // namespace fastrdp
