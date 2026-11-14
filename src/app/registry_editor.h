#pragma once

#include <QString>

#include "core/platform/registry_address.h"

namespace avel::app {

// The key as Registry Editor's address bar writes it ("HKEY_LOCAL_MACHINE\...", "HKEY_CURRENT_USER\..."
// for the user running Avel, "HKEY_USERS\<SID>\..." for another signed-in user), or empty when the
// key's hive isn't loaded where Registry Editor sees it (a signed-out user's).
QString regeditPath(const platform::RegistryAddress& key);

// Opens Registry Editor at `path` (from regeditPath) in a new window. Registry Editor opens at the
// key it last showed, kept in its LastKey value, so that value is written first.
void openRegistryEditor(const QString& path);

}  // namespace avel::app
