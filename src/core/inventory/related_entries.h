// Decides whether two inventory entries describe the same installed program.
//
// Ported from Bulk Crap Uninstaller (https://github.com/BCUninstaller/Bulk-Crap-Uninstaller),
// Copyright (c) 2017 Marcin Szeniak, Apache License 2.0:
//   UninstallTools/Factory/ApplicationEntryTools.cs (AreEntriesRelated)
//   UninstallTools/Factory/ApplicationUninstallerFactory.cs (MergeResults threshold)
#pragma once

#include "core/inventory/program.h"

namespace avel::inventory {

// A score of this much or more means "the same program".
inline constexpr int kRelatedThreshold = 1;
// Returned when the install location or the uninstall command is identical.
inline constexpr int kCertainlyRelated = 100;

// How likely `other` is the same program as `base`. Each piece of evidence (install folder,
// bitness, install date, version, publisher, name, ...) adds or subtracts points; evidence that
// can't be judged (missing or too short) counts as unsure.
// BCU's orphan and rating checks are left out: Avel has no orphan detection or ratings yet.
int relatedness_score(const Program& base, const Program& other);

}  // namespace avel::inventory
