#include "main_window.hpp"

#include "dialogs.hpp"
#include "secrets.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace fastrdp {

namespace {

enum Column { ColName, ColComputer, ColUser, ColLast, ColCount };
constexpr int kIdRole = Qt::UserRole;
constexpr int kSessionExitAuthFailed = 2;

QString userLabel(const Bookmark& b) {
    if (b.username.isEmpty()) return {};
    return b.domain.isEmpty() ? b.username : b.domain + QLatin1Char('\\') + b.username;
}

QString whenLabel(const QDateTime& t) {
    if (!t.isValid()) return QObject::tr("never");
    const QDate today = QDate::currentDate();
    const QLocale loc;
    if (t.date() == today) return QObject::tr("today %1").arg(loc.toString(t.time(), QLocale::ShortFormat));
    if (t.date() == today.addDays(-1))
        return QObject::tr("yesterday %1").arg(loc.toString(t.time(), QLocale::ShortFormat));
    return loc.toString(t.date(), QLocale::ShortFormat);
}

} // namespace

MainWindow::MainWindow() {
    setWindowTitle(tr("fastrdp"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("krdc"), QIcon::fromTheme(QStringLiteral("network-server"))));

    // --- Actions / toolbar ---
    auto* actNew = new QAction(QIcon::fromTheme(QStringLiteral("list-add")), tr("New"), this);
    actNew->setShortcut(QKeySequence::New);
    actConnect_ = new QAction(QIcon::fromTheme(QStringLiteral("network-connect")), tr("Connect"), this);
    actEdit_ = new QAction(QIcon::fromTheme(QStringLiteral("document-edit")), tr("Edit…"), this);
    actEdit_->setShortcut(Qt::Key_F2);
    actDuplicate_ = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Duplicate"), this);
    actDelete_ = new QAction(QIcon::fromTheme(QStringLiteral("edit-delete")), tr("Delete"), this);
    actDelete_->setShortcut(QKeySequence::Delete);
    actLog_ = new QAction(QIcon::fromTheme(QStringLiteral("text-x-log"), QIcon::fromTheme(QStringLiteral("text-x-generic"))),
                          tr("Show last session log"), this);

    connect(actNew, &QAction::triggered, this, &MainWindow::newBookmark);
    connect(actConnect_, &QAction::triggered, this, &MainWindow::connectSelected);
    connect(actEdit_, &QAction::triggered, this, &MainWindow::editBookmark);
    connect(actDuplicate_, &QAction::triggered, this, &MainWindow::duplicateBookmark);
    connect(actDelete_, &QAction::triggered, this, &MainWindow::deleteBookmark);
    connect(actLog_, &QAction::triggered, this, &MainWindow::showLog);

    auto* tb = addToolBar(tr("Main"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->addAction(actConnect_);
    tb->addSeparator();
    tb->addAction(actNew);
    tb->addAction(actEdit_);
    tb->addAction(actDuplicate_);
    tb->addAction(actDelete_);

    // --- Quick connect + search ---
    quick_ = new QLineEdit;
    quick_->setPlaceholderText(tr("Quick connect: computer name or address"));
    quick_->setClearButtonEnabled(true);
    auto* quickBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("network-connect")), tr("Connect"));
    connect(quick_, &QLineEdit::returnPressed, this, &MainWindow::quickConnect);
    connect(quickBtn, &QPushButton::clicked, this, &MainWindow::quickConnect);

    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Search saved connections"));
    search_->setClearButtonEnabled(true);
    connect(search_, &QLineEdit::textChanged, this, &MainWindow::reload);

    // --- List ---
    list_ = new QTreeWidget;
    list_->setColumnCount(ColCount);
    list_->setHeaderLabels({tr("Name"), tr("Computer"), tr("User"), tr("Last connected")});
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setAlternatingRowColors(true);
    list_->setSortingEnabled(true);
    list_->sortByColumn(ColName, Qt::AscendingOrder);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(ColComputer, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(ColUser, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(ColLast, QHeaderView::ResizeToContents);
    connect(list_, &QTreeWidget::itemActivated, this, &MainWindow::connectSelected);
    connect(list_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateActions);
    connect(list_, &QTreeWidget::customContextMenuRequested, this, &MainWindow::contextMenu);

    auto* empty = new QLabel(tr("<p style='font-size:large'>No saved connections yet</p>"
                                "<p>Click <b>New</b> to add a computer, or use quick connect above.</p>"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setEnabled(false);

    stack_ = new QStackedWidget;
    stack_->addWidget(list_);
    stack_->addWidget(empty);

    auto* central = new QWidget;
    auto* v = new QVBoxLayout(central);
    auto* quickRow = new QHBoxLayout;
    quickRow->addWidget(quick_, 1);
    quickRow->addWidget(quickBtn);
    v->addLayout(quickRow);
    v->addWidget(search_);
    v->addWidget(stack_, 1);
    setCentralWidget(central);

    status_ = new QLabel;
    statusBar()->addPermanentWidget(status_);

    QSettings settings;
    if (!restoreGeometry(settings.value(QStringLiteral("launcher/geometry")).toByteArray()))
        resize(860, 540);

    if (!store_.load())
        QMessageBox::warning(this, tr("fastrdp"), tr("Couldn't read %1").arg(BookmarkStore::path()));
    reload();
    updateStatus();
}

// ---------------------------------------------------------------------------------------

void MainWindow::reload() {
    const QString keep = selectedId();
    const QString filter = search_->text().trimmed();

    list_->setSortingEnabled(false);
    list_->clear();
    for (const Bookmark& b : store_.items()) {
        if (!filter.isEmpty() && !b.label().contains(filter, Qt::CaseInsensitive) &&
            !b.host.contains(filter, Qt::CaseInsensitive) &&
            !b.username.contains(filter, Qt::CaseInsensitive))
            continue;
        auto* item = new QTreeWidgetItem(list_);
        item->setData(ColName, kIdRole, b.id);
        item->setText(ColName, b.label());
        item->setText(ColComputer, b.address() + (b.gateway ? tr("  (via %1)").arg(b.gatewayHost) : QString()));
        item->setText(ColUser, userLabel(b));
        item->setText(ColLast, whenLabel(b.lastConnected));
        item->setData(ColLast, Qt::ToolTipRole, b.lastConnected.isValid()
                                                    ? QLocale().toString(b.lastConnected)
                                                    : QString());
        const bool running = isRunning(b.id);
        item->setIcon(ColName, QIcon::fromTheme(running ? QStringLiteral("media-playback-start")
                                                        : QStringLiteral("computer")));
        if (running) {
            QFont f = item->font(ColName);
            f.setBold(true);
            item->setFont(ColName, f);
            item->setToolTip(ColName, tr("Session open"));
        }
        if (b.id == keep) item->setSelected(true);
    }
    list_->setSortingEnabled(true);
    if (!list_->selectedItems().isEmpty()) list_->scrollToItem(list_->selectedItems().first());
    else if (list_->topLevelItemCount() > 0) list_->setCurrentItem(list_->topLevelItem(0));

    stack_->setCurrentIndex(store_.items().isEmpty() ? 1 : 0);
    updateActions();
}

QString MainWindow::selectedId() const {
    const auto sel = list_->selectedItems();
    return sel.isEmpty() ? QString() : sel.first()->data(ColName, kIdRole).toString();
}

void MainWindow::updateActions() {
    const bool has = !selectedId().isEmpty();
    for (QAction* a : {actConnect_, actEdit_, actDuplicate_, actDelete_}) a->setEnabled(has);
    actLog_->setEnabled(has && logs_.contains(selectedId()));
}

void MainWindow::updateStatus() {
    const int n = int(sessions_.size());
    status_->setText(n == 0 ? tr("No open sessions") : tr("%n open session(s)", nullptr, n));
}

bool MainWindow::isRunning(const QString& id) const {
    for (const Session& s : sessions_)
        if (s.bookmark.id == id) return true;
    return false;
}

void MainWindow::contextMenu(const QPoint& pos) {
    if (!list_->itemAt(pos)) return;
    QMenu menu(this);
    menu.addAction(actConnect_);
    menu.addSeparator();
    menu.addAction(actEdit_);
    menu.addAction(actDuplicate_);
    menu.addAction(actDelete_);
    menu.addSeparator();
    menu.addAction(actLog_);
    menu.exec(list_->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------------------
// Bookmark editing

void MainWindow::newBookmark() {
    Bookmark b = Bookmark::create();
    EditDialog dlg(b, false, false, this);
    if (dlg.exec() != QDialog::Accepted) return;
    b = dlg.bookmark();

    QString err;
    if (b.savePassword && !secrets::write(b.passwordKey(), dlg.password(), &err)) {
        QMessageBox::warning(this, tr("fastrdp"), tr("Couldn't save the password: %1").arg(err));
        b.savePassword = false;
    }
    if (b.gatewaySavePassword && !secrets::write(b.gatewayPasswordKey(), dlg.gatewayPassword(), &err))
        b.gatewaySavePassword = false;

    store_.upsert(b);
    store_.save();
    search_->clear();
    reload();
    for (int i = 0; i < list_->topLevelItemCount(); i++)
        if (list_->topLevelItem(i)->data(ColName, kIdRole).toString() == b.id)
            list_->setCurrentItem(list_->topLevelItem(i));
}

void MainWindow::editBookmark() {
    const Bookmark* cur = store_.find(selectedId());
    if (!cur) return;
    EditDialog dlg(*cur, cur->savePassword, cur->gatewaySavePassword, this);
    if (dlg.exec() != QDialog::Accepted) return;
    Bookmark b = dlg.bookmark();

    QString err;
    if (b.savePassword) {
        if (!dlg.password().isEmpty() && !secrets::write(b.passwordKey(), dlg.password(), &err)) {
            QMessageBox::warning(this, tr("fastrdp"), tr("Couldn't save the password: %1").arg(err));
            b.savePassword = false;
        }
    } else {
        secrets::remove(b.passwordKey());
    }
    if (b.gatewaySavePassword) {
        if (!dlg.gatewayPassword().isEmpty() &&
            !secrets::write(b.gatewayPasswordKey(), dlg.gatewayPassword(), &err))
            b.gatewaySavePassword = false;
    } else {
        secrets::remove(b.gatewayPasswordKey());
    }

    store_.upsert(b);
    store_.save();
    reload();
}

void MainWindow::duplicateBookmark() {
    const Bookmark* cur = store_.find(selectedId());
    if (!cur) return;
    Bookmark b = *cur;
    b.id = Bookmark::create().id;
    b.name = tr("%1 (copy)").arg(cur->label());
    b.lastConnected = {};
    // Copy saved secrets so the duplicate works the same way.
    QString pw;
    if (b.savePassword && !(secrets::read(cur->passwordKey(), pw) && secrets::write(b.passwordKey(), pw)))
        b.savePassword = false;
    if (b.gatewaySavePassword &&
        !(secrets::read(cur->gatewayPasswordKey(), pw) && secrets::write(b.gatewayPasswordKey(), pw)))
        b.gatewaySavePassword = false;
    pw.fill(QChar(0));
    store_.upsert(b);
    store_.save();
    reload();
}

void MainWindow::deleteBookmark() {
    const Bookmark* cur = store_.find(selectedId());
    if (!cur) return;
    if (QMessageBox::question(this, tr("Delete connection"),
                              tr("Delete “%1”? Its saved password is removed as well.").arg(cur->label()),
                              QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    secrets::remove(cur->passwordKey());
    secrets::remove(cur->gatewayPasswordKey());
    logs_.remove(cur->id);
    store_.remove(cur->id);
    store_.save();
    reload();
}

void MainWindow::showLog() {
    const QString id = selectedId();
    if (!logs_.contains(id)) return;
    QDialog dlg(this);
    const Bookmark* b = store_.find(id);
    dlg.setWindowTitle(tr("Session log — %1").arg(b ? b->label() : id));
    auto* text = new QPlainTextEdit(logs_.value(id));
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(text);
    v->addWidget(buttons);
    dlg.resize(900, 500);
    text->moveCursor(QTextCursor::End);
    dlg.exec();
}

// ---------------------------------------------------------------------------------------
// Connecting

void MainWindow::connectSelected() {
    const Bookmark* cur = store_.find(selectedId());
    if (cur) connectTo(*cur, true);
}

void MainWindow::quickConnect() {
    const QString text = quick_->text().trimmed();
    if (text.isEmpty()) return;
    Bookmark b = Bookmark::create();
    splitHostPort(text, b.host, b.port, 3389);
    connectTo(b, false);
}

void MainWindow::connectTo(Bookmark b, bool saved, const QString& error) {
    QString password;
    bool havePassword = false;
    if (b.savePassword && !b.smartcardLogon && error.isEmpty()) {
        QString err;
        havePassword = secrets::read(b.passwordKey(), password, &err);
        if (!havePassword && !err.isEmpty())
            statusBar()->showMessage(tr("Couldn't read the saved password: %1").arg(err), 8000);
    }
    if (!havePassword) {
        CredentialsDialog dlg(b.label(), b.username, b.domain, b.savePassword, error, this,
                              b.smartcardLogon);
        if (dlg.exec() != QDialog::Accepted) return;
        b.username = dlg.username();
        b.domain = dlg.domain();
        password = dlg.password();
        if (saved && !b.smartcardLogon) {
            QString err;
            if (dlg.remember()) {
                b.savePassword = secrets::write(b.passwordKey(), password, &err);
                if (!b.savePassword)
                    QMessageBox::warning(this, tr("fastrdp"), tr("Couldn't save the password: %1").arg(err));
            } else if (b.savePassword) {
                secrets::remove(b.passwordKey());
                b.savePassword = false;
            }
        }
    }

    QString gwPassword;
    if (b.gateway && !b.gatewaySameCreds) {
        const bool have = b.gatewaySavePassword && error.isEmpty() &&
                          secrets::read(b.gatewayPasswordKey(), gwPassword);
        if (!have) {
            bool ok = false;
            gwPassword = QInputDialog::getText(this, tr("Gateway sign-in"),
                                               tr("Password for %1 on gateway %2:")
                                                   .arg(b.gatewayUser, b.gatewayHost),
                                               QLineEdit::Password, QString(), &ok);
            if (!ok) return;
        }
    }

    b.lastConnected = QDateTime::currentDateTime();
    if (saved) {
        store_.upsert(b);
        store_.save();
    }
    launch(b, saved, password, gwPassword);
    password.fill(QChar(0));
    gwPassword.fill(QChar(0));
}

void MainWindow::launch(const Bookmark& b, bool saved, const QString& password,
                        const QString& gwPassword) {
    auto* p = new QProcess(this);
    p->setProgram(QCoreApplication::applicationFilePath());
    p->setArguments({QStringLiteral("--session-stdin")});
    p->setProcessChannelMode(QProcess::MergedChannels);
    sessions_.insert(p, Session{p, b, saved});
    logs_[b.id].clear();

    connect(p, &QProcess::readyRead, this, [this, p] {
        auto it = sessions_.find(p);
        if (it == sessions_.end()) return;
        QString& log = logs_[it->bookmark.id];
        log += QString::fromLocal8Bit(p->readAll());
        if (log.size() > 400000) log = log.right(300000);
    });
    connect(p, &QProcess::finished, this,
            [this, p](int code, QProcess::ExitStatus st) { sessionFinished(p, code, st); });

    p->start();
    if (!p->waitForStarted(5000)) {
        QMessageBox::critical(this, tr("fastrdp"), tr("Couldn't start the session: %1").arg(p->errorString()));
        sessions_.remove(p);
        p->deleteLater();
        return;
    }

    QByteArray payload;
    for (const QString& a : b.sessionArgs(password, gwPassword)) {
        payload += a.toUtf8();
        payload += '\0';
    }
    p->write(payload);
    p->closeWriteChannel();
    payload.fill('\0');

    statusBar()->showMessage(tr("Connecting to %1…").arg(b.label()), 5000);
    updateStatus();
    reload();
}

void MainWindow::sessionFinished(QProcess* p, int exitCode, QProcess::ExitStatus status) {
    Session s = sessions_.take(p);
    p->deleteLater();
    updateStatus();
    reload();

    if (status == QProcess::CrashExit) {
        QMessageBox box(QMessageBox::Critical, tr("fastrdp"),
                        tr("The session for “%1” quit unexpectedly.").arg(s.bookmark.label()),
                        QMessageBox::Ok, this);
        box.setDetailedText(logs_.value(s.bookmark.id).right(8000));
        box.exec();
    } else if (exitCode == kSessionExitAuthFailed) {
        // Re-read the bookmark in case it was edited while the session ran.
        Bookmark b = s.bookmark;
        if (s.saved)
            if (const Bookmark* cur = store_.find(b.id)) b = *cur;
        QTimer::singleShot(0, this, [this, b, saved = s.saved] {
            connectTo(b, saved, tr("The user name or password was not accepted. Try again."));
        });
    } else if (exitCode != 0) {
        statusBar()->showMessage(tr("Connection to %1 ended with an error — see the session log.")
                                     .arg(s.bookmark.label()),
                                 10000);
    }

    if (isHidden() && sessions_.isEmpty()) qApp->quit();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings().setValue(QStringLiteral("launcher/geometry"), saveGeometry());
    if (sessions_.isEmpty()) {
        e->accept();
        qApp->quit();
        return;
    }
    QMessageBox box(QMessageBox::Question, tr("fastrdp"),
                    tr("%n session(s) still open.", nullptr, int(sessions_.size())),
                    QMessageBox::NoButton, this);
    auto* keep = box.addButton(tr("Keep running"), QMessageBox::AcceptRole);
    auto* end = box.addButton(tr("Disconnect all"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(keep);
    box.exec();
    if (box.clickedButton() == keep) {
        hide(); // quits once the last session ends
        e->ignore();
    } else if (box.clickedButton() == end) {
        for (QProcess* p : sessions_.keys()) {
            p->terminate();
            if (!p->waitForFinished(3000)) p->kill();
        }
        e->accept();
        qApp->quit();
    } else {
        e->ignore();
    }
}

} // namespace fastrdp
