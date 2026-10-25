// Net file changes from journal records, against a made-up volume Q: whose folders a fake resolver
// knows (no disk is read, except for the test of items older than the trace, which uses real files
// in a temporary folder for their creation times).
#include <gtest/gtest.h>

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/platform/hash.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/tracker/file_changes.h"
#include "file_tree.h"

namespace avel::tracker {
namespace {

using platform::to_utf8;

constexpr std::uint64_t kRoot = 0x0005000000000005;  // NTFS's root folder: record 5, sequence 5

// Folders that exist on Q: and have no records of their own.
constexpr std::uint64_t kProgramFiles = 0x0001000000001000;
constexpr std::uint64_t kData = 0x0001000000001001;
constexpr std::uint64_t kConfigMsi = 0x0001000000001002;
constexpr std::uint64_t kTemp = 0x0001000000001003;  // Stands for a real temporary folder

constexpr std::uint32_t kFolder = FILE_ATTRIBUTE_DIRECTORY;
constexpr std::uint32_t kFile = FILE_ATTRIBUTE_ARCHIVE;

class FakeResolver : public PathResolver {
public:
    void add_folder(std::uint64_t frn, std::uint64_t parent, std::wstring name) {
        folders_[frn] = {parent, std::move(name)};
    }
    void add_fixed(std::uint64_t frn, std::wstring path) { fixed_[frn] = std::move(path); }
    void add_id(const std::wstring& path, std::uint64_t frn) { ids_[platform::to_lower(path)] = frn; }
    void add_created(const std::wstring& path, std::int64_t time) {
        created_[platform::to_lower(path)] = time;
    }
    // The attributes programs see, through a container's filter.
    void add_attributes(const std::wstring& path, std::uint32_t attributes) {
        attributes_[platform::to_lower(path)] = attributes;
    }
    const std::set<std::uint64_t>& asked() const { return asked_; }

    std::optional<std::wstring> path_now(std::uint64_t folder) override {
        asked_.insert(folder);
        if (const auto fixed = fixed_.find(folder); fixed != fixed_.end()) {
            return fixed->second;
        }
        std::wstring path;
        for (std::uint64_t current = folder; current != kRoot;) {
            const auto found = folders_.find(current);
            if (found == folders_.end()) {
                return std::nullopt;
            }
            path = L"\\" + found->second.second + path;
            current = found->second.first;
        }
        return L"Q:" + path;
    }
    std::optional<std::pair<std::uint64_t, std::wstring>> name_now(std::uint64_t folder) override {
        asked_.insert(folder);
        const auto found = folders_.find(folder);
        return found != folders_.end() ? std::optional(found->second) : std::nullopt;
    }
    std::optional<std::uint64_t> id_at(const std::wstring& path) override {
        const auto found = ids_.find(platform::to_lower(path));
        return found != ids_.end() ? std::optional(found->second) : std::nullopt;
    }
    std::optional<std::int64_t> created_at(const std::wstring& path) override {
        if (const auto found = created_.find(platform::to_lower(path)); found != created_.end()) {
            return found->second;
        }
        // Real files (in a temporary folder) answer from the disk.
        return path.starts_with(L"Q:") ? std::nullopt : PathResolver::created_at(path);
    }
    std::optional<std::uint32_t> attributes_at(const std::wstring& path) override {
        if (const auto found = attributes_.find(platform::to_lower(path)); found != attributes_.end()) {
            return found->second;
        }
        return path.starts_with(L"Q:") ? std::nullopt : PathResolver::attributes_at(path);
    }

private:
    std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::wstring>> folders_;
    std::unordered_map<std::uint64_t, std::wstring> fixed_;
    std::unordered_map<std::wstring, std::uint64_t> ids_;
    std::unordered_map<std::wstring, std::int64_t> created_;
    std::unordered_map<std::wstring, std::uint32_t> attributes_;
    std::set<std::uint64_t> asked_;
};

// "created Q:\A\ +2", "renamed Q:\B <- Q:\A", "modified Q:\C replaced": compact and readable when
// a comparison fails.
std::vector<std::string> describe(const std::vector<TraceChange>& changes) {
    std::vector<std::string> lines;
    for (const auto& change : changes) {
        const auto& location = std::get<leftovers::FileLocation>(change.location);
        std::string line = std::string(to_string(change.change)) + " " + to_utf8(location.path);
        if (location.directory) {
            line += "\\";
        }
        if (!change.old_path.empty()) {
            line += " <- " + to_utf8(change.old_path);
        }
        if (change.contents != 0) {
            line += " +" + std::to_string(change.contents);
        }
        if (change.replaced) {
            line += " replaced";
        }
        if (change.noise != NoiseReason::None) {
            line += " (" + std::string(to_string(change.noise)) + ")";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

class FileChangesTest : public ::testing::Test {
protected:
    void SetUp() override {
        resolver_.add_folder(kProgramFiles, kRoot, L"Program Files");
        resolver_.add_folder(kData, kRoot, L"Data");
        resolver_.add_folder(kConfigMsi, kRoot, L"Config.Msi");
    }

    void record(std::uint64_t frn, std::uint64_t parent, std::wstring_view name, std::uint32_t reason,
                std::uint32_t attributes) {
        platform::UsnRecord record;
        record.usn = next_usn_;
        next_usn_ += 96;
        record.frn = frn;
        record.parent_frn = parent;
        record.reason = reason;
        record.attributes = attributes;
        record.name = name;
        changes_.add(record);
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            files_.insert(frn);
        }
    }

    // The records Windows writes for each operation.
    void create(std::uint64_t frn, std::uint64_t parent, std::wstring_view name,
                std::uint32_t attributes = kFile) {
        record(frn, parent, name, USN_REASON_FILE_CREATE, attributes);
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            record(frn, parent, name, USN_REASON_FILE_CREATE | USN_REASON_CLOSE, attributes);
        } else {
            record(frn, parent, name, USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND, attributes);
            record(frn, parent, name, USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND | USN_REASON_CLOSE,
                   attributes);
        }
    }
    void write(std::uint64_t frn, std::uint64_t parent, std::wstring_view name) {
        record(frn, parent, name, USN_REASON_DATA_OVERWRITE, kFile);
        record(frn, parent, name, USN_REASON_DATA_OVERWRITE | USN_REASON_CLOSE, kFile);
    }
    void remove(std::uint64_t frn, std::uint64_t parent, std::wstring_view name,
                std::uint32_t attributes = kFile) {
        record(frn, parent, name, USN_REASON_FILE_DELETE | USN_REASON_CLOSE, attributes);
    }
    void rename(std::uint64_t frn, std::uint64_t old_parent, std::wstring_view old_name,
                std::uint64_t new_parent, std::wstring_view new_name, std::uint32_t attributes = kFile) {
        record(frn, old_parent, old_name, USN_REASON_RENAME_OLD_NAME, attributes);
        record(frn, new_parent, new_name, USN_REASON_RENAME_NEW_NAME, attributes);
        record(frn, new_parent, new_name, USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE, attributes);
    }

    std::vector<std::string> resolve(std::int64_t trace_start = 0) { return resolve(changes_, trace_start); }
    std::vector<std::string> resolve(const FileChangeAggregator& changes, std::int64_t trace_start = 0) {
        auto lines = describe(resolve_file_changes(changes, kRoot, L"Q:\\", resolver_, trace_start));
        // Files take their names from their records; only folders are ever looked up.
        for (const auto frn : resolver_.asked()) {
            EXPECT_FALSE(files_.contains(frn)) << std::hex << frn;
        }
        return lines;
    }

    FileChangeAggregator changes_;
    FakeResolver resolver_;
    std::set<std::uint64_t> files_;
    std::int64_t next_usn_ = 0x10000;
};

TEST_F(FileChangesTest, ACreatedThenDeletedFileLeavesNothing) {
    create(0x0001000000002000, kData, L"transient.tmp");
    write(0x0001000000002000, kData, L"transient.tmp");
    remove(0x0001000000002000, kData, L"transient.tmp");
    EXPECT_TRUE(resolve().empty());
}

TEST_F(FileChangesTest, ATemporaryFileRenamedOverAnotherReplacesIt) {
    constexpr std::uint64_t kOld = 0x0003000000002001;
    constexpr std::uint64_t kNew = 0x0001000000002002;
    create(kNew, kData, L"app.dll.tmp");
    // MoveFileEx with MOVEFILE_REPLACE_EXISTING deletes the old file as part of the rename.
    remove(kOld, kData, L"app.dll");
    rename(kNew, kData, L"app.dll.tmp", kData, L"app.dll");
    EXPECT_EQ(resolve(), std::vector<std::string>{R"(modified Q:\Data\app.dll replaced)"});
}

TEST_F(FileChangesTest, DeletingAFileAndRenamingANewOneIntoItsPlaceReplacesIt) {
    constexpr std::uint64_t kOld = 0x0003000000002003;
    constexpr std::uint64_t kNew = 0x0001000000002004;
    remove(kOld, kData, L"config.xml");
    create(kNew, kProgramFiles, L"config.new");
    rename(kNew, kProgramFiles, L"config.new", kData, L"config.xml");
    EXPECT_EQ(resolve(), std::vector<std::string>{R"(modified Q:\Data\config.xml replaced)"});
}

TEST_F(FileChangesTest, AFileMovedAsideAndWrittenAgainIsReplacedAndStaysRenamed) {
    // Windows Installer moves a file it replaces into Config.Msi for a rollback.
    constexpr std::uint64_t kOld = 0x0003000000002005;
    constexpr std::uint64_t kNew = 0x0001000000002006;
    rename(kOld, kData, L"app.exe", kConfigMsi, L"1a2b3c.rbf");
    create(kNew, kData, L"app.exe");
    const std::vector<std::string> expected = {
        R"(renamed Q:\Config.Msi\1a2b3c.rbf <- Q:\Data\app.exe)",
        R"(modified Q:\Data\app.exe replaced)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, AnOlderFileRenamedOverAnotherReplacesItAndItsNameIsGone) {
    constexpr std::uint64_t kOld = 0x0003000000002007;
    constexpr std::uint64_t kNew = 0x0002000000002008;  // Existed before the trace
    remove(kOld, kData, L"settings.ini");
    rename(kNew, kData, L"settings.new", kData, L"settings.ini");
    const std::vector<std::string> expected = {
        R"(modified Q:\Data\settings.ini replaced)",
        R"(deleted Q:\Data\settings.new)",
    };
    EXPECT_EQ(resolve(), expected);
}

// WMI's performance adapter writes WmiApRpl_new.ini and renames it over WmiApRpl.ini. Here it
// created the new file before the trace and closed it during it, so its first record in the trace
// still carries FILE_CREATE: the name it had at the start is gone all the same.
TEST_F(FileChangesTest, AFileOpenSinceBeforeTheTraceRenamedOverAnotherReplacesIt) {
    constexpr std::uint64_t kOld = 0x000200000000135b;
    constexpr std::uint64_t kNew = 0x0002000000003dcf;
    resolver_.add_created(LR"(Q:\Data\WmiApRpl.ini)", 1000);  // kNew's creation time
    record(kNew, kData, L"WmiApRpl_new.ini",
           USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND | USN_REASON_CLOSE, kFile);
    remove(kOld, kData, L"WmiApRpl.ini");
    rename(kNew, kData, L"WmiApRpl_new.ini", kData, L"WmiApRpl.ini");
    const std::vector<std::string> expected = {
        R"(modified Q:\Data\WmiApRpl.ini replaced)",
        R"(deleted Q:\Data\WmiApRpl_new.ini)",
    };
    EXPECT_EQ(resolve(2000), expected);
}

// Two files swapped through a third name: each place holds another file now, and no name is gone.
TEST_F(FileChangesTest, FilesRenamedIntoEachOthersPlacesAreReplaced) {
    constexpr std::uint64_t kFirst = 0x0002000000002009;
    constexpr std::uint64_t kSecond = 0x000200000000200a;
    rename(kFirst, kData, L"a.dat", kData, L"swap.tmp");
    rename(kSecond, kData, L"b.dat", kData, L"a.dat");
    rename(kFirst, kData, L"swap.tmp", kData, L"b.dat");
    const std::vector<std::string> expected = {
        R"(modified Q:\Data\a.dat replaced)",
        R"(modified Q:\Data\b.dat replaced)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, MetadataOnlyChangesAreNoise) {
    record(0x0001000000002010, kData, L"touched.txt", USN_REASON_BASIC_INFO_CHANGE | USN_REASON_CLOSE, kFile);
    record(0x0001000000002011, kData, L"secured.txt", USN_REASON_SECURITY_CHANGE | USN_REASON_CLOSE, kFile);
    record(0x0001000000002012, kData, L"tracked.txt", USN_REASON_OBJECT_ID_CHANGE, kFile);
    record(kData, kRoot, L"Data", USN_REASON_INDEXABLE_CHANGE | USN_REASON_CLOSE, kFolder);
    record(0x0001000000002014, kData, L"packed.bin", USN_REASON_COMPRESSION_CHANGE, kFile);
    // Code Integrity caching its check of a signed file: an extended attribute, the data untouched.
    record(0x0001000000002017, kData, L"signed.dll", USN_REASON_EA_CHANGE | USN_REASON_CLOSE, kFile);
    write(0x0001000000002015, kData, L"written.txt");
    record(0x0001000000002016, kData, L"tagged.exe", USN_REASON_NAMED_DATA_EXTEND | USN_REASON_CLOSE, kFile);
    record(0x0001000000002018, kData, L"rewritten.ini",
           USN_REASON_DATA_OVERWRITE | USN_REASON_BASIC_INFO_CHANGE | USN_REASON_CLOSE, kFile);
    const std::vector<std::string> expected = {
        R"(modified Q:\Data\ (metadata-only))",
        R"(modified Q:\Data\packed.bin (metadata-only))",
        R"(modified Q:\Data\rewritten.ini)",
        R"(modified Q:\Data\secured.txt (metadata-only))",
        R"(modified Q:\Data\signed.dll (metadata-only))",
        R"(modified Q:\Data\tagged.exe)",
        R"(modified Q:\Data\touched.txt (metadata-only))",
        R"(modified Q:\Data\tracked.txt (metadata-only))",
        R"(modified Q:\Data\written.txt)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, AFileDeletedWhileOpenIsDeleted) {
    // NTFS moves a file deleted with POSIX semantics into $Extend\$Deleted until its last handle
    // closes; that may be after the trace.
    constexpr std::uint64_t kExtend = 0x000B00000000000B;
    constexpr std::uint64_t kPending = 0x0001000000001004;
    resolver_.add_folder(kExtend, kRoot, L"$Extend");
    resolver_.add_folder(kPending, kExtend, L"$Deleted");
    create(0x0001000000002040, kData, L"setup.tmp");
    rename(0x0001000000002040, kData, L"setup.tmp", kPending, L"0001000000002040A1B2C3D4");
    rename(0x0003000000002041, kData, L"old.log", kPending, L"0003000000002041A1B2C3D5");
    EXPECT_EQ(resolve(), std::vector<std::string>{R"(deleted Q:\Data\old.log)"});
}

TEST_F(FileChangesTest, AFileOpenSinceBeforeTheTraceAndDeletedIsDeleted) {
    // The shell keeps its cache open from its creation until it deletes it, so the record of the
    // deletion still carries FILE_CREATE.
    constexpr std::uint32_t kKept =
        USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_OVERWRITE;
    create(0x0001000000002050, kData, L"cache.ver06.db");
    record(0x0001000000002051, kData, L"cache.ver04.db", kKept | USN_REASON_FILE_DELETE | USN_REASON_CLOSE,
           kFile);
    // The same, deleted while another handle stays open.
    constexpr std::uint64_t kExtend = 0x000B00000000000B;
    constexpr std::uint64_t kPending = 0x0001000000001004;
    resolver_.add_folder(kExtend, kRoot, L"$Extend");
    resolver_.add_folder(kPending, kExtend, L"$Deleted");
    record(0x0001000000002052, kData, L"index.db", kKept | USN_REASON_RENAME_OLD_NAME, kFile);
    record(0x0001000000002052, kPending, L"0001000000002052A1B2C3D4", kKept | USN_REASON_RENAME_NEW_NAME,
           kFile);
    const std::vector<std::string> expected = {
        R"(deleted Q:\Data\cache.ver04.db)",
        R"(created Q:\Data\cache.ver06.db)",
        R"(deleted Q:\Data\index.db)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, AFileDeletedBeforeTheTraceIsNoChange) {
    // Deleted while open before the trace: in $Extend\$Deleted already, gone when its last handle
    // closes. With a folder renamed during the trace, $Deleted's name can't be read (only its path).
    constexpr std::uint64_t kPending = 0x0001000000001004;
    resolver_.add_fixed(kPending, LR"(Q:\$Extend\$Deleted)");
    record(0x0002000000002042, kPending, L"00020000000020427F081A7D",
           USN_REASON_FILE_DELETE | USN_REASON_CLOSE, kFile);
    rename(0x0001000000002043, kRoot, L"Old Folder", kRoot, L"New Folder", kFolder);
    EXPECT_EQ(resolve(), std::vector<std::string>{R"(renamed Q:\New Folder\ <- Q:\Old Folder)"});
}

// The records Windows Sandbox writes as NTFS gets its placeholders from the base layer
// (attributes: archive, sparse, reparse point, offline; folders: directory, reparse point, offline),
// which programs see as plain files and folders.
TEST_F(FileChangesTest, AContainersPlaceholdersAreNoChange) {
    constexpr std::uint32_t kPlaceholder = 0x1620;
    constexpr std::uint32_t kPlaceholderFolder = 0x1410;
    constexpr std::uint32_t kListed = USN_REASON_FILE_CREATE | USN_REASON_REPARSE_POINT_CHANGE;
    constexpr std::uint32_t kRead = kListed | USN_REASON_DATA_EXTEND;
    constexpr std::uint32_t kLoaded = kRead | USN_REASON_EA_CHANGE;  // Code Integrity's catalog hint
    constexpr std::int64_t kTraceStart = 100;
    // Older than the trace, and `seen` as programs see it.
    const auto placeholder = [&](std::uint64_t frn, std::wstring_view name, std::uint32_t reasons,
                                 std::uint32_t attributes, std::uint32_t seen, std::int64_t created = 1) {
        record(frn, kData, name, reasons, attributes);
        record(frn, kData, name, reasons | USN_REASON_CLOSE, attributes);
        resolver_.add_created(LR"(Q:\Data\)" + std::wstring(name), created);
        resolver_.add_attributes(LR"(Q:\Data\)" + std::wstring(name), seen);
    };
    // Listed, read, loaded: nothing changed.
    placeholder(0x0001000000002060, L"bg-BG", kListed, kPlaceholderFolder, kFolder);
    placeholder(0x0001000000002061, L"C_037.NLS", kRead, kPlaceholder, kFile);
    placeholder(0x0001000000002062, L"sc.exe", kLoaded, kPlaceholder, kFile);
    record(0x0001000000002062, kData, L"sc.exe", USN_REASON_CLOSE, kPlaceholder);
    // Changed besides.
    placeholder(0x0001000000002063, L"patched.dll", kRead, kPlaceholder, kFile);
    write(0x0001000000002063, kData, L"patched.dll");
    placeholder(0x0001000000002064, L"secured.dll", kRead, kPlaceholder, kFile);
    record(0x0001000000002064, kData, L"secured.dll", kRead | USN_REASON_SECURITY_CHANGE, kPlaceholder);
    placeholder(0x0001000000002066, L"old.ini", kRead, kPlaceholder, kFile);
    rename(0x0001000000002066, kData, L"old.ini", kData, L"new.ini", kPlaceholder);
    resolver_.add_created(LR"(Q:\Data\new.ini)", 1);
    resolver_.add_attributes(LR"(Q:\Data\new.ini)", kFile);
    placeholder(0x0001000000002068, L"gone.dll", kRead, kPlaceholder, kFile);
    remove(0x0001000000002068, kData, L"gone.dll", kPlaceholder);
    // Not a container's: a stub an archiving product left that programs see as one, a cloud
    // file's placeholder, and one newer than the trace.
    placeholder(0x0001000000002069, L"archived.dat", kRead, kPlaceholder, kPlaceholder);
    placeholder(0x000100000000206A, L"cloud.docx", kListed,
                kPlaceholder | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS,
                kFile | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS);
    placeholder(0x000100000000206D, L"fresh.dll", kRead, kPlaceholder, kFile, kTraceStart + 1);
    // An installer's own file and junction.
    create(0x000100000000206B, kData, L"real.txt");
    create(0x000100000000206C, kData, L"link", kFolder);
    record(0x000100000000206C, kData, L"link", USN_REASON_FILE_CREATE | USN_REASON_REPARSE_POINT_CHANGE,
           kFolder | FILE_ATTRIBUTE_REPARSE_POINT);

    const std::vector<std::string> expected = {
        R"(modified Q:\Data\archived.dat)",  // Older than the trace, its creation not recorded
        R"(modified Q:\Data\cloud.docx)",
        R"(created Q:\Data\fresh.dll)",
        R"(deleted Q:\Data\gone.dll)",
        R"(created Q:\Data\link\)",
        R"(renamed Q:\Data\new.ini <- Q:\Data\old.ini)",
        R"(modified Q:\Data\patched.dll)",
        R"(created Q:\Data\real.txt)",
        R"(modified Q:\Data\secured.dll (metadata-only))",
    };
    EXPECT_EQ(resolve(kTraceStart), expected);
}

// Windows Sandbox's records of two base-layer files, one deleted and one renamed after takeown
// and icacls: the placeholder's own records, then a tombstone under the old name (a new item,
// here with the deleted one's record and the next sequence number).
TEST_F(FileChangesTest, AContainersTombstonesAreNoChange) {
    constexpr std::uint32_t kPlaceholder = 0x1620;  // Archive, sparse, reparse point, offline
    constexpr std::uint32_t kPinned = 0x0620;       // No longer offline
    constexpr std::uint32_t kTombstone = 0x0426;    // Hidden, system, archive, reparse point
    constexpr std::uint32_t kHydrated = 0x00100102;
    constexpr std::uint32_t kReparse = USN_REASON_REPARSE_POINT_CHANGE;
    constexpr std::uint32_t kSecurity = USN_REASON_SECURITY_CHANGE;
    constexpr std::uint32_t kClose = USN_REASON_CLOSE;
    constexpr std::uint64_t kDeleted = 0x00010000000028df;
    constexpr std::uint64_t kRenamed = 0x00010000000028e2;
    const auto prepared = [&](std::uint64_t frn, std::wstring_view name) {
        for (const std::uint32_t reasons : {kHydrated, kHydrated | kClose, kReparse, kSecurity | kReparse,
                                            kSecurity | kReparse | kClose, kSecurity, kSecurity | kClose}) {
            record(frn, kData, name, reasons, kPlaceholder);
        }
    };
    prepared(kDeleted, L"Alarm01.wav");
    record(kDeleted, kData, L"Alarm01.wav", USN_REASON_BASIC_INFO_CHANGE, kPinned);
    record(kDeleted, kData, L"Alarm01.wav", USN_REASON_BASIC_INFO_CHANGE | kClose, kPinned);
    record(kDeleted, kData, L"Alarm01.wav", USN_REASON_FILE_DELETE | kClose, kPinned);
    record(0x00020000000028df, kData, L"alarm01.wav", USN_REASON_FILE_CREATE | kReparse, kTombstone);
    record(0x00020000000028df, kData, L"alarm01.wav", USN_REASON_FILE_CREATE | kReparse | kClose, kTombstone);
    prepared(kRenamed, L"Alarm02.wav");
    record(kRenamed, kData, L"Alarm02.wav", kReparse, kPlaceholder);
    record(kRenamed, kData, L"Alarm02.wav", USN_REASON_RENAME_OLD_NAME | kReparse, kPlaceholder);
    record(kRenamed, kData, L"Alarm02.wav.avel", USN_REASON_RENAME_NEW_NAME | kReparse, kPlaceholder);
    record(kRenamed, kData, L"Alarm02.wav.avel", USN_REASON_RENAME_NEW_NAME | kReparse | kClose,
           kPlaceholder);
    record(0x00010000000028e3, kData, L"alarm02.wav", USN_REASON_FILE_CREATE | kReparse, kTombstone);
    record(0x00010000000028e3, kData, L"alarm02.wav", USN_REASON_FILE_CREATE | kReparse | kClose, kTombstone);
    resolver_.add_created(LR"(Q:\Data\Alarm02.wav.avel)", 1);
    resolver_.add_attributes(LR"(Q:\Data\Alarm02.wav.avel)", kFile);
    // A hidden, system reparse point that programs do see is no tombstone.
    record(0x00010000000028e4, kData, L"desktop.lnk", USN_REASON_FILE_CREATE | kReparse, kTombstone);
    resolver_.add_id(LR"(Q:\Data\desktop.lnk)", 0x00010000000028e4);

    const std::vector<std::string> expected = {
        R"(deleted Q:\Data\Alarm01.wav)",
        R"(renamed Q:\Data\Alarm02.wav.avel <- Q:\Data\Alarm02.wav)",
        R"(created Q:\Data\desktop.lnk)",
    };
    EXPECT_EQ(resolve(100), expected);
}

TEST_F(FileChangesTest, ARenamedFileIsRenamed) {
    rename(0x0001000000002020, kData, L"old name.txt", kProgramFiles, L"new name.txt");
    write(0x0001000000002020, kProgramFiles, L"new name.txt");
    EXPECT_EQ(resolve(),
              std::vector<std::string>{R"(renamed Q:\Program Files\new name.txt <- Q:\Data\old name.txt)"});
}

TEST_F(FileChangesTest, AFileRenamedAndBackIsOnlyModified) {
    rename(0x0001000000002021, kData, L"settings.ini", kData, L"settings.bak");
    rename(0x0001000000002022, kData, L"untouched.ini", kData, L"untouched.bak");
    write(0x0001000000002021, kData, L"settings.bak");
    rename(0x0001000000002021, kData, L"settings.bak", kData, L"settings.ini");
    rename(0x0001000000002022, kData, L"untouched.bak", kData, L"untouched.ini");
    EXPECT_EQ(resolve(), std::vector<std::string>{R"(modified Q:\Data\settings.ini)"});
}

TEST_F(FileChangesTest, AFolderRenamedAfterItsChildsLastRecord) {
    constexpr std::uint64_t kContoso = 0x0001000000002030;
    create(0x0001000000002031, kContoso, L"app.exe");
    rename(kContoso, kProgramFiles, L"Contoso", kProgramFiles, L"Contoso Suite", kFolder);
    const std::vector<std::string> expected = {
        R"(renamed Q:\Program Files\Contoso Suite\ <- Q:\Program Files\Contoso)",
        R"(created Q:\Program Files\Contoso Suite\app.exe)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, PathsAtTheStartFollowARenamedAncestor) {
    constexpr std::uint64_t kOld = 0x0001000000002040;
    constexpr std::uint64_t kSub = 0x0001000000002041;  // Has no records of its own
    resolver_.add_folder(kOld, kData, L"New");          // As it is now
    resolver_.add_folder(kSub, kOld, L"Sub");
    rename(kOld, kData, L"Old", kData, L"New", kFolder);
    remove(0x0001000000002042, kSub, L"gone.txt");
    write(0x0001000000002043, kSub, L"kept.txt");
    const std::vector<std::string> expected = {
        R"(renamed Q:\Data\New\ <- Q:\Data\Old)",
        R"(modified Q:\Data\New\Sub\kept.txt)",
        R"(deleted Q:\Data\Old\Sub\gone.txt)",
    };
    EXPECT_EQ(resolve(), expected);
}

// A folder filled before the trace and moved into another folder as a whole during it (Edge's
// component unpacker does this): the move's records are the folder's only ones, and what it holds
// has none. One rename, whose old path stands for everything that went away with it.
TEST_F(FileChangesTest, AFolderMovedAsAWholeIsOneRename) {
    constexpr std::uint64_t kUnpacked = 0x0001000000002044;
    resolver_.add_folder(kUnpacked, kProgramFiles, L"120.0");  // As it is now
    rename(kUnpacked, kData, L"Unpacker_4732", kProgramFiles, L"120.0", kFolder);
    EXPECT_EQ(resolve(),
              std::vector<std::string>{R"(renamed Q:\Program Files\120.0\ <- Q:\Data\Unpacker_4732)"});
}

// Adding or removing a hard link writes one record, with CLOSE, naming that link; later records
// name the link the file was opened through.
constexpr std::uint32_t kLinkChange = USN_REASON_HARD_LINK_CHANGE | USN_REASON_CLOSE;

TEST_F(FileChangesTest, AHardLinkAddedToAnOlderFileIsANewName) {
    constexpr std::uint64_t kShared = 0x0004000000002050;
    resolver_.add_id(LR"(Q:\Data\shared.dll)", kShared);
    resolver_.add_id(LR"(Q:\Program Files\shared-link.dll)", kShared);
    resolver_.add_created(LR"(Q:\Program Files\shared-link.dll)", 1000);  // The file's own creation time
    record(kShared, kProgramFiles, L"shared-link.dll", kLinkChange, kFile);
    write(kShared, kProgramFiles, L"shared-link.dll");
    // A link added and removed again.
    record(kShared, kData, L"temporary.dll", kLinkChange, kFile);
    record(kShared, kData, L"temporary.dll", kLinkChange, kFile);
    EXPECT_EQ(resolve(2000), std::vector<std::string>{R"(created Q:\Program Files\shared-link.dll)"});
}

TEST_F(FileChangesTest, AnOlderFileWrittenThroughANewLinkIsModifiedUnderItsOwnName) {
    constexpr std::uint64_t kShared = 0x0004000000002052;
    resolver_.add_id(LR"(Q:\Data\shared.dll)", kShared);
    resolver_.add_id(LR"(Q:\Program Files\shared-link.dll)", kShared);
    write(kShared, kData, L"shared.dll");
    record(kShared, kProgramFiles, L"shared-link.dll", kLinkChange, kFile);
    write(kShared, kProgramFiles, L"shared-link.dll");
    const std::vector<std::string> expected = {
        R"(modified Q:\Data\shared.dll)",
        R"(created Q:\Program Files\shared-link.dll)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, ACreatedFileIsListedUnderEachOfItsLinks) {
    constexpr std::uint64_t kTool = 0x0001000000002051;
    resolver_.add_id(LR"(Q:\Data\tool.exe)", kTool);
    resolver_.add_id(LR"(Q:\Program Files\tool.exe)", kTool);
    create(kTool, kData, L"tool.exe");
    record(kTool, kProgramFiles, L"tool.exe", kLinkChange, kFile);
    record(kTool, kData, L"tool-old.exe", kLinkChange, kFile);  // Added, then removed
    record(kTool, kData, L"tool-old.exe", kLinkChange, kFile);
    const std::vector<std::string> expected = {
        R"(created Q:\Data\tool.exe)",
        R"(created Q:\Program Files\tool.exe)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, AReusedRecordIsAnotherFile) {
    // MFT record 0x3000, first with sequence 7, then reused with sequence 8.
    remove(0x0007000000003000, kData, L"old.txt");
    create(0x0008000000003000, kData, L"new.txt");
    const std::vector<std::string> expected = {
        R"(created Q:\Data\new.txt)",
        R"(deleted Q:\Data\old.txt)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, CreatedItemsAreListedAsRootsWithTheirContents) {
    constexpr std::uint64_t kContoso = 0x0001000000004000;
    constexpr std::uint64_t kSub = 0x0001000000004002;
    constexpr std::uint64_t kDeep = 0x0001000000004004;
    create(kContoso, kProgramFiles, L"Contoso", kFolder);
    create(0x0001000000004001, kContoso, L"app.exe");
    create(kSub, kContoso, L"Sub", kFolder);
    create(0x0001000000004003, kSub, L"b.dat");
    create(kDeep, kSub, L"Deep", kFolder);
    create(0x0001000000004005, kDeep, L"c.dat");
    create(0x0001000000004006, kData, L"loose.txt");
    const std::vector<std::string> expected = {
        R"(created Q:\Data\loose.txt)",
        R"(created Q:\Program Files\Contoso\ +5)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, AFolderSomethingOlderWasMovedIntoIsNotOfferedWhole) {
    constexpr std::uint64_t kContoso = 0x0001000000005000;
    constexpr std::uint64_t kSub = 0x0001000000005002;
    constexpr std::uint64_t kClean = 0x0001000000005004;
    create(kContoso, kProgramFiles, L"Contoso", kFolder);
    create(0x0001000000005001, kContoso, L"a.txt");
    create(kSub, kContoso, L"Sub", kFolder);
    create(0x0001000000005003, kSub, L"b.txt");
    create(kClean, kContoso, L"Clean", kFolder);
    create(0x0001000000005005, kClean, L"c.txt");
    rename(0x0002000000005006, kData, L"x.cfg", kSub, L"x.cfg");  // Existed before the trace
    const std::vector<std::string> expected = {
        R"(created Q:\Program Files\Contoso\a.txt)",
        R"(created Q:\Program Files\Contoso\Clean\ +1)",
        R"(created Q:\Program Files\Contoso\Sub\b.txt)",
        R"(renamed Q:\Program Files\Contoso\Sub\x.cfg <- Q:\Data\x.cfg)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, AFolderReplacedByANewOneKeepsOnlyItsNewItemsApart) {
    constexpr std::uint64_t kOldFolder = 0x0002000000006000;
    constexpr std::uint64_t kNewFolder = 0x0001000000006002;
    remove(0x0002000000006001, kOldFolder, L"app.exe");
    remove(kOldFolder, kProgramFiles, L"Contoso", kFolder);
    create(kNewFolder, kProgramFiles, L"Contoso", kFolder);
    create(0x0001000000006003, kNewFolder, L"app.exe");
    create(0x0001000000006004, kNewFolder, L"readme.txt");
    const std::vector<std::string> expected = {
        R"(modified Q:\Program Files\Contoso\ replaced)",
        R"(modified Q:\Program Files\Contoso\app.exe replaced)",
        R"(created Q:\Program Files\Contoso\readme.txt)",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, ItemsOpenAcrossTheStartAreOnlyModified) {
    testing::TempFolder temp;
    testing::write_file(temp / L"early.log", "before the trace");
    testing::write_file(temp / L"fresh.log", "its creation time came from an archive");
    testing::make_folder(temp / L"early folder");
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    const std::int64_t trace_start =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(now.dwHighDateTime) << 32) |
                                  now.dwLowDateTime) +
        10'000'000;  // A second after the files were made
    resolver_.add_fixed(kTemp, temp.path());

    // Opened before the trace: its records carry FILE_CREATE along, but none is the creation.
    record(0x0001000000007000, kTemp, L"early.log", USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND, kFile);
    record(0x0001000000007000, kTemp, L"early.log",
           USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND | USN_REASON_CLOSE, kFile);
    // Created during the trace, with an older creation time set by whoever wrote it.
    create(0x0001000000007001, kTemp, L"fresh.log");
    record(0x0001000000007002, kTemp, L"early folder", USN_REASON_FILE_CREATE | USN_REASON_CLOSE, kFolder);
    create(0x0001000000007003, 0x0001000000007002, L"new.txt");

    const std::wstring folder = platform::normalize_path(temp.path());
    const std::vector<std::string> expected = {
        "modified " + to_utf8(folder) + R"(\early folder\)",
        "created " + to_utf8(folder) + R"(\early folder\new.txt)",
        "modified " + to_utf8(folder) + R"(\early.log)",
        "created " + to_utf8(folder) + R"(\fresh.log)",
    };
    EXPECT_EQ(resolve(trace_start), expected);
}

TEST_F(FileChangesTest, ItemsInUnknownFoldersAreUnresolved) {
    constexpr std::uint64_t kUnknown = 0x0001000000008000;
    create(0x0001000000008001, kUnknown, L"orphan.txt");
    remove(0x0001000000008002, kUnknown, L"gone.txt");
    const std::vector<std::string> expected = {
        R"(deleted Q:\?\gone.txt (unresolved))",
        R"(created Q:\?\orphan.txt (unresolved))",
    };
    EXPECT_EQ(resolve(), expected);
}

TEST_F(FileChangesTest, TheAggregatorKeepsTheNamesBeforeAndAfter) {
    constexpr std::uint64_t kItem = 0x0001000000009000;
    write(kItem, kData, L"a.txt");
    rename(kItem, kData, L"a.txt", kProgramFiles, L"b.txt");
    rename(kItem, kProgramFiles, L"b.txt", kProgramFiles, L"c.txt");
    const auto& entry = changes_.entries().at(kItem);
    ASSERT_TRUE(entry.first && entry.last);
    EXPECT_EQ(*entry.first, (FileChangeAggregator::Name{kData, L"a.txt"}));
    EXPECT_EQ(*entry.last, (FileChangeAggregator::Name{kProgramFiles, L"c.txt"}));
    EXPECT_EQ(entry.first_reasons, static_cast<std::uint32_t>(USN_REASON_DATA_OVERWRITE));
    EXPECT_EQ(entry.open_reasons, 0u);
    EXPECT_TRUE(entry.links.empty());
}

class FileChangeCheckpointTest : public FileChangesTest {
protected:
    void SetUp() override {
        FileChangesTest::SetUp();
        create(0x0001000000002000, kProgramFiles, L"Contoso", kFolder);
        create(0x0001000000002001, 0x0001000000002000, L"app.exe");
        rename(0x0003000000002002, kData, L"old.ini", kData, L"new.ini");
        record(0x0004000000002003, kData, L"link.dll", kLinkChange, kFile);
        record(0x0001000000002004, kData, L"open.log", USN_REASON_DATA_EXTEND, kFile);  // Still open
        remove(0x0005000000002005, kData, L"gone.txt");
    }
};

TEST_F(FileChangeCheckpointTest, SavesAndLoadsEverything) {
    const auto bytes = changes_.save();
    const auto loaded = FileChangeAggregator::load(bytes);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->entries(), changes_.entries());
    EXPECT_EQ(loaded->save(), bytes);
    EXPECT_EQ(resolve(*loaded), resolve(changes_));
    EXPECT_EQ(changes_.entries().at(0x0001000000002004).open_reasons,
              static_cast<std::uint32_t>(USN_REASON_DATA_EXTEND));
}

TEST_F(FileChangeCheckpointTest, RejectsADamagedCheckpoint) {
    const auto bytes = changes_.save();
    for (const std::size_t at : {std::size_t{0}, std::size_t{9}, bytes.size() / 2, bytes.size() - 1}) {
        auto damaged = bytes;
        damaged[at] ^= std::byte{0x40};
        EXPECT_FALSE(FileChangeAggregator::load(damaged)) << at;
    }
    EXPECT_FALSE(FileChangeAggregator::load(std::span(bytes).first(bytes.size() - 1)));
    EXPECT_FALSE(FileChangeAggregator::load({}));

    // Another version, with a checksum that matches.
    auto other_version = std::vector<std::byte>(bytes.begin(), bytes.end() - sizeof(std::uint64_t));
    other_version[4] = std::byte{2};
    const std::uint64_t checksum = platform::fnv1a(other_version.data(), other_version.size());
    const auto* checksum_bytes = reinterpret_cast<const std::byte*>(&checksum);
    other_version.insert(other_version.end(), checksum_bytes, checksum_bytes + sizeof(checksum));
    EXPECT_FALSE(FileChangeAggregator::load(other_version));

    // An empty aggregator round-trips too.
    const auto empty = FileChangeAggregator::load(FileChangeAggregator().save());
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->empty());
}

}  // namespace
}  // namespace avel::tracker
