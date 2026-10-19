#include "core/platform/command_line.h"

#include <windows.h>

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

}  // namespace avel::platform
