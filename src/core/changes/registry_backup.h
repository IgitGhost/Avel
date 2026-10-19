#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace avel::changes {

struct ValueSnapshot {
    std::wstring name;  // Empty for the default value; may hold unpaired surrogates
    DWORD type = REG_NONE;
    std::vector<std::byte> data;
};

// A registry key and everything below it, exactly enough to recreate it: names, classes, values
// as raw bytes of their exact type and length (a missing default value stays missing, an empty one
// stays empty), and each key's security descriptor (owner, group and DACL).
struct KeySnapshot {
    std::wstring name;  // The key's own name, the last component of its path
    std::wstring class_name;
    std::vector<std::byte> security;  // Self-relative security descriptor
    std::vector<ValueSnapshot> values;
    std::vector<KeySnapshot> subkeys;
};

// Captures `parent\name`. Every key in the tree is opened for deletion as well as reading, so a
// tree that can't be deleted fails here, before anything has changed:
//   ERROR_FILE_NOT_FOUND    the key doesn't exist;
//   ERROR_ACCESS_DENIED     some key can't be read or deleted (not junk: never take ownership);
//   ERROR_CANT_ACCESS_FILE  the tree holds a registry symbolic link, which is never followed;
//   other Win32 errors as the registry reports them.
LSTATUS capture_key(HKEY parent, std::wstring_view name, KeySnapshot& snapshot);

// The same for a key something else deletes (a service's key goes with DeleteService): opened for
// reading only, and subkeys named in `skip` (directly below the key) are left out.
LSTATUS capture_key_readonly(HKEY parent, std::wstring_view name, const std::vector<std::wstring>& skip,
                             KeySnapshot& snapshot);

// Deletes `parent\name` and everything below it.
LSTATUS delete_key(HKEY parent, std::wstring_view name);

// Recreates `snapshot` as `parent\snapshot.name`. Returns ERROR_ALREADY_EXISTS, changing
// nothing, if the key exists: a reinstalled program's key is never overwritten. Keys under a
// volatile parent are recreated volatile. A key's security descriptor is applied after its values
// and subkeys, and only where it differs from what the new key inherited; a foreign owner needs
// SeRestorePrivilege, which is enabled when the process has it.
LSTATUS restore_key(HKEY parent, const KeySnapshot& snapshot);

// Adds what's missing: values and subkeys of `snapshot` that `parent\snapshot.name` lacks, creating
// the key if needed. Existing values are never overwritten. For a deletion that stopped part way,
// and for a key recreated by someone else (a service's key after CreateService).
LSTATUS merge_key(HKEY parent, const KeySnapshot& snapshot);

// The same for a single value of `key`.
LSTATUS capture_value(HKEY key, std::wstring_view name, ValueSnapshot& snapshot);
LSTATUS restore_value(HKEY key, const ValueSnapshot& snapshot);

// A .reg file (regedit's format, UTF-16 with a BOM) describing the snapshot, for people to read.
// `path` is the key's full path as regedit writes it ("HKEY_LOCAL_MACHINE\SOFTWARE\...").
std::wstring to_reg_text(std::wstring_view path, const KeySnapshot& snapshot);
std::wstring to_reg_text(std::wstring_view key_path, const ValueSnapshot& snapshot);

}  // namespace avel::changes
