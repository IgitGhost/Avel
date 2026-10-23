#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <string>

#include "core/inventory/diagnostic.h"
#include "core/inventory/program.h"
#include "core/uninstall/batch.h"
#include "core/uninstall/uninstall_plan.h"
#include "core/uninstall/uninstaller.h"

namespace avel::app {

inline QString toQString(const std::wstring& text) { return QString::fromStdWString(text); }

// Sizes the way Explorer shows them: base 1024 with KB/MB/GB units, in the UI locale.
QString formatSize(std::uint64_t bytes);

// Translated, user-facing labels for the engine's codes. The engine never produces UI text.
class ProgramText {
    Q_DECLARE_TR_FUNCTIONS(ProgramText)

public:
    // "Store app", "MSI", "Inno Setup", ... ; "Desktop app" when the technology is unknown.
    static QString typeLabel(const inventory::Program& program);
    static QString architectureLabel(inventory::Architecture architecture);
    // "All users" or the owning account.
    static QString scopeLabel(const inventory::Program& program);
    static QString diagnosticText(const inventory::Diagnostic& diagnostic);
    // Why the entry can't be uninstalled the normal way (see inventory::is_broken), one line each.
    static QStringList brokenReasons(const inventory::Program& program);

    // How a plan will uninstall, e.g. "Silently (Inno Setup)"; `requested` tells whether quiet
    // mode was asked for, to explain a fallback to the uninstaller's window.
    static QString planText(const uninstall::UninstallPlan& plan, uninstall::Mode requested);
    static QString planErrorText(uninstall::PlanError error);
    static QString outcomeText(const uninstall::UninstallResult& result);
    // A Windows error code (Win32 or HRESULT) as the system describes it, with the code.
    static QString systemErrorText(std::uint32_t error);
};

}  // namespace avel::app
