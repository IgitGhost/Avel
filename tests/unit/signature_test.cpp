// Authenticode checks on files every Windows machine has: Windows' own files are signed through
// catalogs and come out as Windows' own; Microsoft's applications and WHQL-signed driver packages
// (where the machine has them) are Microsoft-signed but not Windows' own; the test helper Avel
// builds is unsigned; a missing file is missing. Read-only, and never on the network.
#include <gtest/gtest.h>

#include <windows.h>

#include <wil/resource.h>

#include <string>

#include "core/platform/signature.h"
#include "core/platform/text.h"
#include "test_spawner.h"

namespace avel::platform {
namespace {

std::wstring system_file(std::wstring_view name) {
    wchar_t folder[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(folder, MAX_PATH);
    return std::wstring(folder, length) + L"\\" + std::wstring(name);
}

TEST(SignatureTest, WindowsFilesAreSignedByMicrosoft) {
    // cmd.exe, notepad.exe and wevtsvc.dll carry no signature of their own: a catalog lists them.
    // kernel32.dll, svchost.exe and ntoskrnl.exe are signed themselves (and listed too). Each is
    // Microsoft's.
    bool catalog = false;
    for (const auto* name :
         {L"cmd.exe", L"notepad.exe", L"wevtsvc.dll", L"kernel32.dll", L"svchost.exe", L"ntoskrnl.exe"}) {
        const std::wstring path = system_file(name);
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;  // notepad.exe isn't on every edition
        }
        const Signature signature = verify_signature(path);
        EXPECT_EQ(signature.state, SignatureState::Valid) << to_utf8(path);
        EXPECT_EQ(signature.root, MicrosoftRoot::Product) << to_utf8(path);
        EXPECT_TRUE(signature.windows) << to_utf8(path);
        EXPECT_TRUE(platform::istarts_with(signature.signer, L"Microsoft")) << to_utf8(signature.signer);
        catalog = catalog || signature.catalog;
    }
    EXPECT_TRUE(catalog);  // The catalog path ran and found Microsoft too
}

// Microsoft's programs that aren't part of Windows are signed through its application root: they
// are Microsoft's, not Windows' own. Edge is on most machines, though not every CI runner.
TEST(SignatureTest, MicrosoftsApplicationsArentWindowsOwn) {
    std::wstring found;
    for (const auto* variable : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        wchar_t folder[MAX_PATH] = {};
        const DWORD length = ::GetEnvironmentVariableW(variable, folder, MAX_PATH);
        const std::wstring path = std::wstring(folder, length) + LR"(\Microsoft\Edge\Application\msedge.exe)";
        if (length > 0 && length < MAX_PATH &&
            ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            found = path;
            break;
        }
    }
    if (found.empty()) {
        GTEST_SKIP() << "Microsoft Edge isn't installed here";
    }
    const Signature signature = verify_signature(found);
    EXPECT_EQ(signature.state, SignatureState::Valid);
    EXPECT_EQ(signature.root, MicrosoftRoot::Application);
    EXPECT_FALSE(signature.windows);
    EXPECT_TRUE(platform::istarts_with(signature.signer, L"Microsoft")) << to_utf8(signature.signer);
}

// Hardware vendors' driver packages are signed by Microsoft's WHQL publisher, under a product root:
// they are the vendor's, not Windows' own. Their catalogs in the DriverStore carry that signature;
// a machine without third-party drivers has none.
TEST(SignatureTest, WhqlSignedDriverPackagesArentWindowsOwn) {
    wchar_t windows[MAX_PATH] = {};
    const UINT length = ::GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring store = std::wstring(windows, length) + LR"(\System32\DriverStore\FileRepository)";
    std::size_t checked = 0;
    bool found = false;
    WIN32_FIND_DATAW package{};
    const wil::unique_hfind packages(::FindFirstFileExW((store + L"\\*").c_str(), FindExInfoBasic, &package,
                                                        FindExSearchLimitToDirectories, nullptr, 0));
    for (bool more = static_cast<bool>(packages); more && !found && checked < 200;
         more = ::FindNextFileW(packages.get(), &package) != FALSE) {
        if (package.cFileName[0] == L'.' || !(package.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        const std::wstring folder = store + L"\\" + package.cFileName;
        WIN32_FIND_DATAW file{};
        const wil::unique_hfind catalogs(::FindFirstFileExW((folder + L"\\*.cat").c_str(), FindExInfoBasic,
                                                            &file, FindExSearchNameMatch, nullptr, 0));
        if (!catalogs) {
            continue;  // An inbox driver: its catalog is in CatRoot
        }
        ++checked;
        const Signature signature = verify_signature(folder + L"\\" + file.cFileName);
        if (signature.state == SignatureState::Valid &&
            platform::iequals(signature.signer, L"Microsoft Windows Hardware Compatibility Publisher")) {
            found = true;
            EXPECT_EQ(signature.root, MicrosoftRoot::Product) << to_utf8(folder);
            EXPECT_FALSE(signature.windows) << to_utf8(folder);
        }
    }
    if (!found) {
        GTEST_SKIP() << "No WHQL-signed driver package here";
    }
}

TEST(SignatureTest, TheTestHelperIsUnsigned) {
    const Signature signature = verify_signature(testing::spawner_path());
    EXPECT_EQ(signature.state, SignatureState::Unsigned);
    EXPECT_EQ(signature.root, MicrosoftRoot::None);
    EXPECT_FALSE(signature.windows);
    EXPECT_FALSE(signature.catalog);
    EXPECT_TRUE(signature.signer.empty());
}

TEST(SignatureTest, AMissingFileIsMissing) {
    EXPECT_EQ(verify_signature(system_file(L"avel-tests-no-such-file.exe")).state, SignatureState::Missing);
    EXPECT_EQ(verify_signature(LR"(Z:\avel\tests\no\such\folder\x.exe)").state, SignatureState::Missing);
    EXPECT_EQ(verify_signature(L"").state, SignatureState::Missing);
    EXPECT_FALSE(verify_signature(system_file(L"avel-tests-no-such-file.exe")).windows);
}

TEST(SignatureTest, TheCacheGivesTheSameAnswer) {
    SignatureCache cache;
    const std::wstring path = system_file(L"notepad.exe");
    const Signature first = cache.get(path);
    std::wstring upper = path;
    ::CharUpperBuffW(upper.data(), static_cast<DWORD>(upper.size()));
    const Signature again = cache.get(upper);  // Paths compare ignoring case
    EXPECT_EQ(first.state, SignatureState::Valid);
    EXPECT_EQ(again.state, first.state);
    EXPECT_EQ(again.signer, first.signer);
    EXPECT_EQ(again.root, first.root);
    EXPECT_EQ(again.windows, first.windows);
    EXPECT_EQ(again.catalog, first.catalog);
    EXPECT_EQ(cache.get(testing::spawner_path()).state, SignatureState::Unsigned);
}

TEST(SignatureTest, AFolderIsAnError) {
    wchar_t folder[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(folder, MAX_PATH);
    EXPECT_EQ(verify_signature(std::wstring(folder, length)).state, SignatureState::Error);
}

}  // namespace
}  // namespace avel::platform
