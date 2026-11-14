// What Avel knows about Windows' own packaged apps, as data (Phase 6b): which are critical (never
// offered), which are semi-critical (offered with a warning) and which are often removed (the
// "Check often removed" preset). Matched by the package's identity name, the part of the family
// name before its publisher id.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace avel::tools {

enum class AppCategory : std::uint8_t {
    Other,         // Nothing known: offered, never checked by a preset
    OftenRemoved,  // Offered, checked by the "often removed" preset
    SemiCritical,  // Offered with a warning: removing it breaks something people rely on
    Critical,      // Never offered: Windows needs it (the Store, App Installer, the shell's hosts...)
};

// What removing a semi-critical app breaks: the warning the front ends show for it. None for the
// other categories.
enum class AppWarning : std::uint8_t {
    None,
    OpensFilesByDefault,  // It opens a common file type by default (Photos, Notepad, Paint)
    DefaultTerminal,      // Console programs open in it (Windows Terminal)
    BrowserIntegration,   // Edge's package: Edge's integration with Windows and its web content
    GameSignIn,           // Games sign in to Xbox and show its overlays through it
    GameBar,              // Game Bar: recording, screenshots and performance overlays in games
    GamingServices,       // Games from the Store and Game Pass need it to start and update
    Widgets,              // The Widgets board, the taskbar's weather and the lock screen's widgets
    ScreenCapture,        // Snipping Tool: Win+Shift+S and the Print Screen key
    MediaFormats,         // A media extension: apps can't open or play its formats without it
    CloudFiles,           // OneDrive's integration with File Explorer (files on demand, status)
    WingetCatalog,        // The winget command's catalog of the Microsoft Store and winget sources
    DisplayLanguage,      // A language pack: Windows' display language may change back
    DefaultMediaPlayer,   // Media Player: music and videos open in it by default
    GamePass,             // The Xbox app: PC Game Pass games are installed, started and updated with it
};

// The category of a package family ("Microsoft.WindowsStore_8wekyb3d8bbwe") or identity name.
// Compared ignoring case.
AppCategory app_category(std::wstring_view family_or_name);

// The warning of a semi-critical app; None for any other.
AppWarning app_warning(std::wstring_view family_or_name);

// One row of the table: an identity name, or a prefix of one when it ends with '*'.
struct AppDataEntry {
    std::wstring_view name;
    AppCategory category = AppCategory::Other;
    AppWarning warning = AppWarning::None;
};

// The whole table, for tests and reviews.
std::span<const AppDataEntry> app_data_entries();

std::string_view to_string(AppCategory category);
std::string_view to_string(AppWarning warning);

}  // namespace avel::tools
