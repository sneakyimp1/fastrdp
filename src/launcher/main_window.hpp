#pragma once

#include "bookmark.hpp"

#include <QHash>
#include <QMainWindow>
#include <QProcess>

class QAction;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace fastrdp {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    struct Session {
        QProcess* process = nullptr;
        Bookmark bookmark;
        bool saved = false; // bookmark lives in the store (quick connects don't)
    };

    void reload();
    void updateActions();
    void updateStatus();
    QString selectedId() const;

    void newBookmark();
    void editBookmark();
    void duplicateBookmark();
    void deleteBookmark();
    void connectSelected();
    void quickConnect();
    void showLog();
    void contextMenu(const QPoint& pos);

    // Resolves credentials (keyring or prompt) and starts the session process.
    void connectTo(Bookmark b, bool saved, const QString& error = {});
    void launch(const Bookmark& b, bool saved, const QString& password, const QString& gwPassword);
    void sessionFinished(QProcess* p, int exitCode, QProcess::ExitStatus status);
    bool isRunning(const QString& id) const;

    BookmarkStore store_;
    QHash<QProcess*, Session> sessions_;
    QHash<QString, QString> logs_; // bookmark id -> last session output

    QTreeWidget* list_;
    QStackedWidget* stack_;
    QLineEdit* search_;
    QLineEdit* quick_;
    QLabel* status_;
    QAction *actConnect_, *actEdit_, *actDuplicate_, *actDelete_, *actLog_;
};

} // namespace fastrdp
