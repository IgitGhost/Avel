// Authenticode checks on files every Windows machine has: Windows' own files are signed through
// catalogs and come out as Microsoft's; the test helper Avel builds is unsigned; a missing file is
// missing. Read-only, and never on the network.
#include <gtest/gtest.h>

#include <windows.h>

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
        EXPECT_TRUE(signature.microsoft) << to_utf8(path);
        EXPECT_TRUE(platform::istarts_with(signature.signer, L"Microsoft")) << to_utf8(signature.signer);
        catalog = catalog || signature.catalog;
    }
    EXPECT_TRUE(catalog);  // The catalog path ran and found Microsoft too
}

TEST(SignatureTest, TheTestHelperIsUnsigned) {
    const Signature signature = verify_signature(testing::spawner_path());
    EXPECT_EQ(signature.state, SignatureState::Unsigned);
    EXPECT_FALSE(signature.microsoft);
    EXPECT_FALSE(signature.catalog);
    EXPECT_TRUE(signature.signer.empty());
}

TEST(SignatureTest, AMissingFileIsMissing) {
    EXPECT_EQ(verify_signature(system_file(L"avel-tests-no-such-file.exe")).state, SignatureState::Missing);
    EXPECT_EQ(verify_signature(LR"(Z:\avel\tests\no\such\folder\x.exe)").state, SignatureState::Missing);
    EXPECT_EQ(verify_signature(L"").state, SignatureState::Missing);
    EXPECT_FALSE(verify_signature(system_file(L"avel-tests-no-such-file.exe")).microsoft);
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
    EXPECT_EQ(again.microsoft, first.microsoft);
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
