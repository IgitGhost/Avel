// An independent dump of a file tree, to compare before and after a change: names, attributes,
// sizes, content hashes, timestamps, file ids (so hard links and renames show), alternate streams,
// security, and reparse tags of links (never followed). Shared by the unit tests and the Sandbox's
// avel_state_dump helper.
#pragma once

#include <windows.h>
#include <sddl.h>

#include <wil/resource.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "core/platform/text.h"

namespace avel::testing {

inline std::wstring long_name(const std::wstring& path) {
    return path.size() >= 248 && !path.starts_with(LR"(\\?\)") ? LR"(\\?\)" + path : path;
}

struct DumpOptions {
    bool folder_ids = true;  // Folders recreated file by file get new ids
};

inline std::string dump_item(const std::wstring& path, const std::wstring& relative, const DumpOptions& options) {
    wil::unique_hfile item(::CreateFileW(long_name(path).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES | FILE_READ_DATA,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!item) {
        return std::format("{} <unreadable {}>\n", platform::to_utf8(relative), ::GetLastError());
    }
    BY_HANDLE_FILE_INFORMATION info{};
    ::GetFileInformationByHandle(item.get(), &info);
    const bool folder = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool link = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    std::string text = std::format("{} attr={:x} created={:x}{:08x} written={:x}{:08x}", platform::to_utf8(relative),
                                   info.dwFileAttributes, info.ftCreationTime.dwHighDateTime,
                                   info.ftCreationTime.dwLowDateTime, info.ftLastWriteTime.dwHighDateTime,
                                   info.ftLastWriteTime.dwLowDateTime);
    if (!folder || options.folder_ids) {
        text += std::format(" id={:x}{:08x} links={}", info.nFileIndexHigh, info.nFileIndexLow, info.nNumberOfLinks);
    }
    if (!folder && !link) {
        std::uint64_t hash = 1469598103934665603ull;
        char buffer[4096];
        DWORD read = 0;
        std::uint64_t size = 0;
        while (::ReadFile(item.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            for (DWORD i = 0; i < read; ++i) {
                hash = (hash ^ static_cast<unsigned char>(buffer[i])) * 1099511628211ull;
            }
            size += read;
        }
        text += std::format(" size={} hash={:016x}", size, hash);
        WIN32_FIND_STREAM_DATA stream{};
        wil::unique_hfind streams(::FindFirstStreamW(long_name(path).c_str(), FindStreamInfoStandard, &stream, 0));
        if (streams) {
            do {
                text += std::format(" stream={}:{}", platform::to_utf8(stream.cStreamName), stream.StreamSize.QuadPart);
            } while (::FindNextStreamW(streams.get(), &stream));
        }
    }
    DWORD size = 0;
    ::GetKernelObjectSecurity(item.get(), OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                              DACL_SECURITY_INFORMATION,
                              nullptr, 0, &size);
    std::vector<std::byte> security(size);
    wil::unique_hlocal_string sddl;
    if (size && ::GetKernelObjectSecurity(item.get(),
                                          OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                              DACL_SECURITY_INFORMATION,
                                          security.data(), size, &size) &&
        ::ConvertSecurityDescriptorToStringSecurityDescriptorW(
            security.data(), SDDL_REVISION_1,
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &sddl, nullptr)) {
        text += " sd=" + platform::to_utf8(sddl.get());
    }
    return text + "\n";
}

// Everything below `root` (and `root` itself), sorted by path. Links are listed, never entered.
inline std::string dump_tree(const std::wstring& root, const DumpOptions& options = {}) {
    if (::GetFileAttributesW(long_name(root).c_str()) == INVALID_FILE_ATTRIBUTES) {
        return "<missing>";
    }
    std::vector<std::string> lines{dump_item(root, L".", options)};
    std::vector<std::wstring> pending{L""};
    while (!pending.empty()) {
        const std::wstring relative = pending.back();
        pending.pop_back();
        const std::wstring folder = relative.empty() ? root : root + L"\\" + relative;
        WIN32_FIND_DATAW data{};
        wil::unique_hfind find(::FindFirstFileW(long_name(folder + L"\\*").c_str(), &data));
        if (!find) {
            continue;
        }
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = relative.empty() ? name : relative + L"\\" + name;
            lines.push_back(dump_item(root + L"\\" + child, child, options));
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                pending.push_back(child);
            }
        } while (::FindNextFileW(find.get(), &data));
    }
    std::sort(lines.begin(), lines.end());
    std::string text;
    for (const auto& line : lines) {
        text += line;
    }
    return text;
}

}  // namespace avel::testing
