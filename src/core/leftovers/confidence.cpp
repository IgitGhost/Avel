// Ported from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin Szeniak, Apache License 2.0.
// See confidence.h for the original source files.
#include "core/leftovers/confidence.h"

#include <algorithm>
#include <cstdlib>

namespace avel::leftovers {

int points(const Record& record) {
    switch (record.evidence) {
        case Evidence::UninstallerKey:
        case Evidence::InstallerRegistration:
            return 20;
        case Evidence::TracedInstallRoot:
            return 5;
        case Evidence::TracedValueUndo:
            return 3;
        case Evidence::TracedCreated:
        case Evidence::TracedValueRestore:
            return 1;
        case Evidence::ExplicitConnection:
        case Evidence::PathReference:
        case Evidence::CompanyNameMatch:
        case Evidence::AllSubdirsMatched:
            return 4;
        case Evidence::StartupMatched:
            return 6;
        case Evidence::ProductNamePerfectMatch:
            return 2;
        case Evidence::Depth:
            return 2 - std::abs(record.level) * 2;
        case Evidence::DirectlyInsideKnownFolder:
            return -1;
        case Evidence::ProductNameDodgyMatch:
        case Evidence::OutsideCommonLocations:
        case Evidence::ItemNameEqualsCompanyName:
        case Evidence::UsedBySimilarNamedApp:
        case Evidence::OtherUserData:
            return -2;
        case Evidence::QuestionableDirectoryName:
            return -3;
        case Evidence::ContainerHoldsMore:
            return -4;
        case Evidence::StartupIsRunOnce:
            return -5;
        case Evidence::DirectoryStillUsed:
        case Evidence::SharedDllStillUsed:
            return -7;
        case Evidence::StoreApp:
            return -10;
        case Evidence::NoOwner:
            return 0;
    }
    return 0;
}

bool is_trace_evidence(Evidence evidence) {
    return evidence == Evidence::TracedInstallRoot || evidence == Evidence::TracedCreated ||
           evidence == Evidence::TracedValueUndo || evidence == Evidence::TracedValueRestore ||
           evidence == Evidence::ContainerHoldsMore;
}

int score(std::span<const Record> records) {
    int total = 0;
    for (const auto& record : records) {
        total += points(record);
    }
    return total;
}

Confidence level(std::span<const Record> records) {
    if (records.empty()) {
        return Confidence::Unknown;
    }
    const int total = score(records);
    if (total < 0) {
        return Confidence::Bad;
    }
    if (total < 2) {
        return Confidence::Questionable;
    }
    return total < 5 ? Confidence::Good : Confidence::VeryGood;
}

void add_record(std::vector<Record>& records, Record record) {
    const bool present = std::any_of(records.begin(), records.end(), [&](const Record& existing) {
        return existing.evidence == record.evidence && existing.level == record.level &&
               existing.reference == record.reference;
    });
    if (!present) {
        records.push_back(std::move(record));
    }
}

std::string_view to_string(Evidence evidence) {
    switch (evidence) {
        case Evidence::UninstallerKey:
            return "uninstaller-key";
        case Evidence::InstallerRegistration:
            return "installer-registration";
        case Evidence::TracedInstallRoot:
            return "traced-install-root";
        case Evidence::TracedValueUndo:
            return "traced-value-undo";
        case Evidence::TracedCreated:
            return "traced-created";
        case Evidence::TracedValueRestore:
            return "traced-value-restore";
        case Evidence::ExplicitConnection:
            return "explicit-connection";
        case Evidence::PathReference:
            return "path-reference";
        case Evidence::CompanyNameMatch:
            return "company-name-match";
        case Evidence::AllSubdirsMatched:
            return "all-subdirs-matched";
        case Evidence::StartupMatched:
            return "startup-matched";
        case Evidence::ProductNamePerfectMatch:
            return "product-name-perfect-match";
        case Evidence::NoOwner:
            return "no-owner";
        case Evidence::Depth:
            return "depth";
        case Evidence::DirectlyInsideKnownFolder:
            return "directly-inside-known-folder";
        case Evidence::ProductNameDodgyMatch:
            return "product-name-dodgy-match";
        case Evidence::OutsideCommonLocations:
            return "outside-common-locations";
        case Evidence::ItemNameEqualsCompanyName:
            return "item-name-equals-company-name";
        case Evidence::UsedBySimilarNamedApp:
            return "used-by-similar-named-app";
        case Evidence::OtherUserData:
            return "other-user-data";
        case Evidence::QuestionableDirectoryName:
            return "questionable-directory-name";
        case Evidence::ContainerHoldsMore:
            return "container-holds-more";
        case Evidence::StartupIsRunOnce:
            return "startup-is-run-once";
        case Evidence::DirectoryStillUsed:
            return "directory-still-used";
        case Evidence::SharedDllStillUsed:
            return "shared-dll-still-used";
        case Evidence::StoreApp:
            return "store-app";
    }
    return "unknown";
}

std::string_view to_string(ReferenceKind kind) {
    switch (kind) {
        case ReferenceKind::None:
            return "none";
        case ReferenceKind::RunValue:
            return "run-value";
        case ReferenceKind::ServiceImage:
            return "service-image";
        case ReferenceKind::TaskAction:
            return "task-action";
        case ReferenceKind::AppPath:
            return "app-path";
        case ReferenceKind::SharedDll:
            return "shared-dll";
        case ReferenceKind::InstallerFolder:
            return "installer-folder";
        case ReferenceKind::MuiCache:
            return "mui-cache";
        case ReferenceKind::ShortcutTarget:
            return "shortcut-target";
        case ReferenceKind::ComServer:
            return "com-server";
        case ReferenceKind::FirewallApp:
            return "firewall-app";
        case ReferenceKind::PackageExternalLocation:
            return "package-external-location";
    }
    return "unknown";
}

std::string_view to_string(Confidence confidence) {
    switch (confidence) {
        case Confidence::Unknown:
            return "unknown";
        case Confidence::Bad:
            return "bad";
        case Confidence::Questionable:
            return "questionable";
        case Confidence::Good:
            return "good";
        case Confidence::VeryGood:
            return "very-good";
    }
    return "unknown";
}

}  // namespace avel::leftovers
