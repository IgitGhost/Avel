#include "core/platform/paths.h"

#include <windows.h>

#include <algorithm>

#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::platform {

namespace {

std::wstring_view strip_quotes(std::wstring_view text) {
    text = trim(text);
    while (!text.empty() && text.front() == L'"') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == L'"') {
        text.remove_suffix(1);
    }
    return trim(text);
}

// Length of the root part of a path that uses only '\' separators: "C:\" (3), "\\server\share",
// "\" (1), or 0 for a relative path.
size_t root_length(std::wstring_view path) {
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        size_t end = path.find(L'\\', 2);  // after the server
        if (end == std::wstring_view::npos) {
            return path.size();
        }
        end = path.find(L'\\', end + 1);  // after the share
        return end == std::wstring_view::npos ? path.size() : end;
    }
    if (path.size() >= 2 && path[1] == L':') {
        return path.size() >= 3 && path[2] == L'\\' ? 3 : 2;
    }
    return !path.empty() && path[0] == L'\\' ? 1 : 0;
}

std::wstring windows_directory() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = ::GetWindowsDirectoryW(buffer, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring(L"C:\\Windows");
}

}  // namespace

std::wstring normalize_path(std::wstring_view input) {
    std::wstring path(strip_quotes(input));
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (istarts_with(path, LR"(\\?\UNC\)")) {
        path = L"\\\\" + path.substr(8);
    } else if (istarts_with(path, LR"(\\?\)") || istarts_with(path, LR"(\??\)")) {
        path.erase(0, 4);
    }
    if (path.empty()) {
        return path;
    }

    // Split off the root, normalizing it: "C:" and "C:\" both become "C:\".
    std::wstring root;
    std::wstring_view rest;
    const bool unc = path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
    if (unc) {
        // Collapse separators inside "\\server\\\share" before measuring the root.
        std::wstring tail = path.substr(2);
        tail.erase(std::unique(tail.begin(), tail.end(), [](wchar_t a, wchar_t b) { return a == L'\\' && b == a; }),
                   tail.end());
        path = L"\\\\" + tail;
        const size_t length = root_length(path);
        root = path.substr(0, length);
        while (!root.empty() && root.back() == L'\\') {
            root.pop_back();
        }
        rest = std::wstring_view(path).substr(length);
    } else if (path.size() >= 2 && path[1] == L':') {
        root = path.substr(0, 2) + L"\\";
        rest = std::wstring_view(path).substr(2);
    } else if (path[0] == L'\\') {
        root = L"\\";
        rest = std::wstring_view(path).substr(1);
    } else {
        rest = path;
    }

    std::vector<std::wstring_view> parts;
    while (!rest.empty()) {
        const size_t separator = rest.find(L'\\');
        const std::wstring_view part = rest.substr(0, separator);
        rest = separator == std::wstring_view::npos ? std::wstring_view() : rest.substr(separator + 1);
        if (part.empty() || part == L".") {
            continue;
        }
        if (part == L"..") {
            if (!parts.empty() && parts.back() != L"..") {
                parts.pop_back();
            } else if (root.empty()) {
                parts.push_back(part);  // A relative path may legitimately start with ".."
            }
            continue;
        }
        parts.push_back(part);
    }

    std::wstring result = root;
    for (const auto part : parts) {
        if (!result.empty() && result.back() != L'\\') {
            result += L'\\';
        }
        result += part;
    }
    return result;
}

bool paths_equal(std::wstring_view a, std::wstring_view b) {
    const std::wstring left = normalize_path(a);
    return !left.empty() && iequals(left, normalize_path(b));
}

bool path_is_under(std::wstring_view path, std::wstring_view ancestor) {
    return normalized_path_is_under(normalize_path(path), normalize_path(ancestor));
}

bool normalized_path_is_under(std::wstring_view path, std::wstring_view ancestor) {
    if (ancestor.empty() || path.size() < ancestor.size() || !istarts_with(path, ancestor)) {
        return false;
    }
    return path.size() == ancestor.size() || ancestor.back() == L'\\' || path[ancestor.size()] == L'\\';
}

bool is_absolute_path(std::wstring_view path) {
    const std::wstring normalized = normalize_path(path);
    if (normalized.size() >= 3 && normalized[1] == L':' && normalized[2] == L'\\') {
        return true;
    }
    // "\\server\share": a server and a share name.
    return normalized.size() > 2 && normalized[0] == L'\\' && normalized[1] == L'\\' &&
           normalized.find(L'\\', 2) != std::wstring::npos;
}

bool is_root_path(std::wstring_view path) {
    const std::wstring normalized = normalize_path(path);
    return is_absolute_path(normalized) && root_length(normalized) >= normalized.size();
}

bool path_is_inside(std::wstring_view path, std::wstring_view ancestor) {
    return path_is_under(path, ancestor) && !paths_equal(path, ancestor);
}

std::wstring_view last_path_component(std::wstring_view path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.remove_suffix(1);
    }
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? path : path.substr(separator + 1);
}

std::wstring parent_path(std::wstring_view path) {
    const std::wstring normalized = normalize_path(path);
    const size_t root = root_length(normalized);
    if (normalized.size() <= root) {
        return {};
    }
    const size_t separator = normalized.find_last_of(L'\\');
    if (separator == std::wstring::npos || separator < root) {
        // "C:\A" -> "C:\"; "\\server\share\A" -> "\\server\share"; "A" -> ""
        return normalized.substr(0, root);
    }
    return normalize_path(std::wstring_view(normalized).substr(0, separator));
}

std::wstring long_path(std::wstring_view path) {
    const std::wstring normalized = normalize_path(path);
    if (normalized.empty()) {
        return normalized;
    }
    const DWORD size = ::GetLongPathNameW(normalized.c_str(), nullptr, 0);
    if (size == 0) {
        return normalized;
    }
    std::wstring result(size, L'\0');
    const DWORD written = ::GetLongPathNameW(normalized.c_str(), result.data(), size);
    if (written == 0 || written >= size) {
        return normalized;
    }
    result.resize(written);
    return normalize_path(result);
}

std::vector<std::wstring> referenced_paths(std::wstring_view command, const Expander& expand,
                                           const FileExistsFunction& file_exists) {
    std::wstring text(trim(command));
    if (text.empty()) {
        return {};
    }
    text = expand ? expand(text) : expand_environment(text);

    const std::wstring windows = windows_directory();
    if (istarts_with(text, LR"(\SystemRoot\)")) {
        text = windows + text.substr(11);  // keeps the separator after "\SystemRoot"
    } else if (istarts_with(text, LR"(\??\)")) {
        text.erase(0, 4);
    } else if (istarts_with(text, LR"(System32\)") || istarts_with(text, LR"(SysWOW64\)")) {
        text = windows + L"\\" + text;
    }

    const CommandLine parts = file_exists ? split_command_line(text, file_exists) : split_command_line(text);
    std::vector<std::wstring> result;
    if (parts.executable.empty()) {
        return result;
    }
    std::wstring executable = parts.executable;
    if (executable.find_first_of(L"\\/") == std::wstring::npos) {
        // A bare name such as "rundll32.exe" or "MsiExec.exe" is found through the system folder.
        executable = windows + LR"(\System32\)" + executable;
    }
    result.push_back(normalize_path(executable));

    const std::wstring_view name = last_path_component(executable);
    if (iequals(name, L"rundll32.exe") || iequals(name, L"rundll32")) {
        // rundll32 "C:\x.dll",Entry  or  rundll32 C:\x.dll,Entry
        std::wstring_view arguments = trim(parts.arguments);
        std::wstring_view dll;
        if (!arguments.empty() && arguments.front() == L'"') {
            const size_t end = arguments.find(L'"', 1);
            dll = arguments.substr(1, end == std::wstring_view::npos ? std::wstring_view::npos : end - 1);
        } else {
            dll = arguments.substr(0, arguments.find(L','));
        }
        dll = trim(dll);
        if (!dll.empty()) {
            result.push_back(normalize_path(dll));
        }
    }
    return result;
}

}  // namespace avel::platform
