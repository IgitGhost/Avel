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

// The script or DLL a Windows host runs, as written in the command: `rundll32 <dll>,<entry>`,
// `powershell -File <script>` (or pwsh, -File abbreviated down to -f), `wscript`/`cscript <script>`.
// Empty for other programs, and when the host's arguments name no file.
std::wstring hosted_file(const CommandLine& command);

// The file a command can't run without, if it is known to be missing: the executable when the
// command names it by full path, or the script or DLL a Windows host would run
// (`powershell -File <script>`, `wscript`/`cscript <script>`, `rundll32 <dll>,<entry>`). Empty when
// the file is there or that can't be told (a bare name like "msiexec.exe", a relative path).
// `file_exists` answers true for a file that exists or can't be checked.
std::wstring missing_command_file(std::wstring_view command, const FileExistsFunction& file_exists);

// Same, probing the real file system: only "not found" on a fixed disk counts as missing, not
// "access denied", and network shares and removable media aren't probed.
std::wstring missing_command_file(std::wstring_view command);

}  // namespace avel::platform
