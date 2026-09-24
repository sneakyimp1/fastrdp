#include "dialogs.hpp"

#include "secrets.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace fastrdp {

namespace {

QLabel* hint(const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setEnabled(false); // renders in the theme's secondary text color
    return l;
}

void selectData(QComboBox* c, const QString& value) {
    const int i = c->findData(value);
    c->setCurrentIndex(i >= 0 ? i : 0);
}

} // namespace

EditDialog::EditDialog(const Bookmark& b, bool hasSavedPassword, bool hasSavedGatewayPassword,
                       QWidget* parent)
    : QDialog(parent), b_(b), hadPassword_(hasSavedPassword),
      hadGatewayPassword_(hasSavedGatewayPassword) {
    setWindowTitle(b.host.isEmpty() ? tr("New connection") : tr("Edit “%1”").arg(b.label()));

    auto* tabs = new QTabWidget;
    tabs->addTab(generalTab(), tr("General"));
    tabs->addTab(displayTab(), tr("Display"));
    tabs->addTab(performanceTab(), tr("Performance"));
    tabs->addTab(resourcesTab(), tr("Local resources"));
    tabs->addTab(gatewayTab(), tr("Gateway"));
    tabs->addTab(advancedTab(), tr("Advanced"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);

    updateEnabled();
    resize(560, sizeHint().height());
    address_->setFocus();
}

QWidget* EditDialog::generalTab() {
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);

    address_ = new QLineEdit(b_.host.isEmpty() ? QString() : b_.address());
    address_->setPlaceholderText(tr("hostname or IP, optionally :port"));
    name_ = new QLineEdit(b_.name);
    name_->setPlaceholderText(tr("defaults to the computer name"));
    user_ = new QLineEdit(b_.username);
    user_->setPlaceholderText(tr("user, DOMAIN\\user or user@domain"));
    domain_ = new QLineEdit(b_.domain);
    domain_->setPlaceholderText(tr("optional"));
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(hadPassword_ ? tr("saved — leave blank to keep")
                                               : tr("ask when connecting"));
    savePassword_ = new QCheckBox(tr("Remember password in the system keyring (KWallet)"));
    savePassword_->setChecked(b_.savePassword);
    if (!secrets::available()) {
        savePassword_->setEnabled(false);
        savePassword_->setChecked(false);
        savePassword_->setToolTip(tr("No Secret Service / KWallet is available."));
    }

    form->addRow(tr("Computer:"), address_);
    form->addRow(tr("Name:"), name_);
    form->addRow(tr("User name:"), user_);
    form->addRow(tr("Domain:"), domain_);
    smartcardLogon_ = new QCheckBox(tr("Sign in with a smart card (asks for the PIN)"));
    smartcardLogon_->setChecked(b_.smartcardLogon);
    form->addRow(tr("Password:"), password_);
    form->addRow(QString(), savePassword_);
    form->addRow(QString(), smartcardLogon_);
    form->addRow(QString(), hint(tr("Passwords are only ever stored in your keyring, never in "
                                    "fastrdp's configuration files.")));
    connect(savePassword_, &QCheckBox::toggled, this, &EditDialog::updateEnabled);
    connect(smartcardLogon_, &QCheckBox::toggled, this, &EditDialog::updateEnabled);
    return w;
}

QWidget* EditDialog::displayTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);

    auto* box = new QGroupBox(tr("Remote desktop size"));
    auto* bv = new QVBoxLayout(box);
    dispWindow_ = new QRadioButton(tr("Match the window — resize the remote desktop with it"));
    dispFull_ = new QRadioButton(tr("Full screen"));
    dispMulti_ = new QRadioButton(tr("Full screen on all my monitors"));
    dispFixed_ = new QRadioButton(tr("Fixed resolution:"));
    width_ = new QSpinBox;
    height_ = new QSpinBox;
    width_->setRange(200, 8192);
    height_->setRange(200, 8192);
    width_->setValue(b_.width);
    height_->setValue(b_.height);
    auto* fixedRow = new QHBoxLayout;
    fixedRow->addWidget(dispFixed_);
    fixedRow->addWidget(width_);
    fixedRow->addWidget(new QLabel(QStringLiteral("×")));
    fixedRow->addWidget(height_);
    fixedRow->addStretch();
    bv->addWidget(dispWindow_);
    bv->addWidget(dispFull_);
    bv->addWidget(dispMulti_);
    bv->addLayout(fixedRow);
    switch (b_.display) {
    case Bookmark::Display::Window: dispWindow_->setChecked(true); break;
    case Bookmark::Display::Fullscreen: dispFull_->setChecked(true); break;
    case Bookmark::Display::Fixed: dispFixed_->setChecked(true); break;
    case Bookmark::Display::AllMonitors: dispMulti_->setChecked(true); break;
    }
    for (auto* r : {dispWindow_, dispFull_, dispMulti_, dispFixed_})
        connect(r, &QRadioButton::toggled, this, &EditDialog::updateEnabled);

    auto* form = new QFormLayout;
    scale_ = new QComboBox;
    scale_->addItem(tr("Scale to fit (keep aspect)"), QStringLiteral("fit"));
    scale_->addItem(tr("Stretch to window"), QStringLiteral("stretch"));
    scale_->addItem(tr("Don't scale"), QStringLiteral("native"));
    selectData(scale_, b_.scale);
    form->addRow(tr("When sizes differ:"), scale_);

    v->addWidget(box);
    v->addLayout(form);
    v->addWidget(hint(tr("HiDPI scaling follows your display settings automatically. "
                         "Toggle full screen during a session with Ctrl+Alt+Enter.")));
    v->addStretch();
    return w;
}

QWidget* EditDialog::performanceTab() {
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);

    network_ = new QComboBox;
    network_->addItem(tr("Auto-detect (recommended)"), QStringLiteral("auto"));
    network_->addItem(tr("LAN (10 Mbps or higher)"), QStringLiteral("lan"));
    network_->addItem(tr("Broadband, high"), QStringLiteral("broadband-high"));
    network_->addItem(tr("WAN"), QStringLiteral("wan"));
    network_->addItem(tr("Broadband, low"), QStringLiteral("broadband-low"));
    network_->addItem(tr("Modem"), QStringLiteral("modem"));
    selectData(network_, b_.network);

    h264_ = new QCheckBox(tr("Use H.264 / AVC444 video (best for scrolling and video)"));
    h264_->setChecked(b_.h264);
    gpu_ = new QCheckBox(tr("Decode video on the GPU (VAAPI)"));
    gpu_->setChecked(b_.gpuDecode);
    vsync_ = new QCheckBox(tr("Sync to display refresh (steadier pacing, +1 frame latency)"));
    vsync_->setChecked(b_.vsync);
    connect(h264_, &QCheckBox::toggled, this, &EditDialog::updateEnabled);

    form->addRow(tr("Connection:"), network_);
    form->addRow(QString(), h264_);
    form->addRow(QString(), gpu_);
    form->addRow(QString(), vsync_);
    form->addRow(QString(), hint(tr("For the best result enable “Prioritize H.264/AVC 444 graphics "
                                    "mode” and hardware encoding in the remote PC's Group Policy.")));
    return w;
}

QWidget* EditDialog::resourcesTab() {
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    audio_ = new QComboBox;
    audio_->addItem(tr("Play on this computer"), QStringLiteral("local"));
    audio_->addItem(tr("Play on the remote computer"), QStringLiteral("remote"));
    audio_->addItem(tr("Don't play"), QStringLiteral("off"));
    selectData(audio_, b_.audio);
    clipboard_ = new QCheckBox(tr("Clipboard (text, formatted text and images)"));
    clipboard_->setChecked(b_.clipboard);
    mic_ = new QCheckBox(tr("Microphone"));
    mic_->setChecked(b_.microphone);
    home_ = new QCheckBox(tr("Share my home folder"));
    home_->setChecked(b_.shareHome);
    smartcards_ = new QCheckBox(tr("Smart card readers"));
    smartcards_->setChecked(b_.smartcards || b_.smartcardLogon);
    form->addRow(tr("Remote audio:"), audio_);
    form->addRow(QString(), clipboard_);
    form->addRow(QString(), mic_);
    form->addRow(QString(), home_);
    form->addRow(QString(), smartcards_);
    return w;
}

QWidget* EditDialog::gatewayTab() {
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    gwEnabled_ = new QCheckBox(tr("Connect through a Remote Desktop Gateway"));
    gwEnabled_->setChecked(b_.gateway);
    gwAddress_ = new QLineEdit(b_.gatewayHost.isEmpty()
                                   ? QString()
                                   : (b_.gatewayPort == 443 ? b_.gatewayHost
                                                            : QStringLiteral("%1:%2").arg(b_.gatewayHost).arg(b_.gatewayPort)));
    gwAddress_->setPlaceholderText(tr("gateway.example.com"));
    gwSame_ = new QCheckBox(tr("Use my remote computer credentials"));
    gwSame_->setChecked(b_.gatewaySameCreds);
    gwUser_ = new QLineEdit(b_.gatewayUser);
    gwDomain_ = new QLineEdit(b_.gatewayDomain);
    gwPassword_ = new QLineEdit;
    gwPassword_->setEchoMode(QLineEdit::Password);
    gwPassword_->setPlaceholderText(hadGatewayPassword_ ? tr("saved — leave blank to keep")
                                                        : tr("ask when connecting"));
    gwSave_ = new QCheckBox(tr("Remember gateway password"));
    gwSave_->setChecked(b_.gatewaySavePassword);
    gwSave_->setEnabled(secrets::available());

    form->addRow(gwEnabled_);
    form->addRow(tr("Gateway:"), gwAddress_);
    form->addRow(QString(), gwSame_);
    form->addRow(tr("User name:"), gwUser_);
    form->addRow(tr("Domain:"), gwDomain_);
    form->addRow(tr("Password:"), gwPassword_);
    form->addRow(QString(), gwSave_);
    connect(gwEnabled_, &QCheckBox::toggled, this, &EditDialog::updateEnabled);
    connect(gwSame_, &QCheckBox::toggled, this, &EditDialog::updateEnabled);
    return w;
}

QWidget* EditDialog::advancedTab() {
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    extra_ = new QLineEdit(b_.extraArgs);
    extra_->setPlaceholderText(tr("e.g. /drive:media,/run/media +multitouch"));
    form->addRow(tr("Extra FreeRDP arguments:"), extra_);
    form->addRow(QString(), hint(tr("Passed to FreeRDP as-is; see xfreerdp --help. Quote arguments "
                                    "that contain spaces.")));
    return w;
}

void EditDialog::updateEnabled() {
    const bool card = smartcardLogon_->isChecked();
    password_->setEnabled(!card);
    savePassword_->setEnabled(!card && secrets::available());
    if (card) smartcards_->setChecked(true);
    smartcards_->setEnabled(!card);
    width_->setEnabled(dispFixed_->isChecked());
    height_->setEnabled(dispFixed_->isChecked());
    gpu_->setEnabled(h264_->isChecked());

    const bool gw = gwEnabled_->isChecked();
    const bool own = gw && !gwSame_->isChecked();
    gwAddress_->setEnabled(gw);
    gwSame_->setEnabled(gw);
    gwUser_->setEnabled(own);
    gwDomain_->setEnabled(own);
    gwPassword_->setEnabled(own);
    gwSave_->setEnabled(own && secrets::available());
}

void EditDialog::accept() {
    if (address_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Enter the computer to connect to."));
        address_->setFocus();
        return;
    }
    if (gwEnabled_->isChecked() && gwAddress_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Enter the gateway address or turn it off."));
        return;
    }
    QDialog::accept();
}

Bookmark EditDialog::bookmark() const {
    Bookmark b = b_;
    splitHostPort(address_->text(), b.host, b.port, 3389);
    b.name = name_->text().trimmed();
    b.username = user_->text().trimmed();
    b.domain = domain_->text().trimmed();
    b.smartcardLogon = smartcardLogon_->isChecked();
    // A card PIN is never stored.
    b.savePassword = !b.smartcardLogon && savePassword_->isChecked() &&
                     (hadPassword_ || !password_->text().isEmpty());

    b.display = dispFull_->isChecked()    ? Bookmark::Display::Fullscreen
                : dispMulti_->isChecked() ? Bookmark::Display::AllMonitors
                : dispFixed_->isChecked() ? Bookmark::Display::Fixed
                                          : Bookmark::Display::Window;
    b.width = width_->value();
    b.height = height_->value();
    b.scale = scale_->currentData().toString();

    b.network = network_->currentData().toString();
    b.h264 = h264_->isChecked();
    b.gpuDecode = gpu_->isChecked();
    b.vsync = vsync_->isChecked();

    b.audio = audio_->currentData().toString();
    b.clipboard = clipboard_->isChecked();
    b.microphone = mic_->isChecked();
    b.shareHome = home_->isChecked();
    b.smartcards = smartcards_->isChecked();

    b.gateway = gwEnabled_->isChecked();
    splitHostPort(gwAddress_->text(), b.gatewayHost, b.gatewayPort, 443);
    b.gatewaySameCreds = gwSame_->isChecked();
    b.gatewayUser = gwUser_->text().trimmed();
    b.gatewayDomain = gwDomain_->text().trimmed();
    b.gatewaySavePassword = gwSave_->isChecked() && !b.gatewaySameCreds &&
                            (hadGatewayPassword_ || !gwPassword_->text().isEmpty());

    b.extraArgs = extra_->text().trimmed();
    return b;
}

QString EditDialog::password() const { return password_->text(); }
QString EditDialog::gatewayPassword() const { return gwPassword_->text(); }

// ---------------------------------------------------------------------------------------

CredentialsDialog::CredentialsDialog(const QString& target, const QString& user,
                                     const QString& domain, bool remember, const QString& error,
                                     QWidget* parent, bool pinOnly)
    : QDialog(parent) {
    setWindowTitle(tr("Sign in"));
    auto* v = new QVBoxLayout(this);

    auto* heading = new QLabel(tr("Sign in to <b>%1</b>").arg(target.toHtmlEscaped()));
    v->addWidget(heading);
    if (!error.isEmpty()) {
        auto* err = new QLabel(error);
        err->setWordWrap(true);
        QPalette pal = err->palette();
        pal.setColor(QPalette::WindowText, QColor(0xda, 0x44, 0x53));
        err->setPalette(pal);
        v->addWidget(err);
    }

    auto* form = new QFormLayout;
    user_ = new QLineEdit(user);
    domain_ = new QLineEdit(domain);
    domain_->setPlaceholderText(tr("optional"));
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    remember_ = new QCheckBox(tr("Remember password"));
    remember_->setChecked(remember);
    remember_->setEnabled(secrets::available());
    if (pinOnly) {
        user_->hide();
        domain_->hide();
        remember_->setChecked(false);
        remember_->hide();
        form->addRow(tr("Smart card PIN:"), password_);
    } else {
        form->addRow(tr("User name:"), user_);
        form->addRow(tr("Domain:"), domain_);
        form->addRow(tr("Password:"), password_);
        form->addRow(QString(), remember_);
    }
    v->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    (user.isEmpty() && !pinOnly ? user_ : password_)->setFocus();
    setMinimumWidth(380);
}

QString CredentialsDialog::username() const { return user_->text().trimmed(); }
QString CredentialsDialog::domain() const { return domain_->text().trimmed(); }
QString CredentialsDialog::password() const { return password_->text(); }
bool CredentialsDialog::remember() const { return remember_->isChecked(); }

} // namespace fastrdp
