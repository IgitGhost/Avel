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

TEST(CommandLineTest, MissingUninstallerByFullPath) {
    EXPECT_EQ(missing_command_file(LR"("C:\Program Files\App\unins000.exe" /SILENT)", files({})),
              LR"(C:\Program Files\App\unins000.exe)");
    EXPECT_EQ(missing_command_file(LR"(C:\Program Files\App\uninst.exe /S)", files({})),
              LR"(C:\Program Files\App\uninst.exe)");
    EXPECT_EQ(missing_command_file(LR"("C:\Program Files\App\unins000.exe" /SILENT)",
                                   files({LR"(C:\Program Files\App\unins000.exe)"})),
              L"");
    // A bare name is found through the search path; it can't be called missing.
    EXPECT_EQ(missing_command_file(L"MsiExec.exe /X{605108C1-153E-43D8-8A67-7CE326B00ECA}", files({})), L"");
    EXPECT_EQ(missing_command_file(L"", files({})), L"");
}

TEST(CommandLineTest, MissingScriptOrDllOfAWindowsHost) {
    const auto system =
        files({LR"(C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe)",
               LR"(C:\Windows\System32\rundll32.exe)", LR"(C:\Windows\System32\wscript.exe)"});
    EXPECT_EQ(
        missing_command_file(
            LR"(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\Program Files\App\uninstall.ps1" -Quiet)",
            system),
        LR"(C:\Program Files\App\uninstall.ps1)");
    EXPECT_EQ(
        missing_command_file(
            LR"("C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -f C:\App\remove.ps1)", system),
        LR"(C:\App\remove.ps1)");
    EXPECT_EQ(
        missing_command_file(LR"(C:\Windows\System32\rundll32.exe "C:\App\setup.dll",Uninstall)", system),
        LR"(C:\App\setup.dll)");
    EXPECT_EQ(missing_command_file(LR"(rundll32.exe C:\App\setup.dll,Uninstall /q)", system),
              LR"(C:\App\setup.dll)");
    EXPECT_EQ(missing_command_file(LR"(wscript.exe //B "C:\App\remove.vbs")", system),
              LR"(C:\App\remove.vbs)");
    // Other options of PowerShell aren't the script; a script that's there isn't missing.
    EXPECT_EQ(missing_command_file(LR"(powershell.exe -Command "& {Remove-Item x}")", system), L"");
    EXPECT_EQ(
        missing_command_file(LR"(powershell.exe -File C:\App\remove.ps1)", files({LR"(C:\App\remove.ps1)"})),
        L"");
}

TEST(CommandLineTest, HostedFileAsWritten) {
    const auto hosted = [](std::wstring_view command) {
        return hosted_file(split_command_line(command, files({})));
    };
    EXPECT_EQ(hosted(LR"(rundll32.exe "C:\App\x.dll",Entry)"), LR"(C:\App\x.dll)");
    EXPECT_EQ(hosted(LR"(pwsh -f %APPDATA%\x.ps1 -Quiet)"), LR"(%APPDATA%\x.ps1)");
    EXPECT_EQ(hosted(LR"(cscript.exe //Nologo x.vbs)"), L"x.vbs");
    EXPECT_EQ(hosted(LR"(powershell.exe -Command Start-Process x)"), L"");
    EXPECT_EQ(hosted(LR"("C:\App\app.exe" -File C:\App\x.ps1)"), L"");  // Not a Windows host
}

}  // namespace
}  // namespace avel::platform
