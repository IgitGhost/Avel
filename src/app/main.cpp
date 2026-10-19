// avel.exe: the Avel GUI.

#include <QApplication>

#include <windows.h>
#include <objbase.h>

#include "core/platform/com_security.h"
#include "core/version.h"
#include "language_manager.h"
#include "main_window.h"

using namespace Qt::StringLiterals;

int main(int argc, char* argv[]) {
    // Process-wide COM security has to be set before anything uses COM (Qt does, for drag and
    // drop and the clipboard). System Restore needs it. The GUI thread is a single-threaded
    // apartment, as Qt expects.
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    avel::platform::initialize_com_security();

    QApplication app(argc, argv);
    QApplication::setOrganizationName(u"Avel"_s);
    QApplication::setApplicationName(u"Avel"_s);
    QApplication::setApplicationVersion(QString::fromLatin1(avel::kVersionString));

    avel::app::LanguageManager languages;
    languages.loadSavedPreference();

    avel::app::MainWindow window(languages);
    window.show();
    return QApplication::exec();
}
