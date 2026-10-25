#include "core/platform/usn_journal.h"

#include <winioctl.h>

#include <cstring>
#include <vector>

#include "core/platform/elevation.h"
#include "core/platform/paths.h"

namespace avel::platform {

namespace {

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr std::size_t kReadBufferSize = 256 * 1024;

// "C:\" -> "\\.\C:"
std::wstring device_name(const std::wstring& root) { return L"\\\\.\\" + root.substr(0, 2); }

template <typename Source>
void copy_common_fields(const Source& source, UsnRecord& record) {
    record.usn = source.Usn;
    record.time = source.TimeStamp.QuadPart;
    record.reason = source.Reason;
    record.source_info = source.SourceInfo;
    record.attributes = source.FileAttributes;
}

// One record; `bytes` spans exactly its RecordLength. False for a version other than 2 or 3, an
// id that needs more than 64 bits, or a name outside the record.
bool parse_record(std::span<const std::byte> bytes, UsnRecord& record) {
    USN_RECORD_COMMON_HEADER header{};
    if (bytes.size() < sizeof(header)) {
        return false;
    }
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::size_t fixed_size = 0;
    std::size_t name_offset = 0;
    std::size_t name_length = 0;
    if (header.MajorVersion == 2) {
        fixed_size = offsetof(USN_RECORD_V2, FileName);
        if (bytes.size() < fixed_size) {
            return false;
        }
        USN_RECORD_V2 source{};
        std::memcpy(&source, bytes.data(), fixed_size);
        copy_common_fields(source, record);
        record.frn = source.FileReferenceNumber;
        record.parent_frn = source.ParentFileReferenceNumber;
        name_offset = source.FileNameOffset;
        name_length = source.FileNameLength;
    } else if (header.MajorVersion == 3) {
        fixed_size = offsetof(USN_RECORD_V3, FileName);
        if (bytes.size() < fixed_size) {
            return false;
        }
        USN_RECORD_V3 source{};
        std::memcpy(&source, bytes.data(), fixed_size);
        // NTFS ids fit in the low 64 bits; anything else (ReFS) can't be opened by a 64-bit id.
        std::uint64_t id[2] = {};
        std::uint64_t parent[2] = {};
        std::memcpy(id, source.FileReferenceNumber.Identifier, sizeof(id));
        std::memcpy(parent, source.ParentFileReferenceNumber.Identifier, sizeof(parent));
        if (id[1] != 0 || parent[1] != 0) {
            return false;
        }
        copy_common_fields(source, record);
        record.frn = id[0];
        record.parent_frn = parent[0];
        name_offset = source.FileNameOffset;
        name_length = source.FileNameLength;
    } else {
        return false;
    }
    if (name_offset < fixed_size || name_length % sizeof(wchar_t) != 0 || name_offset > bytes.size() ||
        name_length > bytes.size() - name_offset) {
        return false;
    }
    record.name = std::wstring_view(reinterpret_cast<const wchar_t*>(bytes.data() + name_offset),
                                    name_length / sizeof(wchar_t));
    return true;
}

}  // namespace

std::int64_t parse_usn_buffer(std::span<const std::byte> buffer,
                              const std::function<void(const UsnRecord&)>& sink) {
    if (buffer.size() < sizeof(USN)) {
        return 0;
    }
    std::int64_t next = 0;
    std::memcpy(&next, buffer.data(), sizeof(USN));
    for (std::size_t offset = sizeof(USN); buffer.size() - offset >= sizeof(USN_RECORD_COMMON_HEADER);) {
        USN_RECORD_COMMON_HEADER header{};
        std::memcpy(&header, buffer.data() + offset, sizeof(header));
        if (header.RecordLength == 0 || header.RecordLength > buffer.size() - offset) {
            break;
        }
        UsnRecord record;
        if (parse_record(buffer.subspan(offset, header.RecordLength), record)) {
            sink(record);
        }
        offset += header.RecordLength;
    }
    return next;
}

std::optional<UsnVolume> UsnVolume::open(const std::wstring& root, std::uint32_t& error) {
    UsnVolume volume;
    volume.root_ = normalize_path(root);
    if (volume.root_.size() != 3 || volume.root_[1] != L':') {
        error = ERROR_INVALID_PARAMETER;
        return std::nullopt;
    }
    // Opening items by id anywhere on the volume, past folders the account can't list.
    static const bool backup = enable_privilege(SE_BACKUP_NAME);
    (void)backup;

    wchar_t guid_path[MAX_PATH] = {};
    DWORD serial = 0;
    if (!::GetVolumeNameForVolumeMountPointW(volume.root_.c_str(), guid_path, MAX_PATH) ||
        !::GetVolumeInformationW(volume.root_.c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        error = ::GetLastError();
        return std::nullopt;
    }
    volume.guid_path_ = guid_path;
    volume.serial_ = serial;

    // Every top-level item names the root folder's reference number as its parent.
    volume.root_folder_.reset(::CreateFileW(volume.root_.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!volume.root_folder_ || !::GetFileInformationByHandle(volume.root_folder_.get(), &info)) {
        error = ::GetLastError();
        return std::nullopt;
    }
    volume.root_frn_ = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;

    volume.volume_.reset(::CreateFileW(device_name(volume.root_).c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                       nullptr));
    if (!volume.volume_) {
        error = ::GetLastError();
        return std::nullopt;
    }
    error = ERROR_SUCCESS;
    return volume;
}

std::uint32_t UsnVolume::query(JournalState& state) const {
    USN_JOURNAL_DATA_V2 data{};
    DWORD bytes = 0;
    if (!::DeviceIoControl(volume_.get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &bytes,
                           nullptr)) {
        return ::GetLastError();
    }
    state = {data.UsnJournalID, data.FirstUsn, data.NextUsn, data.LowestValidUsn, data.MaximumSize};
    return ERROR_SUCCESS;
}

std::uint32_t UsnVolume::create(std::uint64_t max_size, std::uint64_t allocation_delta) const {
    CREATE_USN_JOURNAL_DATA data{max_size, allocation_delta};
    DWORD bytes = 0;
    if (::DeviceIoControl(volume_.get(), FSCTL_CREATE_USN_JOURNAL, &data, sizeof(data), nullptr, 0, &bytes,
                          nullptr)) {
        return ERROR_SUCCESS;
    }
    const DWORD error = ::GetLastError();
    if (error != ERROR_ACCESS_DENIED) {
        return error;
    }
    // Creating a journal may need a volume handle opened for writing.
    wil::unique_hfile writable(::CreateFileW(device_name(root_).c_str(), GENERIC_READ | GENERIC_WRITE,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                             nullptr));
    if (!writable) {
        return ::GetLastError();
    }
    return ::DeviceIoControl(writable.get(), FSCTL_CREATE_USN_JOURNAL, &data, sizeof(data), nullptr, 0,
                             &bytes, nullptr)
               ? ERROR_SUCCESS
               : ::GetLastError();
}

std::uint32_t UsnVolume::read(std::uint64_t journal_id, std::int64_t from, std::int64_t until,
                              const std::function<void(const UsnRecord&)>& sink,
                              std::int64_t& reached) const {
    READ_USN_JOURNAL_DATA_V1 request{};
    request.ReasonMask = 0xFFFFFFFF;
    request.ReturnOnlyOnClose = FALSE;
    request.Timeout = 0;
    request.BytesToWaitFor = 0;  // Return at once, also when there is nothing to read
    request.UsnJournalID = journal_id;
    request.MinMajorVersion = 2;
    request.MaxMajorVersion = 3;
    std::vector<std::byte> buffer(kReadBufferSize);
    reached = from;
    while (reached < until) {
        request.StartUsn = reached;
        DWORD bytes = 0;
        if (!::DeviceIoControl(volume_.get(), FSCTL_READ_USN_JOURNAL, &request, sizeof(request),
                               buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
            const DWORD error = ::GetLastError();
            return error == ERROR_HANDLE_EOF ? ERROR_SUCCESS : error;
        }
        // Records at or past `until` are left for the next read, which starts at the first of them.
        std::optional<std::int64_t> beyond;
        const std::int64_t start = reached;
        const std::int64_t next =
            parse_usn_buffer(std::span(buffer.data(), bytes), [&](const UsnRecord& record) {
                if (record.usn >= until) {
                    if (!beyond) {
                        beyond = record.usn;
                    }
                } else if (record.usn >= start) {
                    sink(record);
                }
            });
        if (beyond) {
            reached = *beyond;
            break;
        }
        if (next <= reached) {
            break;  // Nothing more has been written
        }
        reached = next;
    }
    return ERROR_SUCCESS;
}

wil::unique_hfile UsnVolume::open_by_id(std::uint64_t frn) const {
    FILE_ID_DESCRIPTOR id{};
    id.dwSize = sizeof(id);
    id.Type = FileIdType;
    id.FileId.QuadPart = static_cast<LONGLONG>(frn);
    return wil::unique_hfile(::OpenFileById(root_folder_.get(), &id, FILE_READ_ATTRIBUTES, kShareAll, nullptr,
                                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
}

std::optional<std::pair<std::uint64_t, std::wstring>> UsnVolume::name_of(std::uint64_t frn) const {
    const auto item = open_by_id(frn);
    if (!item) {
        return std::nullopt;
    }
    // A record with a name of up to 255 characters fits easily.
    std::vector<std::byte> buffer(4096);
    READ_FILE_USN_DATA versions{2, 3};
    DWORD bytes = 0;
    if (!::DeviceIoControl(item.get(), FSCTL_READ_FILE_USN_DATA, &versions, sizeof(versions), buffer.data(),
                           static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
        return std::nullopt;
    }
    USN_RECORD_COMMON_HEADER header{};
    if (bytes < sizeof(header)) {
        return std::nullopt;
    }
    std::memcpy(&header, buffer.data(), sizeof(header));
    UsnRecord record;
    if (header.RecordLength > bytes || !parse_record(std::span(buffer.data(), header.RecordLength), record)) {
        return std::nullopt;
    }
    return std::pair{record.parent_frn, std::wstring(record.name)};
}

std::optional<std::wstring> UsnVolume::path_of(std::uint64_t frn) const {
    if (frn == root_frn_) {
        return root_;
    }
    const auto item = open_by_id(frn);
    if (!item) {
        return std::nullopt;
    }
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 3; ++attempt) {
        const DWORD length =
            ::GetFinalPathNameByHandleW(item.get(), path.data(), static_cast<DWORD>(path.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
        if (length == 0) {
            return std::nullopt;
        }
        if (length < path.size()) {
            path.resize(length);
            // "\Users\x", relative to the volume's root
            return normalize_path(root_ + (path.starts_with(L'\\') ? path.substr(1) : path));
        }
        path.resize(length + 1);
    }
    return std::nullopt;
}

}  // namespace avel::platform
