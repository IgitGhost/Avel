// How confident Avel is that an item is a leftover of a program, and why.
//
// The model is ported from Bulk Crap Uninstaller (https://github.com/BCUninstaller/Bulk-Crap-Uninstaller),
// Copyright (c) 2017 Marcin Szeniak, Apache License 2.0:
//   UninstallTools/Junk/Confidence/ConfidenceRecords.cs    (the points)
//   UninstallTools/Junk/Confidence/ConfidenceCollection.cs (the levels)
// Evidence marked "Avel" below is not in BCU.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace avel::leftovers {

enum class Evidence : std::uint8_t {
    UninstallerKey,             // +20 The program's own Uninstall key
    ExplicitConnection,         // +4  Named by the program's registration (its install folder, ...)
    PathReference,              // +4  (Avel) Points into the program's install folder; see ReferenceKind
    CompanyNameMatch,           // +4  The parent folder or key is named after the publisher
    AllSubdirsMatched,          // +4  The folder is, or will be, empty once the other leftovers are gone
    StartupMatched,             // +6  A startup entry (Run value, task, service) of the program
    ProductNamePerfectMatch,    // +2
    Depth,                      // 2 - 2 * level: how deep below a search root the item sits
    DirectlyInsideKnownFolder,  // -1
    ProductNameDodgyMatch,      // -2
    ItemNameEqualsCompanyName,  // -2  Looks like a whole publisher's folder, e.g. "Adobe" for Adobe AIR
    UsedBySimilarNamedApp,      // -2  Might belong to another program with a similar name
    OtherUserData,              // -2  (Avel) In another user's profile, for a machine-wide program
    QuestionableDirectoryName,  // -3  "install", "settings", "config", "data", ...
    StartupIsRunOnce,           // -5
    DirectoryStillUsed,         // -7  Another installed program lives in or below this folder
    StoreApp,                   // -10 Data of a packaged app, which Windows manages itself
};

// What points into the program's install folder, for PathReference and StartupMatched.
enum class ReferenceKind : std::uint8_t {
    None,
    RunValue,                 // Run / RunOnce
    ServiceImage,             // A service's ImagePath or ServiceDll
    TaskAction,               // A scheduled task's Exec action
    AppPath,                  // App Paths
    SharedDll,                // SharedDLLs
    InstallerFolder,          // Windows Installer's Folders list
    MuiCache,                 // Explorer's MuiCache
    ShortcutTarget,           // A .lnk file's target
    ComServer,                // A COM class's InprocServer32 / LocalServer32
    FirewallApp,              // A firewall rule's program
    PackageExternalLocation,  // A sparse package's external location
};

struct Record {
    Evidence evidence = Evidence::ExplicitConnection;
    std::int8_t level = 0;                          // For Evidence::Depth
    ReferenceKind reference = ReferenceKind::None;  // For PathReference and StartupMatched
    std::wstring detail;                            // What matched, e.g. the referencing value's path
};

enum class Confidence : std::uint8_t { Unknown, Bad, Questionable, Good, VeryGood };

int points(const Record& record);
int score(std::span<const Record> records);

// No records: Unknown. Then by score: below 0 Bad, below 2 Questionable, below 5 Good, else VeryGood.
Confidence level(std::span<const Record> records);

// Adds `record` unless the same kind of evidence is already there, so merging the findings of
// several scanners never counts one piece of evidence twice.
void add_record(std::vector<Record>& records, Record record);

std::string_view to_string(Evidence evidence);
std::string_view to_string(ReferenceKind kind);
std::string_view to_string(Confidence confidence);

}  // namespace avel::leftovers
