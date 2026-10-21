#include "core/platform/volume_folders.h"

#include <windows.h>
#include <winioctl.h>

#include <wil/resource.h>

#include <cstring>

#include "core/platform/paths.h"

namespace avel::platform {

namespace {

constexpr std::size_t kMaxDepth = 512;

}  // namespace

VolumeFolders::VolumeFolders(std::wstring root, std::uint64_t root_id, std::vector<VolumeFolder> folders)
    : root_(normalize_path(root)), root_id_(root_id), folders_(std::move(folders)) {
    by_id_.reserve(folders_.size());
    for (std::size_t i = 0; i < folders_.size(); ++i) {
        by_id_.emplace(folders_[i].id, i);
    }
}

std::uint32_t VolumeFolders::read(const std::wstring& root, VolumeFolders& result) {
    // The root folder's file reference number, which every top-level folder names as its parent.
    const std::wstring root_path = normalize_path(root);
    wil::unique_hfile root_folder(::CreateFileW(root_path.c_str(), FILE_READ_ATTRIBUTES,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!root_folder || !::GetFileInformationByHandle(root_folder.get(), &info)) {
        return ::GetLastError();
    }
    const std::uint64_t root_id =
        (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;

    // "C:\" -> "\\.\C:"
    const std::wstring device = L"\\\\.\\" + root_path.substr(0, 2);
    wil::unique_hfile volume(::CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr, OPEN_EXISTING, 0, nullptr));
    if (!volume) {
        return ::GetLastError();
    }
    std::vector<VolumeFolder> folders;
    std::vector<std::byte> buffer(1 << 20);
    MFT_ENUM_DATA_V0 range{0, 0, MAXLONGLONG};
    while (true) {
        DWORD bytes = 0;
        if (!::DeviceIoControl(volume.get(), FSCTL_ENUM_USN_DATA, &range, sizeof(range), buffer.data(),
                               static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_HANDLE_EOF) {
                return error;
            }
            break;
        }
        if (bytes < sizeof(USN)) {
            break;
        }
        // The buffer starts with where the next call continues, then holds whole records.
        std::memcpy(&range.StartFileReferenceNumber, buffer.data(), sizeof(USN));
        for (DWORD offset = sizeof(USN); offset + sizeof(USN_RECORD_V2) <= bytes;) {
            const auto* record = reinterpret_cast<const USN_RECORD_V2*>(buffer.data() + offset);
            if (record->RecordLength == 0) {
                break;
            }
            if (record->MajorVersion == 2 && (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                const auto* name = reinterpret_cast<const wchar_t*>(
                    reinterpret_cast<const std::byte*>(record) + record->FileNameOffset);
                folders.push_back({record->FileReferenceNumber, record->ParentFileReferenceNumber,
                                   std::wstring(name, record->FileNameLength / sizeof(wchar_t)),
                                   record->FileAttributes});
            }
            offset += record->RecordLength;
        }
    }
    result = VolumeFolders(root_path, root_id, std::move(folders));
    return ERROR_SUCCESS;
}

std::wstring VolumeFolders::path_of(std::size_t index) const {
    std::vector<const std::wstring*> names;
    for (std::size_t current = index; names.size() < kMaxDepth;) {
        const auto& folder = folders_[current];
        if (folder.id == root_id_) {
            break;  // The root's own record
        }
        names.push_back(&folder.name);
        if (folder.parent == root_id_) {
            std::wstring path = root_;
            for (auto name = names.rbegin(); name != names.rend(); ++name) {
                if (path.back() != L'\\') {
                    path += L'\\';
                }
                path += **name;
            }
            return path;
        }
        const auto parent = by_id_.find(folder.parent);
        if (parent == by_id_.end()) {
            return {};
        }
        current = parent->second;
    }
    return {};
}

std::wstring_view VolumeFolders::parent_name(std::size_t index) const {
    const auto parent = by_id_.find(folders_[index].parent);
    if (parent == by_id_.end() || folders_[parent->second].id == root_id_) {
        return {};
    }
    return folders_[parent->second].name;
}

}  // namespace avel::platform
