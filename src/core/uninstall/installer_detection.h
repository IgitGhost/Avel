#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/inventory/program.h"

namespace avel::uninstall {

// Recognizes the installer technology from an executable's embedded application manifest:
// NSIS stubs identify as "Nullsoft.NSIS.exehead", Inno Setup as "JR.Inno.Setup". Returns Unknown
// otherwise.
inventory::InstallerType installer_type_from_manifest(std::string_view manifest);

// The RT_MANIFEST resource of an executable, loaded as data (nothing is executed).
std::optional<std::string> read_embedded_manifest(const std::wstring& executable);

// Looks at the manifest first, then for the NSIS data signature ("NullsoftInst") in the file,
// which NSIS builds without the identifying manifest still have. Unknown if neither matches or
// the file is unreadable.
inventory::InstallerType detect_installer_type(const std::wstring& executable);

}  // namespace avel::uninstall
