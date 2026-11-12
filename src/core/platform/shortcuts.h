#pragma once

#include <optional>
#include <string>

namespace avel::platform {

// What a shell link (.lnk) runs.
struct ShortcutTarget {
    // The raw path, %VARIABLES% kept so they can be expanded for the shortcut's owner. Empty for an
    // advertised shortcut and for one that points at no file (a Control Panel item, say).
    std::wstring target;
    std::wstring arguments;
    // A Windows Installer (advertised) shortcut: it names a product and a component, not a path.
    bool advertised = false;
};

// Reads `file` as a shell link; nullopt if it can't be loaded as one. Needs COM on the calling
// thread.
std::optional<ShortcutTarget> resolve_shortcut(const std::wstring& file);

}  // namespace avel::platform
