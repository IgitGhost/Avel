#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform/command_line.h"

namespace avel::platform {

// Puts a path into one comparable form without touching the disk:
//   - surrounding whitespace and quotes removed, '/' turned into '\';
//   - the "\\?\" and "\??\" prefixes dropped ("\\?\UNC\server" becomes "\\server");
//   - repeated separators collapsed (except the leading "\\" of a UNC path), "." and ".."
//     segments resolved (never above the root);
//   - no trailing separator, except on a drive root, which is always "X:\".
// Case is kept; compare the results with iequals.
std::wstring normalize_path(std::wstring_view path);

// Both paths normalized and equal, ignoring case. Empty paths are never equal.
bool paths_equal(std::wstring_view a, std::wstring_view b);

// `path` is `ancestor` or lies inside it, respecting component boundaries: C:\Foo\Bar is under
// C:\Foo, but C:\Foobar is not. Both are normalized first; an empty ancestor contains nothing.
bool path_is_under(std::wstring_view path, std::wstring_view ancestor);

// path_is_under for paths that are already normalized, without the cost of normalizing them
// again (for hot loops over snapshots).
bool normalized_path_is_under(std::wstring_view path, std::wstring_view ancestor);

// Starts with a drive ("C:\") or a UNC share ("\\server\share").
bool is_absolute_path(std::wstring_view path);

// A drive root ("C:\") or a UNC share root ("\\server\share").
bool is_root_path(std::wstring_view path);

// Like path_is_under, but `path` must lie strictly inside `ancestor`.
bool path_is_inside(std::wstring_view path, std::wstring_view ancestor);

// The text after the last separator ("C:\A\B" -> "B"); the whole path if there is none.
std::wstring_view last_path_component(std::wstring_view path);

// The normalized parent folder ("C:\A\B" -> "C:\A", "C:\A" -> "C:\"); empty for a root.
std::wstring parent_path(std::wstring_view path);

// The normalized path with 8.3 short names expanded (GetLongPathNameW). Only works while the
// path exists; otherwise returns the normalized input.
std::wstring long_path(std::wstring_view path);

using Expander = std::function<std::wstring(std::wstring_view)>;

// Files a command line refers to, as normalized absolute paths where they can be determined:
//   - the executable, resolved like CreateProcess resolves unquoted paths with spaces;
//   - the forms service ImagePath values use: "\SystemRoot\...", "\??\C:\...", and paths
//     relative to the Windows folder such as "System32\drivers\x.sys";
//   - for rundll32, the DLL it loads ("rundll32.exe "C:\x.dll",Entry").
// %VARIABLES% are expanded with `expand` (expand_environment by default), so a caller can expand
// them for another user.
std::vector<std::wstring> referenced_paths(std::wstring_view command, const Expander& expand = {},
                                           const FileExistsFunction& file_exists = {});

}  // namespace avel::platform
