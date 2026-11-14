#include "registry_editor.h"

#include <windows.h>
#include <shellapi.h>

#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

QString regeditPath(const platform::RegistryAddress& key) {
    if (key.hive == platform::Hive::Machine) {
        return u"HKEY_LOCAL_MACHINE\\"_s + toQString(key.path);
    }
    if (platform::iequals(key.sid, platform::current_user_sid())) {
        return u"HKEY_CURRENT_USER\\"_s + toQString(key.path);
    }
    HKEY hive = nullptr;
    if (::RegOpenKeyExW(HKEY_USERS, key.sid.c_str(), 0, KEY_READ, &hive) != ERROR_SUCCESS) {
        return {};
    }
    ::RegCloseKey(hive);
    return u"HKEY_USERS\\"_s + toQString(key.sid) + u'\\' + toQString(key.path);
}

// Registry Editor writes LastKey under its tree's root, whose name ("Computer") is localized; the
// name it wrote last is reused.
void openRegistryEditor(const QString& path) {
    constexpr wchar_t kRegeditKey[] = LR"(Software\Microsoft\Windows\CurrentVersion\Applets\Regedit)";
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRegeditKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                          nullptr, &key, nullptr) == ERROR_SUCCESS) {
        QString root;
        wchar_t last[1024] = {};
        DWORD size = sizeof(last) - sizeof(wchar_t);
        if (::RegGetValueW(key, nullptr, L"LastKey", RRF_RT_REG_SZ, nullptr, last, &size) == ERROR_SUCCESS) {
            const QString previous = QString::fromWCharArray(last);
            const QString first = previous.section(u'\\', 0, 0);
            if (!first.isEmpty() && !first.startsWith(u"HKEY_"_s, Qt::CaseInsensitive)) {
                root = first + u'\\';
            }
        }
        const std::wstring value = (root + path).toStdWString();
        ::RegSetValueExW(key, L"LastKey", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        ::RegCloseKey(key);
    }
    // -m: a new window even when Registry Editor is open already, so it reads LastKey.
    ::ShellExecuteW(nullptr, L"open", L"regedit.exe", L"-m", nullptr, SW_SHOWNORMAL);
}

}  // namespace avel::app
