#include <gtest/gtest.h>

#include <string>

#include "core/uninstall/uninstall_plan.h"

namespace avel::uninstall {
namespace {

using inventory::InstallerType;
using inventory::Program;
using inventory::Scope;
using inventory::Source;

constexpr wchar_t kMe[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kCode[] = L"{605108C1-153E-43D8-8A67-7CE326B00ECA}";

PlanContext context() {
    PlanContext context;
    context.current_user_sid = kMe;
    context.elevated = true;
    context.file_exists = [](const std::wstring&) { return true; };
    return context;
}

Program desktop_app(std::wstring uninstall, InstallerType type = InstallerType::Unknown) {
    Program program;
    program.display_name = L"App";
    program.uninstall_string = std::move(uninstall);
    program.installer_type = type;
    return program;
}

TEST(UninstallPlanTest, WindowsInstallerProductsUseMsiexecWithTheProductCode) {
    Program program = desktop_app(L"MsiExec.exe /I" + std::wstring(kCode), InstallerType::Msi);
    program.windows_installer = true;
    program.msi_product_code = kCode;

    const auto interactive = make_uninstall_plan(program, Mode::Interactive, context());
    EXPECT_TRUE(interactive.possible());
    EXPECT_EQ(interactive.method, Method::Msi);
    EXPECT_EQ(interactive.executable, L"msiexec.exe");
    EXPECT_EQ(interactive.arguments, L"/x " + std::wstring(kCode));  // /x, not the registered /I
    EXPECT_FALSE(interactive.quiet);

    const auto quiet = make_uninstall_plan(program, Mode::Quiet, context());
    EXPECT_EQ(quiet.arguments, L"/x " + std::wstring(kCode) + L" /qn /norestart");
    EXPECT_TRUE(quiet.quiet);
    EXPECT_EQ(quiet.command_line(), L"msiexec.exe /x " + std::wstring(kCode) + L" /qn /norestart");
}

TEST(UninstallPlanTest, ProductCodeComesFromAnMsiexecCommand) {
    const Program program = desktop_app(L"MsiExec.exe /X" + std::wstring(kCode), InstallerType::Msi);
    const auto plan = make_uninstall_plan(program, Mode::Quiet, context());
    EXPECT_EQ(plan.method, Method::Msi);
    EXPECT_EQ(plan.arguments, L"/x " + std::wstring(kCode) + L" /qn /norestart");
}

TEST(UninstallPlanTest, MsiOnlyProducts) {
    Program program;
    program.source = Source::Msi;
    program.msi_product_code = kCode;
    const auto plan = make_uninstall_plan(program, Mode::Interactive, context());
    EXPECT_TRUE(plan.possible());
    EXPECT_EQ(plan.method, Method::Msi);
}

TEST(UninstallPlanTest, SilentSwitchesByTechnology) {
    struct Case {
        std::wstring uninstall;
        InstallerType type;
        std::wstring quiet_arguments;
    };
    const Case cases[] = {
        {LR"("C:\Program Files\App\unins000.exe")", InstallerType::InnoSetup,
         L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"},
        {LR"("C:\Program Files\App\uninstall.exe")", InstallerType::Nsis, L"/S"},
        {LR"("C:\Program Files\App\uninstall.exe" _?=C:\Program Files\App)", InstallerType::Nsis,
         LR"(/S _?=C:\Program Files\App)"},  // _?= must stay last
        {LR"("C:\ProgramData\Package Cache\{3A1B2C3D-1111-2222-3333-444455556666}\vc_redist.x64.exe"  /uninstall)",
         InstallerType::WixBurn, L"/uninstall /quiet /norestart"},
        {LR"("C:\Users\me\AppData\Local\Discord\Update.exe" --uninstall)", InstallerType::Squirrel,
         L"--uninstall -s"},
        {LR"(C:\Windows\system32\sdbinst.exe -u "C:\Windows\AppPatch\Custom\app.sdb")",
         InstallerType::SdbInst, LR"(-u "C:\Windows\AppPatch\Custom\app.sdb" -q)"},
        {LR"("C:\Program Files\App\unins000.exe" /VERYSILENT)", InstallerType::InnoSetup, L"/VERYSILENT"},
    };
    for (const auto& c : cases) {
        const auto plan = make_uninstall_plan(desktop_app(c.uninstall, c.type), Mode::Quiet, context());
        EXPECT_TRUE(plan.quiet) << c.uninstall;
        EXPECT_EQ(plan.method, Method::Command) << c.uninstall;
        EXPECT_EQ(plan.arguments, c.quiet_arguments) << c.uninstall;
    }
}

TEST(UninstallPlanTest, InteractiveModeKeepsTheCommandAsRegistered) {
    const auto plan =
        make_uninstall_plan(desktop_app(LR"("C:\Program Files\App\unins000.exe")", InstallerType::InnoSetup),
                            Mode::Interactive, context());
    EXPECT_FALSE(plan.quiet);
    EXPECT_EQ(plan.executable, LR"(C:\Program Files\App\unins000.exe)");
    EXPECT_TRUE(plan.arguments.empty());
    EXPECT_EQ(plan.command_line(), LR"("C:\Program Files\App\unins000.exe")");
}

TEST(UninstallPlanTest, UnknownInstallersAreDetectedFromTheExecutable) {
    PlanContext ctx = context();
    std::wstring probed;
    ctx.detect_installer = [&probed](const std::wstring& executable) {
        probed = executable;
        return InstallerType::Nsis;
    };
    const auto plan =
        make_uninstall_plan(desktop_app(LR"("C:\Program Files\Notepad++\uninstall.exe")"), Mode::Quiet, ctx);
    EXPECT_EQ(probed, LR"(C:\Program Files\Notepad++\uninstall.exe)");
    EXPECT_EQ(plan.installer_type, InstallerType::Nsis);
    EXPECT_EQ(plan.arguments, L"/S");
}

TEST(UninstallPlanTest, WithoutAKnownSwitchQuietModeFallsBackToInteractive) {
    const auto plan =
        make_uninstall_plan(desktop_app(LR"("C:\Program Files\App\remove.exe" -x)"), Mode::Quiet, context());
    EXPECT_TRUE(plan.possible());
    EXPECT_FALSE(plan.quiet);
    EXPECT_EQ(plan.arguments, L"-x");
}

TEST(UninstallPlanTest, TheVendorsQuietCommandWins) {
    Program program = desktop_app(LR"("C:\Program Files\App\uninst.exe")", InstallerType::Nsis);
    program.quiet_uninstall_string = LR"("C:\Program Files\App\uninst.exe" /S /KeepSettings)";
    const auto quiet = make_uninstall_plan(program, Mode::Quiet, context());
    EXPECT_TRUE(quiet.quiet);
    EXPECT_EQ(quiet.arguments, L"/S /KeepSettings");

    const auto interactive = make_uninstall_plan(program, Mode::Interactive, context());
    EXPECT_TRUE(interactive.arguments.empty());
}

TEST(UninstallPlanTest, PackagedAppsGoThroughThePackageManager) {
    Program program;
    program.source = Source::Appx;
    program.scope = Scope::User;
    program.user_sid = kMe;
    program.package_full_name = L"Contoso.App_1.0.0.0_x64__abcdefghijklm";
    PlanContext ctx = context();
    ctx.elevated = false;  // Per-user removal needs no elevation

    const auto plan = make_uninstall_plan(program, Mode::Interactive, ctx);
    EXPECT_TRUE(plan.possible());
    EXPECT_EQ(plan.method, Method::Package);
    EXPECT_TRUE(plan.quiet);
    EXPECT_EQ(plan.package_full_name, program.package_full_name);
    EXPECT_TRUE(plan.command_line().empty());
}

TEST(UninstallPlanTest, ReasonsAProgramCantBeUninstalled) {
    Program blocked = desktop_app(LR"("C:\App\uninst.exe")");
    blocked.no_remove = true;
    EXPECT_EQ(make_uninstall_plan(blocked, Mode::Interactive, context()).error, PlanError::RemovalBlocked);

    Program someone_elses = desktop_app(LR"("C:\Users\other\AppData\Local\App\uninst.exe")");
    someone_elses.scope = Scope::User;
    someone_elses.user_sid = L"S-1-5-21-1-2-3-1002";
    EXPECT_EQ(make_uninstall_plan(someone_elses, Mode::Interactive, context()).error, PlanError::OtherUser);

    Program mine = someone_elses;
    mine.user_sid = L"s-1-5-21-1-2-3-1001";  // SIDs compare case-insensitively
    EXPECT_TRUE(make_uninstall_plan(mine, Mode::Interactive, context()).possible());

    PlanContext not_elevated = context();
    not_elevated.elevated = false;
    EXPECT_EQ(
        make_uninstall_plan(desktop_app(LR"("C:\App\uninst.exe")"), Mode::Interactive, not_elevated).error,
        PlanError::ElevationRequired);
    EXPECT_TRUE(make_uninstall_plan(mine, Mode::Interactive, not_elevated).possible());

    EXPECT_EQ(make_uninstall_plan(desktop_app(L""), Mode::Interactive, context()).error,
              PlanError::NoUninstaller);
    EXPECT_EQ(make_uninstall_plan(desktop_app(L"steam://uninstall/440", InstallerType::Steam),
                                  Mode::Interactive, context())
                  .error,
              PlanError::UnsupportedCommand);
}

}  // namespace
}  // namespace avel::uninstall
