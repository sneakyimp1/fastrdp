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
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, QStringLiteral("fastrdp.desktop")).isEmpty())
        QApplication::setDesktopFileName(QStringLiteral("fastrdp"));
    QApplication::setQuitOnLastWindowClosed(false);

    MainWindow w;
    w.show();
    return app.exec();
}

} // namespace fastrdp
