#pragma once

#include <windows.h>

#include <vector>

#include "core/inventory/diagnostic.h"
#include "core/inventory/program.h"
#include "core/inventory/uninstall_entry.h"

namespace avel::inventory {

struct RegistryScanResult {
    std::vector<Program> programs;
    std::vector<Diagnostic> diagnostics;
};

// Reads every Uninstall key Windows knows about:
//   HKLM (64-bit and 32-bit views), and for every user profile HKEY_USERS\<sid>\Software (plus its
//   rarely used WOW6432Node). Signed-out users' NTUSER.DAT hives are loaded privately and
//   read-only for the duration of the scan (needs elevation; otherwise reported as a diagnostic).
RegistryScanResult scan_registry();

// Parses all subkeys of an already opened Uninstall key. `context.location.key_name`, `.path` and
// `id_prefix` are completed per subkey (path = context.location.path + "\" + subkey name).
// Entries without an InstallDate get the date their key was last written, as in Settings.
// Exposed for tests, which point it at a fixture key.
void scan_uninstall_key(HKEY uninstall_key, const UninstallKeyContext& context, RegistryScanResult& result);

}  // namespace avel::inventory
