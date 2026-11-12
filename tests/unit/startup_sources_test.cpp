// The platform helpers the Startup manager and the leftover scan share: shell links, the Task
// Scheduler and the Service Control Manager. The live tests only read.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <wil/com.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/platform/paths.h"
#include "core/platform/scheduled_tasks.h"
#include "core/platform/services.h"
#include "core/platform/shortcuts.h"
#include "core/platform/text.h"
#include "temp_root.h"

namespace avel::platform {
namespace {

namespace fs = std::filesystem;

class ComTest : public ::testing::Test {
protected:
    void SetUp() override { com_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    void TearDown() override {
        if (SUCCEEDED(com_)) {
            ::CoUninitialize();
        }
    }

private:
    HRESULT com_ = E_FAIL;
};

using ShortcutsTest = ComTest;
using ScheduledTasksTest = ComTest;

TEST_F(ShortcutsTest, ResolvesTargetAndArguments) {
    const fs::path folder = testing::long_temp_directory() /
                            (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-shortcuts");
    fs::create_directories(folder);
    const std::wstring file = (folder / L"Tool.lnk").wstring();
    const std::wstring target = LR"(C:\Avel.Tests\Contoso\tool.exe)";
    {
        wil::com_ptr_nothrow<IShellLinkW> link;
        ASSERT_HRESULT_SUCCEEDED(
            ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())));
        ASSERT_HRESULT_SUCCEEDED(link->SetPath(target.c_str()));
        ASSERT_HRESULT_SUCCEEDED(link->SetArguments(L"--tray \"C:\\Some Folder\""));
        ASSERT_HRESULT_SUCCEEDED(link.try_query<IPersistFile>()->Save(file.c_str(), TRUE));
    }
    const auto shortcut = resolve_shortcut(file);
    ASSERT_TRUE(shortcut);
    EXPECT_TRUE(iequals(shortcut->target, target));
    EXPECT_EQ(shortcut->arguments, L"--tray \"C:\\Some Folder\"");
    EXPECT_FALSE(shortcut->advertised);

    EXPECT_FALSE(resolve_shortcut((folder / L"Missing.lnk").wstring()));
    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

TEST(ScheduledTaskCommandTest, ExecActionsAsCommandLines) {
    EXPECT_EQ(exec_command(LR"(C:\Program Files\App\app.exe)", L"/update"),
              LR"("C:\Program Files\App\app.exe" /update)");
    EXPECT_EQ(exec_command(LR"( "C:\Program Files\App\app.exe" )", L""),
              LR"("C:\Program Files\App\app.exe")");
    EXPECT_EQ(exec_command(LR"(C:\App\app.exe)", L""), LR"(C:\App\app.exe)");
    EXPECT_EQ(exec_command(L"", L""), L"");
}

TEST(ScheduledTaskCommandTest, WindowsTasksAreTheMicrosoftFolders) {
    EXPECT_TRUE(is_windows_task(LR"(\Microsoft)"));
    EXPECT_TRUE(is_windows_task(LR"(\Microsoft\Windows\Defrag\ScheduledDefrag)"));
    EXPECT_TRUE(is_windows_task(LR"(\microsoft\Office\Update)"));
    EXPECT_FALSE(is_windows_task(LR"(\MicrosoftEdgeUpdateTaskMachineCore)"));
    EXPECT_FALSE(is_windows_task(LR"(\Contoso\Microsoft)"));
}

TEST_F(ScheduledTasksTest, ReadsThisMachinesTasks) {
    std::vector<ScheduledTask> all;
    ASSERT_EQ(scheduled_tasks(false, all), S_OK);
    EXPECT_TRUE(
        std::any_of(all.begin(), all.end(), [](const auto& task) { return is_windows_task(task.path); }));

    std::vector<ScheduledTask> others;
    ASSERT_EQ(scheduled_tasks(true, others), S_OK);
    EXPECT_TRUE(std::none_of(others.begin(), others.end(),
                             [](const auto& task) { return is_windows_task(task.path); }));
    EXPECT_LT(others.size(), all.size());

    // One task read by its path matches the listing.
    const auto readable =
        std::find_if(all.begin(), all.end(), [](const auto& task) { return !task.actions.empty(); });
    ASSERT_NE(readable, all.end());
    const auto again = scheduled_task(readable->path);
    ASSERT_TRUE(again);
    EXPECT_EQ(again->actions, readable->actions);
    EXPECT_EQ(again->enabled, readable->enabled);
    EXPECT_FALSE(scheduled_task(LR"(\Avel.Tests\No such task)"));
}

TEST(ServicesTest, ListsThisMachinesWin32Services) {
    std::vector<ServiceInfo> services;
    ASSERT_EQ(win32_services(services), static_cast<DWORD>(ERROR_SUCCESS));
    ASSERT_FALSE(services.empty());
    for (const auto& service : services) {
        EXPECT_FALSE(is_driver_type(service.type)) << to_utf8(service.name);
        EXPECT_NE(service.type & SERVICE_WIN32, 0u) << to_utf8(service.name);
    }
    // svchost's services have their DLL.
    EXPECT_TRUE(std::any_of(services.begin(), services.end(), [](const auto& service) {
        return icontains(service.image_path, L"svchost.exe") && is_absolute_path(service.service_dll) &&
               iends_with(service.service_dll, L".dll");
    }));
    EXPECT_TRUE(std::any_of(services.begin(), services.end(),
                            [](const auto& service) { return service.start_type == SERVICE_AUTO_START; }));
}

TEST(ServicesTest, DriverTypes) {
    EXPECT_TRUE(is_driver_type(SERVICE_KERNEL_DRIVER));
    EXPECT_TRUE(is_driver_type(SERVICE_FILE_SYSTEM_DRIVER));
    EXPECT_FALSE(is_driver_type(SERVICE_WIN32_OWN_PROCESS));
    EXPECT_FALSE(is_driver_type(SERVICE_WIN32_SHARE_PROCESS | SERVICE_INTERACTIVE_PROCESS));
}

}  // namespace
}  // namespace avel::platform
