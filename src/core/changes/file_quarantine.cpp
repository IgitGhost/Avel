#include "core/changes/file_quarantine.h"

#include <wil/resource.h>

#include <algorithm>
#include <functional>

#include "core/platform/file_ops.h"
#include "core/platform/paths.h"

namespace avel::changes {

namespace {

// Calls `visit(relative, data)` for everything below `root`, parents before children. Links are
// reported but never entered.
void walk(const std::wstring& root, const std::wstring& relative,
          const std::function<void(const std::wstring&, const WIN32_FIND_DATAW&)>& visit) {
    const std::wstring folder = relative.empty() ? root : root + L"\\" + relative;
    WIN32_FIND_DATAW data{};
    wil::unique_hfind find(::FindFirstFileExW(platform::extended(folder + L"\\*").c_str(), FindExInfoBasic, &data,
                                              FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
        return;
    }
    do {
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring child = relative.empty() ? std::wstring(name) : relative + L"\\" + std::wstring(name);
        visit(child, data);
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            walk(root, child, visit);
        }
    } while (::FindNextFileW(find.get(), &data));
}

bool is_real_folder(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::wstring join(const std::wstring& root, const std::wstring& relative) {
    return relative.empty() ? root : root + L"\\" + relative;
}

bool exists(const std::wstring& path) {
    return ::GetFileAttributesW(platform::extended(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

DWORD rename_path(const std::wstring& from, const std::wstring& to) {
    const auto item = platform::open_item(from, DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES);
    if (!item) {
        return ::GetLastError();
    }
    return platform::rename_item(item.get(), to);
}

// Creates `path`'s missing parents as plain folders.
void create_parents(const std::wstring& path) {
    std::vector<std::wstring> missing;
    for (std::wstring parent = platform::parent_path(path);
         !parent.empty() && !platform::is_root_path(parent) && !exists(parent); parent = platform::parent_path(parent)) {
        missing.push_back(parent);
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        ::CreateDirectoryW(platform::extended(*it).c_str(), nullptr);
    }
}

bool in_use(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION;
}

void set_times(const std::wstring& path, const FILETIME& created, const FILETIME& written) {
    const auto item = platform::open_item(path, FILE_WRITE_ATTRIBUTES);
    if (item) {
        ::SetFileTime(item.get(), &created, nullptr, &written);
    }
}

std::string filetime_text(const FILETIME& time) {
    return std::to_string((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime);
}

FILETIME filetime_from(const Json& json, const char* field) {
    const std::uint64_t value = json.contains(field) && json[field].is_string()
                                    ? std::stoull(json[field].get_ref<const std::string&>())
                                    : 0;
    return {static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
}

}  // namespace

std::optional<FileMove> prepare_move(const std::wstring& path, bool directory, const std::wstring& quarantine,
                                     ItemStatus& failure) {
    FileMove move;
    move.original = platform::normalize_path(path);
    // Keeping the name: renaming a file to another name sets its archive attribute.
    move.quarantine = quarantine + L"\\" + std::wstring(platform::last_path_component(move.original));
    const auto item = platform::open_item(move.original, FILE_READ_ATTRIBUTES | READ_CONTROL);
    if (!item) {
        const DWORD error = ::GetLastError();
        failure = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                      ? ItemStatus{ItemState::Skipped, Reason::Missing, 0}
                      : ItemStatus{ItemState::Skipped, error == ERROR_ACCESS_DENIED ? Reason::AccessDenied : Reason::Error,
                                   error};
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const auto real = platform::final_path(item.get());
    if (!::GetFileInformationByHandle(item.get(), &information) || !real ||
        !platform::paths_equal(*real, platform::long_path(move.original))) {
        failure = {ItemState::Skipped, Reason::PathChanged, 0};
        return std::nullopt;
    }
    move.directory = is_real_folder(information.dwFileAttributes);
    move.attributes = information.dwFileAttributes;
    if (directory != move.directory && !(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        failure = {ItemState::Skipped, Reason::PathChanged, 0};  // A file where a folder was, or the reverse
        return std::nullopt;
    }
    const auto volume = platform::volume_root(move.original);
    const auto target_volume = platform::volume_root(move.quarantine);
    if (!volume || !platform::is_fixed_ntfs(*volume) || !target_volume ||
        !platform::paths_equal(*volume, *target_volume)) {
        failure = {ItemState::Skipped, Reason::UnsupportedVolume, 0};
        return std::nullopt;
    }
    if (!move.directory) {
        move.bytes = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
        return move;
    }
    move.folders.push_back({L"", information.dwFileAttributes, information.ftCreationTime,
                            information.ftLastWriteTime, platform::security_sddl(move.original)});
    walk(move.original, L"", [&](const std::wstring& relative, const WIN32_FIND_DATAW& data) {
        if (is_real_folder(data.dwFileAttributes)) {
            move.folders.push_back({relative, data.dwFileAttributes, data.ftCreationTime, data.ftLastWriteTime,
                                    platform::security_sddl(join(move.original, relative))});
        } else {
            move.bytes += (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)) {
                move.unarchived.push_back(relative);
            }
        }
    });
    return move;
}

MoveResult move_to_quarantine(const FileMove& move) {
    MoveResult result;
    create_parents(move.quarantine);
    const DWORD error = rename_path(move.original, move.quarantine);
    if (error == ERROR_SUCCESS) {
        result.status = {ItemState::Done, Reason::None, 0};
        return result;
    }
    const auto remove_container = [&] {
        ::RemoveDirectoryW(platform::extended(platform::parent_path(move.quarantine)).c_str());  // If empty
    };
    if (!move.directory || !in_use(error)) {
        remove_container();
        result.status = {ItemState::Failed, in_use(error) ? Reason::InUse : Reason::Error, error};
        return result;
    }

    // Something inside is in use: move what can be moved, file by file.
    for (const auto& folder : move.folders) {
        ::CreateDirectoryW(platform::extended(join(move.quarantine, folder.relative)).c_str(), nullptr);
    }
    std::size_t moved = 0;
    walk(move.original, L"", [&](const std::wstring& relative, const WIN32_FIND_DATAW& data) {
        if (is_real_folder(data.dwFileAttributes)) {
            return;
        }
        const std::wstring from = join(move.original, relative);
        if (rename_path(from, join(move.quarantine, relative)) == ERROR_SUCCESS) {
            ++moved;
        } else {
            result.left_behind.push_back(from);
        }
    });
    // Remove the folders that are empty now, deepest first.
    for (auto it = move.folders.rbegin(); it != move.folders.rend(); ++it) {
        ::RemoveDirectoryW(platform::extended(join(move.original, it->relative)).c_str());
    }
    if (moved == 0 && exists(move.original)) {
        platform::delete_tree(move.quarantine);  // Only the empty folders made above
        remove_container();
        result.status = {ItemState::Failed, Reason::InUse, error};
        return result;
    }
    result.status = exists(move.original) ? ItemStatus{ItemState::Partial, Reason::InUse, 0}
                                          : ItemStatus{ItemState::Done, Reason::None, 0};
    return result;
}

MoveResult restore_from_quarantine(const FileMove& move, bool partial) {
    MoveResult result;
    if (!exists(move.quarantine)) {
        result.status = {ItemState::RestoreFailed, Reason::BackupMissing, 0};
        return result;
    }
    if (!partial && exists(move.original)) {
        result.status = {ItemState::RestoreSkipped, Reason::Exists, 0};
        return result;
    }
    if (!exists(move.original)) {
        create_parents(move.original);
        const DWORD error = rename_path(move.quarantine, move.original);
        if (error == ERROR_SUCCESS) {
            ::RemoveDirectoryW(platform::extended(platform::parent_path(move.quarantine)).c_str());
            const auto info = platform::item_info(move.original);
            if (move.attributes != 0 && info && info->attributes != move.attributes) {
                ::SetFileAttributesW(platform::extended(move.original).c_str(),
                                     move.attributes & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
            }
            result.status = {ItemState::Restored, Reason::None, 0};
            return result;
        }
        if (!move.directory || move.folders.empty()) {
            result.status = {ItemState::RestoreFailed, Reason::Error, error};
            return result;
        }
    } else if (!move.directory || move.folders.empty()) {
        result.status = {ItemState::RestoreSkipped, Reason::Exists, 0};
        return result;
    }

    // A folder moved file by file (part of it stayed), or one that can't be renamed back whole:
    // recreate missing folders as they were, then move each file back.
    for (const auto& folder : move.folders) {
        const std::wstring path = join(move.original, folder.relative);
        if (!exists(path) && ::CreateDirectoryW(platform::extended(path).c_str(), nullptr)) {
            platform::apply_security_sddl(path, folder.sddl);
            constexpr DWORD kSettable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                        FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
            if (folder.attributes & kSettable) {
                ::SetFileAttributesW(platform::extended(path).c_str(), folder.attributes & kSettable);
            }
        }
    }
    std::size_t conflicts = 0;
    walk(move.quarantine, L"", [&](const std::wstring& relative, const WIN32_FIND_DATAW& data) {
        if (is_real_folder(data.dwFileAttributes)) {
            return;
        }
        const std::wstring to = join(move.original, relative);
        if (exists(to) || rename_path(join(move.quarantine, relative), to) != ERROR_SUCCESS) {
            ++conflicts;
            result.left_behind.push_back(to);
        } else if ((data.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) &&
                   std::find(move.unarchived.begin(), move.unarchived.end(), relative) != move.unarchived.end()) {
            ::SetFileAttributesW(platform::extended(to).c_str(), data.dwFileAttributes & ~FILE_ATTRIBUTE_ARCHIVE);
        }
    });
    // Moving files in touched the folders' times; put them back, deepest first, and drop the
    // emptied quarantine folders.
    for (auto it = move.folders.rbegin(); it != move.folders.rend(); ++it) {
        set_times(join(move.original, it->relative), it->created, it->written);
        ::RemoveDirectoryW(platform::extended(join(move.quarantine, it->relative)).c_str());
    }
    result.status = conflicts == 0 ? ItemStatus{ItemState::Restored, Reason::None, 0}
                                   : ItemStatus{ItemState::RestoreSkipped, Reason::Exists, 0};
    return result;
}

Json to_json(const FileMove& move) {
    Json json;
    put_text(json, "original", move.original);
    put_text(json, "quarantine", move.quarantine);
    json["directory"] = move.directory;
    json["attributes"] = move.attributes;
    json["bytes"] = move.bytes;
    json["folders"] = Json::array();
    for (const auto& folder : move.folders) {
        Json entry;
        put_text(entry, "relative", folder.relative);
        entry["attributes"] = folder.attributes;
        entry["created"] = filetime_text(folder.created);
        entry["written"] = filetime_text(folder.written);
        put_text(entry, "sddl", folder.sddl);
        json["folders"].push_back(std::move(entry));
    }
    json["unarchived"] = Json::array();
    for (const auto& relative : move.unarchived) {
        Json entry;
        put_text(entry, "path", relative);
        json["unarchived"].push_back(std::move(entry));
    }
    return json;
}

bool from_json(const Json& json, FileMove& move) {
    const auto original = get_text(json, "original");
    const auto quarantine = get_text(json, "quarantine");
    if (!json.is_object() || !original || !quarantine || !json.contains("folders") || !json["folders"].is_array()) {
        return false;
    }
    move = {};
    move.original = *original;
    move.quarantine = *quarantine;
    move.directory = json.value("directory", false);
    move.attributes = json.value("attributes", DWORD{0});
    move.bytes = json.value("bytes", std::uint64_t{0});
    for (const auto& entry : json["folders"]) {
        FileMove::Folder folder;
        folder.relative = get_text(entry, "relative").value_or(L"");
        folder.attributes = entry.value("attributes", DWORD{0});
        folder.created = filetime_from(entry, "created");
        folder.written = filetime_from(entry, "written");
        folder.sddl = get_text(entry, "sddl").value_or(L"");
        move.folders.push_back(std::move(folder));
    }
    if (json.contains("unarchived") && json["unarchived"].is_array()) {
        for (const auto& entry : json["unarchived"]) {
            if (const auto path = get_text(entry, "path")) {
                move.unarchived.push_back(*path);
            }
        }
    }
    return true;
}

}  // namespace avel::changes
