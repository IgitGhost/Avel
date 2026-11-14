#include "core/tools/windows_apps_data.h"

#include <array>

#include "core/platform/text.h"

namespace avel::tools {

namespace {

using C = AppCategory;
using W = AppWarning;

// A reviewed table, like the noise rules: change it only with a reason. Identity names as the
// packages declare them (case doesn't matter); a trailing '*' matches a prefix. Most of Windows' own
// packages are System-signed and protected by that alone; the critical ones are listed here too,
// since some (the Store, App Installer, Windows Security) aren't System-signed.
constexpr std::array kEntries{
    // Critical: Windows or other apps need them; never offered.
    AppDataEntry{L"Microsoft.WindowsStore", C::Critical},         // Installs and updates Store apps
    AppDataEntry{L"Microsoft.StorePurchaseApp", C::Critical},     // The Store's purchases and sign-in
    AppDataEntry{L"Microsoft.DesktopAppInstaller", C::Critical},  // App Installer: winget, .msix installs
    AppDataEntry{L"Microsoft.SecHealthUI", C::Critical},          // Windows Security
    AppDataEntry{L"windows.immersivecontrolpanel", C::Critical},  // Settings
    AppDataEntry{L"Microsoft.Windows.ShellExperienceHost", C::Critical},  // Taskbar flyouts, notifications
    AppDataEntry{L"Microsoft.Windows.StartMenuExperienceHost", C::Critical},  // The Start menu
    AppDataEntry{L"Microsoft.Windows.Search", C::Critical},                   // Search on the taskbar
    AppDataEntry{L"MicrosoftWindows.Client.CBS", C::Critical},  // The shell's components (Windows 11)
    AppDataEntry{L"MicrosoftWindows.Client.Core", C::Critical},
    AppDataEntry{L"MicrosoftWindows.Client.FileExp", C::Critical},  // File Explorer's new parts
    AppDataEntry{L"MicrosoftWindows.Client.Photon", C::Critical},
    AppDataEntry{L"MicrosoftWindows.Client.OOBE", C::Critical},
    AppDataEntry{L"MicrosoftWindows.Client.LKG", C::Critical},
    AppDataEntry{L"Microsoft.Windows.CloudExperienceHost", C::Critical},  // Sign-in and setup pages
    AppDataEntry{L"Microsoft.AAD.BrokerPlugin", C::Critical},             // Work and school account sign-in
    AppDataEntry{L"Microsoft.AccountsControl", C::Critical},              // Microsoft account sign-in
    AppDataEntry{L"Microsoft.CredDialogHost", C::Critical},               // Credential prompts
    AppDataEntry{L"Microsoft.LockApp", C::Critical},                      // The lock screen
    AppDataEntry{L"Microsoft.Win32WebViewHost", C::Critical},       // Web content in desktop apps' dialogs
    AppDataEntry{L"Microsoft.Windows.Apprep.ChxApp", C::Critical},  // SmartScreen
    AppDataEntry{L"Microsoft.Windows.ParentalControls", C::Critical},
    AppDataEntry{L"Microsoft.BioEnrollment", C::Critical},  // Windows Hello setup
    AppDataEntry{L"Microsoft.Windows.OOBENetworkConnectionFlow", C::Critical},
    AppDataEntry{L"Microsoft.Windows.OOBENetworkCaptivePortal", C::Critical},
    AppDataEntry{L"Windows.PrintDialog", C::Critical},
    AppDataEntry{L"Microsoft.UI.Xaml*", C::Critical},  // Frameworks other apps run on
    AppDataEntry{L"Microsoft.WindowsAppRuntime*", C::Critical},
    AppDataEntry{L"MicrosoftCorporationII.WinAppRuntime*", C::Critical},  // Its Main, Singleton, DDLM
    AppDataEntry{L"Microsoft.WinAppRuntime*", C::Critical},
    AppDataEntry{L"Microsoft.VCLibs*", C::Critical},
    AppDataEntry{L"Microsoft.NET.Native*", C::Critical},
    AppDataEntry{L"Microsoft.Services.Store.Engagement", C::Critical},

    // Semi-critical: offered, with what removing it breaks.
    AppDataEntry{L"Microsoft.Windows.Photos", C::SemiCritical, W::OpensFilesByDefault},
    AppDataEntry{L"Microsoft.WindowsNotepad", C::SemiCritical, W::OpensFilesByDefault},
    AppDataEntry{L"Microsoft.Paint", C::SemiCritical, W::OpensFilesByDefault},
    AppDataEntry{L"Microsoft.WindowsTerminal", C::SemiCritical, W::DefaultTerminal},
    AppDataEntry{L"Microsoft.MicrosoftEdge.Stable", C::SemiCritical, W::BrowserIntegration},
    AppDataEntry{L"Microsoft.XboxIdentityProvider", C::SemiCritical, W::GameSignIn},
    AppDataEntry{L"Microsoft.Xbox.TCUI", C::SemiCritical, W::GameSignIn},
    AppDataEntry{L"Microsoft.XboxGamingOverlay", C::SemiCritical, W::GameBar},
    AppDataEntry{L"Microsoft.XboxGameOverlay", C::SemiCritical, W::GameBar},
    AppDataEntry{L"Microsoft.GamingServices", C::SemiCritical, W::GamingServices},
    AppDataEntry{L"MicrosoftWindows.Client.WebExperience", C::SemiCritical, W::Widgets},
    AppDataEntry{L"Microsoft.WidgetsPlatformRuntime", C::SemiCritical, W::Widgets},
    AppDataEntry{L"Microsoft.ScreenSketch", C::SemiCritical, W::ScreenCapture},
    AppDataEntry{L"Microsoft.AV1VideoExtension", C::SemiCritical, W::MediaFormats},
    AppDataEntry{L"Microsoft.AVCEncoderVideoExtension", C::SemiCritical, W::MediaFormats},
    AppDataEntry{L"Microsoft.HEIFImageExtension", C::SemiCritical, W::MediaFormats},
    AppDataEntry{L"Microsoft.HEVCVideoExtension", C::SemiCritical, W::MediaFormats},
    AppDataEntry{L"Microsoft.MPEG2VideoExtension", C::SemiCritical, W::MediaFormats},
    AppDataEntry{L"Microsoft.RawImageExtension", C::SemiCritical, W::MediaFormats},
    AppDataEntry{L"Microsoft.VP9VideoExtensions", C::SemiCritical, W::MediaFormats},
    AppDataEntry{L"Microsoft.WebMediaExtensions", C::SemiCritical, W::MediaFormats},
    AppDataEntry{L"Microsoft.WebpImageExtension", C::SemiCritical, W::MediaFormats},
    AppDataEntry{L"Microsoft.OneDriveSync", C::SemiCritical, W::CloudFiles},
    AppDataEntry{L"Microsoft.Winget.Source", C::SemiCritical, W::WingetCatalog},
    AppDataEntry{L"Microsoft.LanguageExperiencePack*", C::SemiCritical, W::DisplayLanguage},
    AppDataEntry{L"Microsoft.ZuneMusic", C::SemiCritical, W::DefaultMediaPlayer},  // Media Player
    AppDataEntry{L"Microsoft.GamingApp", C::SemiCritical, W::GamePass},            // The Xbox app

    // Often removed: preinstalled apps many people don't use, first Microsoft's own.
    AppDataEntry{L"Clipchamp.Clipchamp", C::OftenRemoved},
    AppDataEntry{L"Microsoft.BingNews", C::OftenRemoved},
    AppDataEntry{L"Microsoft.BingWeather", C::OftenRemoved},
    AppDataEntry{L"Microsoft.BingFinance", C::OftenRemoved},
    AppDataEntry{L"Microsoft.BingSports", C::OftenRemoved},
    AppDataEntry{L"Microsoft.BingSearch", C::OftenRemoved},
    AppDataEntry{L"Microsoft.MicrosoftSolitaireCollection", C::OftenRemoved},
    AppDataEntry{L"Microsoft.GetHelp", C::OftenRemoved},
    AppDataEntry{L"Microsoft.Getstarted", C::OftenRemoved},  // Tips
    AppDataEntry{L"Microsoft.WindowsFeedbackHub", C::OftenRemoved},
    AppDataEntry{L"Microsoft.Todos", C::OftenRemoved},
    AppDataEntry{L"MSTeams", C::OftenRemoved},                      // The new Teams
    AppDataEntry{L"MicrosoftTeams", C::OftenRemoved},               // Teams (personal), Windows 11's first
    AppDataEntry{L"Microsoft.OutlookForWindows", C::OftenRemoved},  // The new Outlook
    AppDataEntry{L"microsoft.windowscommunicationsapps", C::OftenRemoved},  // Mail and Calendar
    AppDataEntry{L"Microsoft.People", C::OftenRemoved},
    AppDataEntry{L"Microsoft.Copilot", C::OftenRemoved},
    AppDataEntry{L"Microsoft.Windows.Ai.Copilot.Provider", C::OftenRemoved},
    AppDataEntry{L"Microsoft.549981C3F5F10", C::OftenRemoved},  // Cortana
    AppDataEntry{L"Microsoft.ZuneVideo", C::OftenRemoved},      // Films & TV
    AppDataEntry{L"Microsoft.YourPhone", C::OftenRemoved},      // Phone Link
    AppDataEntry{L"Microsoft.PowerAutomateDesktop", C::OftenRemoved},
    AppDataEntry{L"MicrosoftCorporationII.QuickAssist", C::OftenRemoved},
    AppDataEntry{L"MicrosoftCorporationII.MicrosoftFamily", C::OftenRemoved},
    AppDataEntry{L"Microsoft.Windows.DevHome", C::OftenRemoved},
    AppDataEntry{L"Microsoft.MicrosoftOfficeHub", C::OftenRemoved},  // Microsoft 365 (Office)
    AppDataEntry{L"Microsoft.Office.OneNote", C::OftenRemoved},      // OneNote for Windows 10
    AppDataEntry{L"Microsoft.SkypeApp", C::OftenRemoved},
    AppDataEntry{L"Microsoft.WindowsMaps", C::OftenRemoved},
    AppDataEntry{L"Microsoft.Microsoft3DViewer", C::OftenRemoved},
    AppDataEntry{L"Microsoft.MSPaint", C::OftenRemoved},  // Paint 3D
    AppDataEntry{L"Microsoft.Print3D", C::OftenRemoved},
    AppDataEntry{L"Microsoft.MixedReality.Portal", C::OftenRemoved},
    AppDataEntry{L"Microsoft.OneConnect", C::OftenRemoved},  // Mobile Plans
    AppDataEntry{L"Microsoft.Messaging", C::OftenRemoved},
    AppDataEntry{L"Microsoft.Wallet", C::OftenRemoved},
    AppDataEntry{L"Microsoft.XboxApp", C::OftenRemoved},  // Xbox Console Companion

    // Third-party promotions Windows installs by itself (the Start menu's "recommended" apps,
    // through the content delivery manager). A copy the user installed from the Store has the same
    // identity name, so it is checked too: the preset only checks, never removes, and nothing is
    // checked by default.
    AppDataEntry{L"king.com.*", C::OftenRemoved},  // King: Candy Crush Saga, Soda, Friends; Bubble Witch 3
    AppDataEntry{L"SpotifyAB.SpotifyMusic", C::OftenRemoved},               // Spotify (the promoted stub)
    AppDataEntry{L"Disney.37853FC22B2CE", C::OftenRemoved},                 // Disney+
    AppDataEntry{L"BytedancePte.Ltd.TikTok", C::OftenRemoved},              // TikTok
    AppDataEntry{L"Facebook.*", C::OftenRemoved},                           // Facebook, Instagram, Messenger
    AppDataEntry{L"4DF9E0F8.Netflix", C::OftenRemoved},                     // Netflix
    AppDataEntry{L"AmazonVideo.PrimeVideo", C::OftenRemoved},               // Prime Video
    AppDataEntry{L"7EE7776C.LinkedInforWindows", C::OftenRemoved},          // LinkedIn
    AppDataEntry{L"HULULLC.HULUPLUS", C::OftenRemoved},                     // Hulu
    AppDataEntry{L"9E2F88E3.Twitter", C::OftenRemoved},                     // Twitter (X)
    AppDataEntry{L"5A894077.McAfeeSecurity", C::OftenRemoved},              // McAfee Security (promotion)
    AppDataEntry{L"flaregamesGmbH.RoyalRevolt2", C::OftenRemoved},          // Royal Revolt 2
    AppDataEntry{L"A278AB0D.MarchofEmpires", C::OftenRemoved},              // Gameloft: March of Empires
    AppDataEntry{L"A278AB0D.DisneyMagicKingdoms", C::OftenRemoved},         // Gameloft: Disney Magic Kingdoms
    AppDataEntry{L"A278AB0D.DragonManiaLegends", C::OftenRemoved},          // Gameloft: Dragon Mania Legends
    AppDataEntry{L"GAMELOFTSA.Asphalt8Airborne", C::OftenRemoved},          // Asphalt 8
    AppDataEntry{L"828B5831.HiddenCityMysteryofShadows", C::OftenRemoved},  // Hidden City
    AppDataEntry{L"D52A8D61.FarmVille2CountryEscape", C::OftenRemoved},     // FarmVille 2
    AppDataEntry{L"Playtika.CaesarsSlotsFreeCasino", C::OftenRemoved},      // Caesars Slots
    AppDataEntry{L"PandoraMediaInc.29680B314EFC2", C::OftenRemoved},        // Pandora
    AppDataEntry{L"ShazamEntertainmentLtd.Shazam", C::OftenRemoved},        // Shazam
    AppDataEntry{L"Flipboard.Flipboard", C::OftenRemoved},                  // Flipboard
    AppDataEntry{L"AdobeSystemsIncorporated.AdobePhotoshopExpress", C::OftenRemoved},  // Photoshop Express
};

bool matches(const AppDataEntry& entry, std::wstring_view name) {
    if (entry.name.ends_with(L'*')) {
        return platform::istarts_with(name, entry.name.substr(0, entry.name.size() - 1));
    }
    return platform::iequals(name, entry.name);
}

const AppDataEntry* find_entry(std::wstring_view family_or_name) {
    // Identity names can't hold '_', so a family name's identity name ends at the first one.
    const std::wstring_view name = family_or_name.substr(0, family_or_name.find(L'_'));
    if (name.empty()) {
        return nullptr;
    }
    for (const AppDataEntry& entry : kEntries) {
        if (matches(entry, name)) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

AppCategory app_category(std::wstring_view family_or_name) {
    const AppDataEntry* entry = find_entry(family_or_name);
    return entry ? entry->category : AppCategory::Other;
}

AppWarning app_warning(std::wstring_view family_or_name) {
    const AppDataEntry* entry = find_entry(family_or_name);
    return entry && entry->category == AppCategory::SemiCritical ? entry->warning : AppWarning::None;
}

std::span<const AppDataEntry> app_data_entries() { return kEntries; }

std::string_view to_string(AppCategory category) {
    switch (category) {
        case AppCategory::Other:
            return "other";
        case AppCategory::OftenRemoved:
            return "often-removed";
        case AppCategory::SemiCritical:
            return "semi-critical";
        case AppCategory::Critical:
            return "critical";
    }
    return "unknown";
}

std::string_view to_string(AppWarning warning) {
    switch (warning) {
        case AppWarning::None:
            return "none";
        case AppWarning::OpensFilesByDefault:
            return "opens-files-by-default";
        case AppWarning::DefaultTerminal:
            return "default-terminal";
        case AppWarning::BrowserIntegration:
            return "browser-integration";
        case AppWarning::GameSignIn:
            return "game-sign-in";
        case AppWarning::GameBar:
            return "game-bar";
        case AppWarning::GamingServices:
            return "gaming-services";
        case AppWarning::Widgets:
            return "widgets";
        case AppWarning::ScreenCapture:
            return "screen-capture";
        case AppWarning::MediaFormats:
            return "media-formats";
        case AppWarning::CloudFiles:
            return "cloud-files";
        case AppWarning::WingetCatalog:
            return "winget-catalog";
        case AppWarning::DisplayLanguage:
            return "display-language";
        case AppWarning::DefaultMediaPlayer:
            return "default-media-player";
        case AppWarning::GamePass:
            return "game-pass";
    }
    return "unknown";
}

}  // namespace avel::tools
