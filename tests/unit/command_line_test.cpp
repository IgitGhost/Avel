#include <gtest/gtest.h>

#include <set>
#include <string>

#include "core/platform/command_line.h"

namespace avel::platform {
namespace {

// A fake file system containing only the given files.
FileExistsFunction files(std::set<std::wstring> existing) {
    return [existing = std::move(existing)](const std::wstring& path) { return existing.contains(path); };
}

TEST(CommandLineTest, QuotedExecutable) {
    const auto parsed = split_command_line(LR"("C:\Program Files\App\uninst.exe" /S /D=x)", files({}));
    EXPECT_EQ(parsed.executable, LR"(C:\Program Files\App\uninst.exe)");
    EXPECT_EQ(parsed.arguments, L"/S /D=x");
}

TEST(CommandLineTest, UnterminatedQuoteTakesTheRest) {
    const auto parsed = split_command_line(LR"("C:\App\uninst.exe)", files({}));
    EXPECT_EQ(parsed.executable, LR"(C:\App\uninst.exe)");
    EXPECT_EQ(parsed.arguments, L"");
}

TEST(CommandLineTest, UnquotedPathWithSpacesIsResolvedLikeCreateProcess) {
    const auto parsed = split_command_line(LR"(C:\Program Files\App\uninst.exe /S)",
                                           files({LR"(C:\Program Files\App\uninst.exe)"}));
    EXPECT_EQ(parsed.executable, LR"(C:\Program Files\App\uninst.exe)");
    EXPECT_EQ(parsed.arguments, L"/S");
}

TEST(CommandLineTest, ShorterExistingPrefixWins) {
    // CreateProcess would run C:\Program.exe here too; that's the classic unquoted-path hazard.
    const auto parsed =
        split_command_line(LR"(C:\Program Files\App\uninst.exe)", files({LR"(C:\Program.exe)"}));
    EXPECT_EQ(parsed.executable, LR"(C:\Program.exe)");
    EXPECT_EQ(parsed.arguments, LR"(Files\App\uninst.exe)");
}

TEST(CommandLineTest, MissingFileFallsBackToFirstExePrefix) {
    const auto parsed = split_command_line(LR"(C:\Program Files\Gone\uninst.exe /S)", files({}));
    EXPECT_EQ(parsed.executable, LR"(C:\Program Files\Gone\uninst.exe)");
    EXPECT_EQ(parsed.arguments, L"/S");
}

TEST(CommandLineTest, RelativeMsiExec) {
    const auto parsed =
        split_command_line(L"MsiExec.exe /X{12345678-1234-1234-1234-123456789ABC}", files({}));
    EXPECT_EQ(parsed.executable, L"MsiExec.exe");
    EXPECT_EQ(parsed.arguments, L"/X{12345678-1234-1234-1234-123456789ABC}");
}

TEST(CommandLineTest, NoExeAnywhereSplitsAtFirstSpace) {
    const auto parsed = split_command_line(L"rundll32 shell32.dll,Control_RunDLL", files({}));
    EXPECT_EQ(parsed.executable, L"rundll32");
    EXPECT_EQ(parsed.arguments, L"shell32.dll,Control_RunDLL");
}

TEST(CommandLineTest, AppendsExeWhenProbing) {
    const auto parsed =
        split_command_line(LR"(C:\Tools\uninstall --force)", files({LR"(C:\Tools\uninstall.exe)"}));
    EXPECT_EQ(parsed.executable, LR"(C:\Tools\uninstall.exe)");
    EXPECT_EQ(parsed.arguments, L"--force");
}

TEST(CommandLineTest, EmptyAndWhitespace) {
    EXPECT_EQ(split_command_line(L"", files({})).executable, L"");
    EXPECT_EQ(split_command_line(L"   ", files({})).executable, L"");
}

}  // namespace
}  // namespace avel::platform
