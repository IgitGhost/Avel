// Every folder on an NTFS volume, read from its master file table with FSCTL_ENUM_USN_DATA: one
// sequential pass over the volume's index instead of a walk of its tree. See ADR 0005.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace avel::platform {

struct VolumeFolder {
    std::uint64_t id = 0;      // File reference number
    std::uint64_t parent = 0;  // The parent folder's file reference number
    std::wstring name;
    std::uint32_t attributes = 0;
};

class VolumeFolders {
public:
    VolumeFolders() = default;
    // From records (tests): `root` is the volume root ("D:\"), `root_id` its file reference number.
    VolumeFolders(std::wstring root, std::uint64_t root_id, std::vector<VolumeFolder> folders);

    // Reads the folders of the volume `root` ("C:\"). Needs administrator rights. Returns
    // ERROR_SUCCESS or the Win32 error (ERROR_ACCESS_DENIED without them).
    static std::uint32_t read(const std::wstring& root, VolumeFolders& result);

    const std::wstring& root() const { return root_; }
    const std::vector<VolumeFolder>& folders() const { return folders_; }

    // The folder's full path; empty if it doesn't lead up to the root (NTFS's own metadata).
    std::wstring path_of(std::size_t index) const;
    // The name of the folder's parent; empty for a folder in the root.
    std::wstring_view parent_name(std::size_t index) const;

private:
    std::wstring root_;
    std::uint64_t root_id_ = 0;
    std::vector<VolumeFolder> folders_;
    std::unordered_map<std::uint64_t, std::size_t> by_id_;
};

}  // namespace avel::platform
