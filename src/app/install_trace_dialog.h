#pragma once

#include <QDialog>
#include <QString>

#include <string>

class QLabel;
class QLineEdit;
class QPushButton;

namespace avel::app {

// Install and trace…: the installer to run (a .msi runs through msiexec), its arguments and a
// description for the trace. Accepting it starts nothing; the caller runs the trace.
class InstallTraceDialog : public QDialog {
    Q_OBJECT

public:
    explicit InstallTraceDialog(QWidget* parent = nullptr);

    // After accept(): the installer's full path, its arguments, and the description.
    std::wstring installer() const;
    std::wstring arguments() const;
    std::wstring description() const;

    void accept() override;

private:
    void browse();
    void onInstallerEdited(const QString& text);
    void updateButtons();

    QLineEdit* installerEdit_ = nullptr;
    QLineEdit* argumentsEdit_ = nullptr;
    QLineEdit* descriptionEdit_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPushButton* startButton_ = nullptr;
    bool descriptionEdited_ = false;  // The user typed one: the installer's name no longer fills it
};

}  // namespace avel::app
