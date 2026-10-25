// avel_capture: an oracle for the install-tracker tests in Windows Sandbox. It lists folders,
// files, registry keys and values with plain Win32 calls, sharing no code with Avel, so comparing
// a capture taken before an installation with one taken after it tells what really changed, to
// check what Avel Trace reports against.
//
//   avel_capture capture <out-file> <spec>... [--exclude <patterns-file>]
//     files:<dir>        every folder and file below <dir>. Links to folders (junctions, symbolic
//                        links, mount points) are listed but not followed
//     files+hash:<dir>   the same, plus a SHA-256 of each file under 16 MB
//     reg:<root>         every key and value below HKLM\..., HKCU\..., HKU\<sid>\... or
//                        HKU\<sid>_Classes\..., in the 64-bit view. Registry links aren't followed
//     --exclude <file>   one regular expression per line (ECMAScript, case-insensitive), matched
//                        against each line's path. A matching line is left out, and so is
//                        everything below a matching folder or key. Empty lines and lines that
//                        start with # are ignored
//   Prints, per spec, how many lines it captured in how long, and per pattern how many lines it
//   left out (a folder or key counts once, without what's below it).
//
//   avel_capture diff <before> <after> [<out-file>]
//     "+ <line>": only after; "- <line>": only before; "~ <line after>\twas <attributes before>":
//     the same path with other attributes. Sorted like the captures. Exit code 0: no differences,
//     1: differences, 2: error.
//
// A capture's lines are UTF-8, a path and its attributes separated by a tab:
//   D <path>                                              a folder
//   D <path>\treparse <tag, hex>                          a link to a folder (not followed)
//   F <path>\t<size> <last write, FILETIME hex>[ <hash>]  a file; <hash> is the SHA-256 under
//                                                         files+hash (none from 16 MB, "link" for
//                                                         a symbolic link, or "unreadable")
//   K <path>                                              a registry key
//   V <key>|<name>\t<type> <size> <FNV-1a 64 of the data, hex>
//   ! <path>\t<error code>                                a folder or key that couldn't be read
// Paths are spelled the way Avel prints them: C:\..., HKLM\SOFTWARE\..., HKU\<sid>\Software\...
// HKCU becomes HKU\<the current user's SID>, and a user's classes HKU\<sid>\Software\Classes\...
// Control characters in names are written as %XX. Lines are sorted by path, ordinally and ignoring
// ASCII case, with "|" and "\" ranked first so a key's values and then its subkeys follow it, and
// then by kind.
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <execution>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kHashLimit = 16ull * 1024 * 1024;

// ---------------------------------------------------------------------------------------------
// Text

void append_utf8(std::string& out, std::wstring_view text) {
    if (text.empty()) {
        return;
    }
    const int size =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    const std::size_t start = out.size();
    out.resize(start + static_cast<std::size_t>(size));
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data() + start, size, nullptr,
                          nullptr);
}

std::string to_utf8(std::wstring_view text) {
    std::string out;
    append_utf8(out, text);
    return out;
}

std::wstring from_utf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// A path as a line shows it: control characters (a tab or a line break would break the line)
// become %XX.
void append_path(std::string& out, std::wstring_view path) {
    if (std::none_of(path.begin(), path.end(), [](wchar_t c) { return c < 0x20; })) {
        append_utf8(out, path);
        return;
    }
    std::wstring escaped;
    for (const wchar_t c : path) {
        if (c < 0x20) {
            escaped += std::format(L"%{:02X}", static_cast<unsigned>(c));
        } else {
            escaped += c;
        }
    }
    append_utf8(out, escaped);
}

bool iequals(std::wstring_view a, std::wstring_view b) {
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool read_file(const std::wstring& path, std::string& text) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = ::GetFileSizeEx(file, &size) != FALSE;
    if (ok) {
        text.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t done = 0;
        while (done < text.size()) {
            DWORD read = 0;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(text.size() - done, 64u << 20));
            if (!::ReadFile(file, text.data() + done, chunk, &read, nullptr) || read == 0) {
                break;
            }
            done += read;
        }
        text.resize(done);
    }
    ::CloseHandle(file);
    if (ok && text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    return ok;
}

// Writes in large chunks; never keeps the file open past close().
class Writer {
public:
    explicit Writer(const std::wstring& path)
        : file_(::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~Writer() { close(); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    bool is_open() const { return file_ != INVALID_HANDLE_VALUE; }

    void write(std::string_view text) {
        buffer_ += text;
        if (buffer_.size() >= (8u << 20)) {
            flush();
        }
    }

    bool close() {
        if (file_ == INVALID_HANDLE_VALUE) {
            return ok_;
        }
        flush();
        ok_ = ::CloseHandle(file_) != FALSE && ok_;
        file_ = INVALID_HANDLE_VALUE;
        return ok_;
    }

private:
    void flush() {
        std::size_t done = 0;
        while (ok_ && done < buffer_.size()) {
            DWORD written = 0;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(buffer_.size() - done, 64u << 20));
            ok_ = ::WriteFile(file_, buffer_.data() + done, chunk, &written, nullptr) != FALSE;
            done += written;
        }
        buffer_.clear();
    }

    HANDLE file_;
    std::string buffer_;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------------------------
// Lines

// One worker's lines: their text in one buffer, and where each line starts.
struct LineBuffer {
    struct Entry {
        std::size_t offset;
        std::uint32_t length;
        std::uint32_t path_length;
    };
    std::string text;
    std::vector<Entry> entries;

    void add(char kind, std::wstring_view path, std::string_view attributes) {
        const std::size_t offset = text.size();
        text += kind;
        text += ' ';
        append_path(text, path);
        const auto path_length = static_cast<std::uint32_t>(text.size() - offset - 2);
        if (!attributes.empty()) {
            text += '\t';
            text += attributes;
        }
        entries.push_back({offset, static_cast<std::uint32_t>(text.size() - offset), path_length});
    }
};

struct Line {
    std::string_view text;
    std::uint32_t path_length = 0;

    char kind() const { return text[0]; }
    std::string_view path() const { return text.substr(2, path_length); }
    std::string_view attributes() const {
        return text.size() > 2u + path_length ? text.substr(3u + path_length) : std::string_view();
    }
};

// Ordinal, ignoring ASCII case; "|" and "\" come first, so a key's values and then its subkeys
// follow it, and a folder's contents follow the folder.
unsigned char fold(char c) {
    const auto u = static_cast<unsigned char>(c);
    if (u == '|') {
        return 1;
    }
    if (u == '\\') {
        return 2;
    }
    if (u >= 'a' && u <= 'z') {
        return static_cast<unsigned char>(u - ('a' - 'A'));
    }
    return u;
}

int compare_paths(std::string_view a, std::string_view b) {
    const std::size_t common = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < common; ++i) {
        const unsigned char x = fold(a[i]);
        const unsigned char y = fold(b[i]);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

int compare_lines(const Line& a, const Line& b) {
    if (const int order = compare_paths(a.path(), b.path()); order != 0) {
        return order;
    }
    return a.kind() == b.kind() ? 0 : (a.kind() < b.kind() ? -1 : 1);
}

bool line_less(const Line& a, const Line& b) { return compare_lines(a, b) < 0; }

// ---------------------------------------------------------------------------------------------
// Exclusions

class Excludes {
public:
    // One pattern per line; empty lines and lines that start with # are skipped.
    bool load(const std::wstring& file, std::string& error) {
        std::string text;
        if (!read_file(file, text)) {
            error = "can't read " + to_utf8(file);
            return false;
        }
        std::wstring combined;
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t end = text.find('\n', start);
            end = end == std::string::npos ? text.size() : end;
            std::string_view line(text.data() + start, end - start);
            if (line.ends_with('\r')) {
                line.remove_suffix(1);
            }
            start = end + 1;
            if (line.empty() || line.starts_with('#')) {
                continue;
            }
            std::wstring pattern = from_utf8(line);
            try {
                regexes_.emplace_back(pattern, kFlags);
            } catch (const std::regex_error& problem) {
                error = std::format("bad pattern '{}': {}", std::string(line), problem.what());
                return false;
            }
            combined += (combined.empty() ? L"(?:" : L"|(?:") + pattern + L")";
            patterns_.push_back(std::string(line));
        }
        if (!patterns_.empty()) {
            try {
                any_ = std::wregex(combined, kFlags);
            } catch (const std::regex_error& problem) {
                error = std::format("can't combine the patterns: {}", problem.what());
                return false;
            }
            counts_ = std::make_unique<std::atomic<std::uint64_t>[]>(patterns_.size());
        }
        return true;
    }

    // True, and counted, if `path` matches a pattern.
    bool skip(std::wstring_view path) const {
        if (patterns_.empty() || !std::regex_search(path.begin(), path.end(), any_)) {
            return false;
        }
        for (std::size_t i = 0; i < regexes_.size(); ++i) {
            if (std::regex_search(path.begin(), path.end(), regexes_[i])) {
                counts_[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        return true;
    }

    std::string report() const {
        std::string text;
        for (std::size_t i = 0; i < patterns_.size(); ++i) {
            text += std::format("excluded {} {}\n", counts_[i].load(), patterns_[i]);
        }
        return text;
    }

private:
    static constexpr auto kFlags = std::regex_constants::ECMAScript | std::regex_constants::icase |
                                   std::regex_constants::nosubs | std::regex_constants::optimize;

    std::vector<std::string> patterns_;
    std::vector<std::wregex> regexes_;
    std::wregex any_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> counts_;
};

// ---------------------------------------------------------------------------------------------
// Workers

// Items for a few threads; processing one may add more. pop() is empty once all are processed.
template <typename Item>
class WorkQueue {
public:
    void push(Item item) {
        {
            std::lock_guard lock(mutex_);
            items_.push_back(std::move(item));
            ++pending_;
        }
        ready_.notify_one();
    }

    std::optional<Item> pop() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return !items_.empty() || pending_ == 0; });
        if (items_.empty()) {
            return std::nullopt;
        }
        Item item = std::move(items_.back());
        items_.pop_back();
        return item;
    }

    void finished() {
        std::lock_guard lock(mutex_);
        if (--pending_ == 0) {
            ready_.notify_all();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<Item> items_;
    std::size_t pending_ = 0;
};

template <typename Item, typename Process>
void run_workers(WorkQueue<Item>& queue, std::vector<LineBuffer>& buffers, const Process& process) {
    std::vector<std::thread> threads;
    threads.reserve(buffers.size());
    for (LineBuffer& buffer : buffers) {
        threads.emplace_back([&queue, &buffer, &process] {
            while (std::optional<Item> item = queue.pop()) {
                process(*item, buffer);
                queue.finished();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

// ---------------------------------------------------------------------------------------------
// Files

std::wstring long_path(const std::wstring& path) {
    return path.starts_with(L"\\\\") ? path : L"\\\\?\\" + path;
}

std::wstring join_path(const std::wstring& folder, std::wstring_view name) {
    std::wstring path = folder;
    if (!path.ends_with(L'\\')) {
        path += L'\\';
    }
    path += name;
    return path;
}

std::string hex(const unsigned char* bytes, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        text[2 * i] = kDigits[bytes[i] >> 4];
        text[2 * i + 1] = kDigits[bytes[i] & 0xF];
    }
    return text;
}

std::string sha256_of(const std::wstring& path, BCRYPT_ALG_HANDLE algorithm) {
    const HANDLE file =
        ::CreateFileW(long_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return "unreadable";
    }
    std::string result = "unreadable";
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        thread_local std::vector<UCHAR> buffer(1u << 20);
        bool ok = true;
        for (;;) {
            DWORD read = 0;
            if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                ok = false;
                break;
            }
            if (read == 0) {
                break;
            }
            if (!BCRYPT_SUCCESS(::BCryptHashData(hash, buffer.data(), read, 0))) {
                ok = false;
                break;
            }
        }
        std::array<UCHAR, 32> digest{};
        if (ok && BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            result = hex(digest.data(), digest.size());
        }
        ::BCryptDestroyHash(hash);
    }
    ::CloseHandle(file);
    return result;
}

struct FileOptions {
    const Excludes* excludes = nullptr;
    BCRYPT_ALG_HANDLE sha256 = nullptr;  // files+hash only
};

std::string file_attributes(const WIN32_FIND_DATAW& data, const std::wstring& path, const FileOptions& options) {
    const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    const std::uint64_t time =
        (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    std::string text = std::format("{} {:016x}", size, time);
    if (options.sha256) {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(data.dwReserved0)) {
            text += " link";
        } else if (size < kHashLimit) {
            text += ' ' + sha256_of(path, options.sha256);
        }
    }
    return text;
}

void list_folder(const std::wstring& folder, const FileOptions& options, WorkQueue<std::wstring>& queue,
                 LineBuffer& out) {
    WIN32_FIND_DATAW data{};
    const HANDLE find = ::FindFirstFileExW(long_path(join_path(folder, L"*")).c_str(), FindExInfoBasic, &data,
                                          FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            out.add('!', folder, std::to_string(error));
        }
        return;
    }
    do {
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        std::wstring path = join_path(folder, name);
        if (options.excludes->skip(path)) {
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(data.dwReserved0)) {
                out.add('D', path, std::format("reparse {:08x}", data.dwReserved0));
            } else {
                out.add('D', path, {});
                queue.push(std::move(path));
            }
        } else {
            out.add('F', path, file_attributes(data, path, options));
        }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
}

// "C:\Program Files\" -> "C:\Program Files"; "C:" -> "C:\".
std::wstring normalize_folder(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.ends_with(L'\\')) {
        path.pop_back();
    }
    if (path.size() == 2 && path[1] == L':') {
        path += L'\\';
    }
    return path;
}

void capture_files(const std::wstring& root, const FileOptions& options, std::vector<LineBuffer>& buffers) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!::GetFileAttributesExW(long_path(root).c_str(), GetFileExInfoStandard, &info)) {
        return;  // Missing: nothing to list
    }
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FIND_DATAW data{};
        data.dwFileAttributes = info.dwFileAttributes;
        data.nFileSizeHigh = info.nFileSizeHigh;
        data.nFileSizeLow = info.nFileSizeLow;
        data.ftLastWriteTime = info.ftLastWriteTime;
        buffers.front().add('F', root, file_attributes(data, root, options));
        return;
    }
    buffers.front().add('D', root, {});
    WorkQueue<std::wstring> queue;
    queue.push(root);
    run_workers(queue, buffers, [&options, &queue](const std::wstring& folder, LineBuffer& out) {
        list_folder(folder, options, queue, out);
    });
}

// ---------------------------------------------------------------------------------------------
// Registry

struct KeyItem {
    std::wstring subpath;  // Below the hive's handle
    std::wstring display;  // As Avel prints it
    bool root = false;     // The spec's own key: links in its path are followed
};

std::uint64_t fnv1a(const BYTE* data, std::size_t size) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash = (hash ^ data[i]) * 0x100000001b3ull;
    }
    return hash;
}

std::wstring child_path(const std::wstring& parent, std::wstring_view name) {
    return parent.empty() ? std::wstring(name) : parent + L"\\" + std::wstring(name);
}

void list_key(const KeyItem& item, HKEY hive, const Excludes& excludes, WorkQueue<KeyItem>& queue, LineBuffer& out) {
    HKEY key = nullptr;
    LSTATUS status = ::RegOpenKeyExW(hive, item.subpath.empty() ? nullptr : item.subpath.c_str(),
                                     item.root ? 0 : REG_OPTION_OPEN_LINK,
                                     KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS) {
        if (status != ERROR_FILE_NOT_FOUND) {  // Missing (or deleted meanwhile): nothing to list
            out.add('!', item.display, std::to_string(status));
        }
        return;
    }
    if (item.root) {
        out.add('K', item.display, {});
    }
    DWORD subkey_count = 0;
    DWORD max_subkey = 0;
    DWORD value_count = 0;
    DWORD max_name = 0;
    DWORD max_data = 0;
    ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkey_count, &max_subkey, nullptr, &value_count, &max_name,
                       &max_data, nullptr, nullptr);

    thread_local std::wstring name;
    thread_local std::vector<BYTE> data;
    name.resize(std::max<std::size_t>(name.size(), static_cast<std::size_t>(max_name) + 2));
    data.resize(std::max<std::size_t>(data.size(), std::max<std::size_t>(max_data, 1)));
    for (DWORD index = 0, retries = 0;;) {
        auto name_length = static_cast<DWORD>(name.size());
        auto data_length = static_cast<DWORD>(data.size());
        DWORD type = 0;
        status = ::RegEnumValueW(key, index, name.data(), &name_length, nullptr, &type, data.data(), &data_length);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status == ERROR_MORE_DATA && retries < 8) {  // It grew meanwhile
            ++retries;
            name.resize(std::max<std::size_t>(name.size() * 2, 16386));
            data.resize(std::max<std::size_t>(static_cast<std::size_t>(data_length) + 16, data.size() * 2));
            continue;
        }
        retries = 0;
        if (status != ERROR_SUCCESS) {
            out.add('!', item.display + L"|#" + std::to_wstring(index), std::to_string(status));
            ++index;
            continue;
        }
        const std::wstring path = item.display + L"|" + std::wstring(name.data(), name_length);
        if (!excludes.skip(path)) {
            out.add('V', path, std::format("{} {} {:016x}", type, data_length, fnv1a(data.data(), data_length)));
        }
        ++index;
    }

    std::wstring subkey(std::max<std::size_t>(max_subkey, 255) + 2, L'\0');
    for (DWORD index = 0;;) {
        auto length = static_cast<DWORD>(subkey.size());
        status = ::RegEnumKeyExW(key, index, subkey.data(), &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status == ERROR_MORE_DATA && subkey.size() < 65536) {
            subkey.resize(subkey.size() * 2);
            continue;
        }
        if (status != ERROR_SUCCESS) {
            out.add('!', item.display + L"\\#" + std::to_wstring(index), std::to_string(status));
            break;
        }
        const std::wstring_view child(subkey.data(), length);
        KeyItem next{child_path(item.subpath, child), item.display + L"\\" + std::wstring(child), false};
        if (!excludes.skip(next.display)) {
            out.add('K', next.display, {});
            queue.push(std::move(next));
        }
        ++index;
    }
    ::RegCloseKey(key);
}

std::wstring current_user_sid() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    std::wstring sid;
    LPWSTR text = nullptr;
    if (size && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
        ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
        sid = text;
        ::LocalFree(text);
    }
    ::CloseHandle(token);
    return sid;
}

struct RegistryRoot {
    HKEY hive = nullptr;
    std::wstring subpath;
    std::wstring display;
};

// The first component of `path`, which loses it and its backslash.
std::wstring take_component(std::wstring& path) {
    const std::size_t slash = path.find(L'\\');
    std::wstring first = path.substr(0, slash);
    path = slash == std::wstring::npos ? L"" : path.substr(slash + 1);
    return first;
}

std::optional<RegistryRoot> parse_registry_root(std::wstring spec) {
    std::replace(spec.begin(), spec.end(), L'/', L'\\');
    while (spec.ends_with(L'\\')) {
        spec.pop_back();
    }
    std::wstring rest = spec;
    const std::wstring first = take_component(rest);
    const auto with_rest = [&rest](std::wstring path) { return rest.empty() ? path : path + L"\\" + rest; };
    if (iequals(first, L"HKLM") || iequals(first, L"HKEY_LOCAL_MACHINE")) {
        return RegistryRoot{HKEY_LOCAL_MACHINE, rest, with_rest(L"HKLM")};
    }
    if (iequals(first, L"HKCU") || iequals(first, L"HKEY_CURRENT_USER")) {
        const std::wstring sid = current_user_sid();
        if (sid.empty()) {
            return std::nullopt;
        }
        return RegistryRoot{HKEY_USERS, with_rest(sid), with_rest(L"HKU\\" + sid)};
    }
    if (iequals(first, L"HKU") || iequals(first, L"HKEY_USERS")) {
        const std::wstring hive = take_component(rest);
        if (hive.empty()) {
            return std::nullopt;
        }
        constexpr std::wstring_view kClasses = L"_Classes";
        if (hive.size() > kClasses.size() &&
            iequals(std::wstring_view(hive).substr(hive.size() - kClasses.size()), kClasses)) {
            const std::wstring sid = hive.substr(0, hive.size() - kClasses.size());
            return RegistryRoot{HKEY_USERS, with_rest(hive), with_rest(L"HKU\\" + sid + L"\\Software\\Classes")};
        }
        return RegistryRoot{HKEY_USERS, with_rest(hive), with_rest(L"HKU\\" + hive)};
    }
    return std::nullopt;
}

void capture_registry(const RegistryRoot& root, const Excludes& excludes, std::vector<LineBuffer>& buffers) {
    WorkQueue<KeyItem> queue;
    queue.push(KeyItem{root.subpath, root.display, true});
    run_workers(queue, buffers, [&root, &excludes, &queue](const KeyItem& item, LineBuffer& out) {
        list_key(item, root.hive, excludes, queue, out);
    });
}

// ---------------------------------------------------------------------------------------------
// Commands

std::size_t line_count(const std::vector<LineBuffer>& buffers) {
    std::size_t count = 0;
    for (const LineBuffer& buffer : buffers) {
        count += buffer.entries.size();
    }
    return count;
}

void print(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

int capture(const std::wstring& out_file, const std::vector<std::wstring>& specs, const std::wstring& exclude_file) {
    Excludes excludes;
    if (!exclude_file.empty()) {
        std::string error;
        if (!excludes.load(exclude_file, error)) {
            std::fprintf(stderr, "avel_capture: %s\n", error.c_str());
            return 2;
        }
    }
    BCRYPT_ALG_HANDLE sha256 = nullptr;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    std::vector<LineBuffer> buffers(std::clamp(cores * 2, 4u, 16u));
    const auto started = Clock::now();
    std::string summary;
    for (const std::wstring& spec : specs) {
        const auto spec_started = Clock::now();
        const std::size_t before = line_count(buffers);
        if (spec.starts_with(L"files:") || spec.starts_with(L"files+hash:")) {
            const bool hashed = spec.starts_with(L"files+hash:");
            FileOptions options{&excludes, nullptr};
            if (hashed) {
                if (!sha256 && !BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
                    std::fprintf(stderr, "avel_capture: SHA-256 is not available\n");
                    return 1;
                }
                options.sha256 = sha256;
            }
            capture_files(normalize_folder(spec.substr(spec.find(L':') + 1)), options, buffers);
        } else if (spec.starts_with(L"reg:")) {
            const std::optional<RegistryRoot> root = parse_registry_root(spec.substr(4));
            if (!root) {
                std::fprintf(stderr, "avel_capture: bad registry root: %s\n", to_utf8(spec).c_str());
                return 2;
            }
            capture_registry(*root, excludes, buffers);
        } else {
            std::fprintf(stderr, "avel_capture: unknown spec: %s\n", to_utf8(spec).c_str());
            return 2;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - spec_started);
        summary += std::format("spec {}: {} lines in {} ms\n", to_utf8(spec), line_count(buffers) - before,
                               elapsed.count());
    }
    if (sha256) {
        ::BCryptCloseAlgorithmProvider(sha256, 0);
    }

    std::vector<Line> lines;
    lines.reserve(line_count(buffers));
    for (const LineBuffer& buffer : buffers) {
        const std::string_view text = buffer.text;
        for (const LineBuffer::Entry& entry : buffer.entries) {
            lines.push_back({text.substr(entry.offset, entry.length), entry.path_length});
        }
    }
    std::sort(std::execution::par, lines.begin(), lines.end(), line_less);

    Writer writer(out_file);
    if (!writer.is_open()) {
        std::fprintf(stderr, "avel_capture: can't write %s (%lu)\n", to_utf8(out_file).c_str(), ::GetLastError());
        return 1;
    }
    std::array<std::size_t, 5> counts{};  // D, F, K, V, !
    constexpr std::string_view kKinds = "DFKV!";
    std::size_t written = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        // The same line twice: overlapping specs, or a key enumerated twice while it changed.
        if (i > 0 && compare_lines(lines[i - 1], lines[i]) == 0) {
            continue;
        }
        writer.write(lines[i].text);
        writer.write("\n");
        ++written;
        const std::size_t kind = kKinds.find(lines[i].kind());
        if (kind != std::string_view::npos) {
            ++counts[kind];
        }
    }
    if (!writer.close()) {
        std::fprintf(stderr, "avel_capture: writing %s failed\n", to_utf8(out_file).c_str());
        return 1;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    summary += std::format("total: {} lines ({} folders, {} files, {} keys, {} values, {} unreadable) in {} ms\n",
                           written, counts[0], counts[1], counts[2], counts[3], counts[4], elapsed.count());
    summary += excludes.report();
    print(summary);
    return 0;
}

struct Capture {
    std::string text;
    std::vector<Line> lines;
};

bool read_capture(const std::wstring& file, Capture& capture, std::string& error) {
    if (!read_file(file, capture.text)) {
        error = "can't read " + to_utf8(file);
        return false;
    }
    const std::string_view text = capture.text;
    std::size_t start = 0;
    std::size_t number = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        end = end == std::string_view::npos ? text.size() : end;
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++number;
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (line.size() < 3 || line[1] != ' ') {
            error = std::format("{}: line {} is not a capture line", to_utf8(file), number);
            return false;
        }
        const std::size_t tab = line.find('\t', 2);
        const std::size_t path_end = tab == std::string_view::npos ? line.size() : tab;
        capture.lines.push_back({line, static_cast<std::uint32_t>(path_end - 2)});
    }
    if (!std::is_sorted(capture.lines.begin(), capture.lines.end(), line_less)) {
        std::stable_sort(capture.lines.begin(), capture.lines.end(), line_less);
    }
    return true;
}

int diff(const std::wstring& before_file, const std::wstring& after_file, const std::wstring& out_file) {
    Capture before;
    Capture after;
    std::string error;
    if (!read_capture(before_file, before, error) || !read_capture(after_file, after, error)) {
        std::fprintf(stderr, "avel_capture: %s\n", error.c_str());
        return 2;
    }
    std::string out;
    std::size_t differences = 0;
    const auto emit = [&out, &differences](std::string_view sign, std::string_view line, std::string_view was = {}) {
        out += sign;
        out += line;
        if (sign == "~ ") {
            out += "\twas ";
            out += was;
        }
        out += '\n';
        ++differences;
    };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < before.lines.size() || j < after.lines.size()) {
        const int order = i == before.lines.size()  ? 1
                          : j == after.lines.size() ? -1
                                                    : compare_lines(before.lines[i], after.lines[j]);
        if (order < 0) {
            emit("- ", before.lines[i++].text);
        } else if (order > 0) {
            emit("+ ", after.lines[j++].text);
        } else {
            if (before.lines[i].attributes() != after.lines[j].attributes()) {
                emit("~ ", after.lines[j].text, before.lines[i].attributes());
            }
            ++i;
            ++j;
        }
    }
    if (out_file.empty()) {
        print(out);
    } else {
        Writer writer(out_file);
        writer.write(out);
        if (!writer.is_open() || !writer.close()) {
            std::fprintf(stderr, "avel_capture: can't write %s\n", to_utf8(out_file).c_str());
            return 2;
        }
    }
    return differences == 0 ? 0 : 1;
}

int usage() {
    std::fputs(
        "usage: avel_capture capture <out-file> <spec>... [--exclude <patterns-file>]\n"
        "         spec: files:<dir> | files+hash:<dir> | reg:<HKLM|HKCU|HKU\\<sid>|HKU\\<sid>_Classes>[\\<path>]\n"
        "       avel_capture diff <before> <after> [<out-file>]\n",
        stderr);
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    const std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.size() >= 3 && args[0] == L"capture") {
        std::vector<std::wstring> specs;
        std::wstring exclude_file;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == L"--exclude") {
                if (i + 1 >= args.size()) {
                    return usage();
                }
                exclude_file = args[++i];
            } else {
                specs.push_back(args[i]);
            }
        }
        if (specs.empty()) {
            return usage();
        }
        return capture(args[1], specs, exclude_file);
    }
    if ((args.size() == 3 || args.size() == 4) && args[0] == L"diff") {
        return diff(args[1], args[2], args.size() == 4 ? args[3] : std::wstring());
    }
    return usage();
}
