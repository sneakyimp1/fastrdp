#pragma once

#include "bookmark.hpp"

#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QWidget;

namespace fastrdp {

// Create / edit a bookmark. Passwords typed here are returned separately and never
// stored in the bookmark itself.
class EditDialog : public QDialog {
    Q_OBJECT
public:
    EditDialog(const Bookmark& b, bool hasSavedPassword, bool hasSavedGatewayPassword,
               QWidget* parent = nullptr);

    Bookmark bookmark() const;
    // Empty means "unchanged" when a password was already saved.
    QString password() const;
    QString gatewayPassword() const;

    void accept() override;

private:
    QWidget* generalTab();
    QWidget* displayTab();
    QWidget* performanceTab();
    QWidget* resourcesTab();
    QWidget* gatewayTab();
    QWidget* advancedTab();
    void updateEnabled();

    Bookmark b_;
    bool hadPassword_, hadGatewayPassword_;

    QLineEdit *name_, *address_, *user_, *domain_, *password_;
    QCheckBox* savePassword_;

    QRadioButton *dispWindow_, *dispFull_, *dispFixed_;
    QSpinBox *width_, *height_;
    QComboBox* scale_;

    QComboBox* network_;
    QCheckBox *h264_, *gpu_, *vsync_;

    QComboBox* audio_;
    QCheckBox *mic_, *home_;

    QCheckBox* gwEnabled_;
    QLineEdit* gwAddress_;
    QCheckBox* gwSame_;
    QLineEdit *gwUser_, *gwDomain_, *gwPassword_;
    QCheckBox* gwSave_;

    QLineEdit* extra_;
};

// Asked at connect time when no password is saved, or after the server rejected one.
class CredentialsDialog : public QDialog {
    Q_OBJECT
public:
    CredentialsDialog(const QString& target, const QString& user, const QString& domain,
                      bool remember, const QString& error, QWidget* parent = nullptr);

    QString username() const;
    QString domain() const;
    QString password() const;
    bool remember() const;

private:
    QLineEdit *user_, *domain_, *password_;
    QCheckBox* remember_;
};

} // namespace fastrdp
