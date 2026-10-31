#include "install_trace_dialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

QString cleanPath(const QString& text) {
    QString path = text.trimmed();
    // A path pasted from Explorer's "Copy as path" comes quoted.
    if (path.size() >= 2 && path.startsWith(u'"') && path.endsWith(u'"')) {
        path = path.mid(1, path.size() - 2);
    }
    return path;
}

}  // namespace

InstallTraceDialog::InstallTraceDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(u"installTraceDialog"_s);
    setWindowTitle(tr("Install and trace"));

    auto* intro = new QLabel(
        tr("Avel takes a snapshot of the registry (a few seconds), starts the installer, and records what it "
           "changes until the installation is over. Later, the program can be removed exactly by its trace.\n"
           "Close the programs you don't need first: what they change meanwhile is recorded too, and only "
           "what Avel recognizes as noise is set aside."),
        this);
    intro->setWordWrap(true);

    installerEdit_ = new QLineEdit(this);
    installerEdit_->setObjectName(u"installerPath"_s);
    installerEdit_->setAccessibleName(tr("Installer"));
    installerEdit_->setPlaceholderText(tr("The installer: a program or a .msi package"));
    installerEdit_->setClearButtonEnabled(true);
    connect(installerEdit_, &QLineEdit::textChanged, this, &InstallTraceDialog::onInstallerEdited);
    auto* browseButton = new QPushButton(tr("&Browse…"), this);
    browseButton->setObjectName(u"browseInstaller"_s);
    connect(browseButton, &QPushButton::clicked, this, &InstallTraceDialog::browse);
    auto* installerRow = new QHBoxLayout;
    installerRow->addWidget(installerEdit_, 1);
    installerRow->addWidget(browseButton);

    argumentsEdit_ = new QLineEdit(this);
    argumentsEdit_->setObjectName(u"installerArguments"_s);
    argumentsEdit_->setAccessibleName(tr("Arguments"));
    argumentsEdit_->setPlaceholderText(tr("Optional, e.g. /S or /qn"));

    descriptionEdit_ = new QLineEdit(this);
    descriptionEdit_->setObjectName(u"traceDescription"_s);
    descriptionEdit_->setAccessibleName(tr("Description"));
    descriptionEdit_->setPlaceholderText(tr("What is being installed"));
    connect(descriptionEdit_, &QLineEdit::textEdited, this, [this] { descriptionEdited_ = true; });

    auto* form = new QFormLayout;
    form->addRow(tr("&Installer:"), installerRow);
    form->addRow(tr("&Arguments:"), argumentsEdit_);
    form->addRow(tr("&Description:"), descriptionEdit_);

    auto* note =
        new QLabel(tr("A .msi package runs through Windows Installer. MSIX and AppX packages can't be "
                      "traced: Windows installs and removes them itself."),
                   this);
    note->setWordWrap(true);

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName(u"installTraceError"_s);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(u"color: #C04030"_s);
    errorLabel_->hide();

    auto* buttons = new QDialogButtonBox(this);
    startButton_ = buttons->addButton(tr("Install and trace"), QDialogButtonBox::AcceptRole);
    startButton_->setObjectName(u"startInstallTraceButton"_s);
    startButton_->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(errorLabel_);
    layout->addStretch();
    layout->addWidget(buttons);
    resize(640, 300);
    updateButtons();
}

std::wstring InstallTraceDialog::installer() const {
    return QDir::toNativeSeparators(QFileInfo(cleanPath(installerEdit_->text())).absoluteFilePath())
        .toStdWString();
}

std::wstring InstallTraceDialog::arguments() const { return argumentsEdit_->text().trimmed().toStdWString(); }

std::wstring InstallTraceDialog::description() const {
    return descriptionEdit_->text().trimmed().toStdWString();
}

void InstallTraceDialog::accept() {
    const QFileInfo file(cleanPath(installerEdit_->text()));
    if (!file.isFile()) {
        errorLabel_->setText(
            tr("There is no file %1.").arg(QDir::toNativeSeparators(installerEdit_->text().trimmed())));
        errorLabel_->show();
        installerEdit_->setFocus();
        return;
    }
    QDialog::accept();
}

void InstallTraceDialog::browse() {
    auto* dialog = new QFileDialog(this, tr("Choose the installer"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilters({tr("Installers (*.exe *.msi)"), tr("All files (*)")});
    const QFileInfo current(cleanPath(installerEdit_->text()));
    if (current.dir().exists() && !installerEdit_->text().trimmed().isEmpty()) {
        dialog->setDirectory(current.dir());
    }
    connect(dialog, &QFileDialog::fileSelected, this, [this](const QString& file) {
        installerEdit_->setText(QDir::toNativeSeparators(file));
        argumentsEdit_->setFocus();
    });
    dialog->open();
}

void InstallTraceDialog::onInstallerEdited(const QString& text) {
    errorLabel_->hide();
    if (!descriptionEdited_) {
        // The installer's name until the user types a description: "7z2408-x64".
        descriptionEdit_->setText(QFileInfo(cleanPath(text)).completeBaseName());
    }
    updateButtons();
}

void InstallTraceDialog::updateButtons() {
    startButton_->setEnabled(!cleanPath(installerEdit_->text()).isEmpty());
}

}  // namespace avel::app
