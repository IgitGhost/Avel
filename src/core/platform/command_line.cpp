#include "core/platform/command_line.h"

#include <windows.h>

#include <vector>

#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::platform {

namespace {

bool has_extension(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    const auto dot = path.find_last_of(L'.');
    return dot != std::wstring_view::npos && (slash == std::wstring_view::npos || dot > slash);
}

CommandLine make(std::wstring_view executable, std::wstring_view rest) {
    return CommandLine{std::wstring(trim(executable)), std::wstring(trim(rest))};
}

// Splits arguments at whitespace; double quotes group, and are dropped.
std::vector<std::wstring> tokens_of(std::wstring_view arguments) {
    std::vector<std::wstring> tokens;
    std::wstring token;
    bool quoted = false;
    bool any = false;
    for (const wchar_t c : arguments) {
        if (c == L'"') {
            quoted = !quoted;
            any = true;
        } else if (!quoted && (c == L' ' || c == L'\t')) {
            if (any) {
                tokens.push_back(std::move(token));
                token.clear();
                any = false;
            }
        } else {
            token += c;
            any = true;
        }
    }
    if (any) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

// The file name without ".exe", lowercase: "powershell" for "C:\...\PowerShell.exe".
std::wstring host_name(std::wstring_view executable) {
    std::wstring name = to_lower(last_path_component(executable));
    if (name.ends_with(L".exe")) {
        name.resize(name.size() - 4);
    }
    return name;
}

// The script or DLL a Windows host runs, as written in the command; empty for other programs.
std::wstring hosted_file(const CommandLine& command) {
    const std::wstring host = host_name(command.executable);
    if (host == L"rundll32") {
        // rundll32 "C:\x.dll",Entry  or  rundll32 C:\x.dll,Entry
        const std::wstring_view arguments = trim(command.arguments);
        if (!arguments.empty() && arguments.front() == L'"') {
            const size_t end = arguments.find(L'"', 1);
            return std::wstring(trim(arguments.substr(1, end == std::wstring_view::npos ? end : end - 1)));
        }
        return std::wstring(trim(arguments.substr(0, arguments.find_first_of(L", \t"))));
    }
    const auto tokens = tokens_of(command.arguments);
    if (host == L"powershell" || host == L"pwsh") {
        // -File (or any abbreviation of it down to -f) names the script; the rest are its arguments.
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
            const std::wstring option = to_lower(tokens[i]);
            if (option.size() >= 2 && (option.front() == L'-' || option.front() == L'/') &&
                std::wstring_view(L"file").starts_with(std::wstring_view(option).substr(1))) {
                return tokens[i + 1];
            }
        }
        return {};
    }
    if (host == L"wscript" || host == L"cscript") {
        // Host options start with "//"; the first other argument is the script.
        for (const auto& token : tokens) {
            if (!token.starts_with(L"/")) {
                return token;
            }
        }
    }
    return {};
}

}  // namespace

CommandLine split_command_line(std::wstring_view command, const FileExistsFunction& file_exists) {
    command = trim(command);
    if (command.empty()) {
        return {};
    }

    if (command.front() == L'"') {
        const auto closing = command.find(L'"', 1);
        if (closing == std::wstring_view::npos) {
            return make(command.substr(1), {});
        }
        return make(command.substr(1, closing - 1), command.substr(closing + 1));
    }

    // Candidate split points: every space, then the end of the string.
    std::wstring_view first_exe_candidate;
    size_t first_exe_split = std::wstring_view::npos;
    for (size_t split = command.find(L' ');; split = command.find(L' ', split + 1)) {
        const size_t end = split == std::wstring_view::npos ? command.size() : split;
        const std::wstring_view candidate = trim(command.substr(0, end));
        if (!candidate.empty()) {
            const std::wstring path(candidate);
            if (file_exists(path)) {
                return make(candidate, command.substr(end));
            }
            if (!has_extension(candidate) && file_exists(path + L".exe")) {
                return CommandLine{path + L".exe", std::wstring(trim(command.substr(end)))};
            }
            if (first_exe_candidate.empty() && iends_with(candidate, L".exe")) {
                first_exe_candidate = candidate;
                first_exe_split = end;
            }
        }
        if (split == std::wstring_view::npos) {
            break;
        }
    }

    if (!first_exe_candidate.empty()) {
        return make(first_exe_candidate, command.substr(first_exe_split));
    }
    const auto space = command.find(L' ');
    if (space == std::wstring_view::npos) {
        return make(command, {});
    }
    return make(command.substr(0, space), command.substr(space));
}

CommandLine split_command_line(std::wstring_view command) {
    return split_command_line(command, [](const std::wstring& path) {
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    });
}

std::wstring missing_command_file(std::wstring_view command, const FileExistsFunction& file_exists) {
    const CommandLine parts = split_command_line(command, file_exists);
    if (parts.executable.empty()) {
        return {};
    }
    if (is_absolute_path(parts.executable) && !file_exists(parts.executable)) {
        return parts.executable;
    }
    const std::wstring file = hosted_file(parts);
    if (!file.empty() && is_absolute_path(file) && !file_exists(file)) {
        return file;
    }
    return {};
}

std::wstring missing_command_file(std::wstring_view command) {
    return missing_command_file(command, [](const std::wstring& path) {
        // Network shares and removable media aren't probed: that could take long, or need a disc.
        if (path.size() < 3 || path[1] != L':' || ::GetDriveTypeW(path.substr(0, 3).c_str()) != DRIVE_FIXED) {
            return true;
        }
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }
        const DWORD error = ::GetLastError();
        return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
    });
}

}  // namespace avel::platform
