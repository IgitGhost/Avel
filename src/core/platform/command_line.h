#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace avel::platform {

struct CommandLine {
    std::wstring executable;  // Unquoted; may be relative, e.g. "MsiExec.exe"
    std::wstring arguments;   // Everything after the executable, leading whitespace removed
};

using FileExistsFunction = std::function<bool(const std::wstring& path)>;

// Splits a command line into executable and arguments, resolving unquoted paths with spaces the
// way CreateProcess does: for `C:\Program Files\App\uninst.exe /S` it probes `C:\Program`,
// `C:\Program Files\App\uninst.exe`, ... (each also with ".exe" appended) and picks the first that
// exists. When nothing exists (e.g. the uninstaller was deleted), the first prefix ending in
// ".exe" wins, then simply the first token.
CommandLine split_command_line(std::wstring_view command, const FileExistsFunction& file_exists);

// Same, probing the real file system.
CommandLine split_command_line(std::wstring_view command);

}  // namespace avel::platform
