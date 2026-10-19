#include "core/uninstall/installer_detection.h"

#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <vector>

namespace avel::uninstall {

namespace {

// NSIS stubs store their data after the executable image behind this signature, also in builds
// that carry no identifying manifest.
constexpr std::string_view kNsisSignature = "NullsoftInst";
constexpr std::uint64_t kMaxScannedBytes = 64ull * 1024 * 1024;

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// Searches a file for `needle` in chunks, without reading files larger than `max_bytes`.
bool file_contains(const std::wstring& path, std::string_view needle, std::uint64_t max_bytes) {
    const wil::unique_hfile file(::CreateFileW(path.c_str(), GENERIC_READ,
                                               FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    LARGE_INTEGER size{};
    if (!file || !::GetFileSizeEx(file.get(), &size) ||
        static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
        return false;
    }
    constexpr DWORD kChunk = 1 << 20;
    std::vector<char> buffer(kChunk + needle.size());
    size_t carried = 0;  // Tail of the previous chunk, so matches across chunk borders are found
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(file.get(), buffer.data() + carried, kChunk, &read, nullptr) || read == 0) {
            return false;
        }
        const std::string_view data(buffer.data(), carried + read);
        if (contains(data, needle)) {
            return true;
        }
        carried = std::min(needle.size() - 1, data.size());
        std::copy(data.end() - static_cast<std::ptrdiff_t>(carried), data.end(), buffer.begin());
    }
}

}  // namespace

inventory::InstallerType installer_type_from_manifest(std::string_view manifest) {
    if (contains(manifest, "Nullsoft.NSIS") || contains(manifest, "Nullsoft Install System")) {
        return inventory::InstallerType::Nsis;
    }
    if (contains(manifest, "JR.Inno.Setup")) {
        return inventory::InstallerType::InnoSetup;
    }
    return inventory::InstallerType::Unknown;
}

std::optional<std::string> read_embedded_manifest(const std::wstring& executable) {
    // As an image resource: mapped for reading, never run, and DllMain isn't called.
    const wil::unique_hmodule module(::LoadLibraryExW(
        executable.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE));
    if (!module) {
        return std::nullopt;
    }
    // Executables carry their manifest as resource 1 (CREATEPROCESS_MANIFEST_RESOURCE_ID).
    HRSRC resource = ::FindResourceW(module.get(), MAKEINTRESOURCEW(1), RT_MANIFEST);
    if (resource == nullptr) {
        return std::nullopt;
    }
    const HGLOBAL loaded = ::LoadResource(module.get(), resource);
    const DWORD size = ::SizeofResource(module.get(), resource);
    const void* data = loaded ? ::LockResource(loaded) : nullptr;
    if (data == nullptr || size == 0) {
        return std::nullopt;
    }
    return std::string(static_cast<const char*>(data), size);
}

inventory::InstallerType detect_installer_type(const std::wstring& executable) {
    if (const auto manifest = read_embedded_manifest(executable)) {
        if (const auto type = installer_type_from_manifest(*manifest);
            type != inventory::InstallerType::Unknown) {
            return type;
        }
    }
    return file_contains(executable, kNsisSignature, kMaxScannedBytes) ? inventory::InstallerType::Nsis
                                                                       : inventory::InstallerType::Unknown;
}

}  // namespace avel::uninstall
