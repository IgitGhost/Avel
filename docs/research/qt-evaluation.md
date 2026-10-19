# Qt evaluation for a Windows system utility (October 2026)

Question: is Qt a good fit for Avel, a Windows-only uninstaller with registry and file
operations, elevation and install monitoring? Facts were checked against primary sources where
possible; unverified items are marked.

## Versions and Windows UI
- **Qt 6.12.0 (LTS) released on 2026-09-30.** The cadence is two minor releases a year, and every fourth minor is LTS (6.8, then 6.12). For open-source users, LTS patch releases stop after the first few.
- **The `windows11` style** has been the default Widgets style on Windows 11 since 6.7: dark mode support, high contrast since 6.10. It is drawn by Qt rather than native WinUI, so it's usable but not pixel-perfect Fluent. Fusion is the fallback.
- **Dark mode** follows the system color scheme (6.5+); 6.8 added an explicit override.
- **High DPI:** Per-Monitor DPI Aware V2 by default.
- **New in 6.12:** `qt_add_executable` generates the Windows manifest. The `QT_WINDOWS_APP_PROJECT_EXECUTION_LEVEL` target property sets `requireAdministrator`, and the manifest enables `longPathAware`. Verified in `lib/cmake/Qt6/QtPublicWindowsHelpers.cmake`.

## Licensing
- qtbase (Core, Gui, Widgets) is available under **LGPLv3** or GPLv2. GPLv3-only modules include Qt Charts and Qt Graphs; Avel avoids them.
- **Dynamic linking** satisfies the LGPL's relinking requirement with no extra work. Static linking is also acceptable for an open-source project, because anyone can rebuild it from source.
- **Obligations when distributing binaries:** include the LGPL text, make Qt's source available, and keep the license notice visible (Help → About Qt).
- The official online installer requires a Qt account. `aqtinstall` installs headlessly, which is what CI uses through `jurplel/install-qt-action`.

## Deployment size
Measured from a shipped Qt 6.10 app on the dev machine:
- Qt6Core 10.1 MB, Qt6Gui 9.5 MB, Qt6Widgets 6.6 MB, platform plugin 1.0 MB.
- A lean `windeployqt --no-opengl-sw` folder is about 30 MB, and about 55 MB with the defaults.
- A static build is estimated at 10–20 MB (unverified for Qt 6).

## Widgets vs Qt Quick
Qt's own guidance is to use Widgets for complex desktop applications. `QTableView`/`QTreeView` with
`QSortFilterProxyModel` handle large, sortable data sets, and 6.9 reduced header memory for large
models. QML views would need headers, sorting and selection built by hand. **Decision: Widgets.**

## Interop with Win32 and C++/WinRT
- Plain Win32 and COM (registry, msi.dll, Restart Manager, SRSetRestorePoint, Task Scheduler, WMI) have no known conflicts. Qt sets `UNICODE`, `/permissive-`, `/Zc:__cplusplus` and `/utf-8`, all compatible with C++/WinRT.
- **COM apartment:** Qt makes the GUI thread an STA (single-threaded apartment), but `winrt::init_apartment()` defaults to MTA (multithreaded). Calling it on the GUI thread fails with `RPC_E_CHANGED_MODE`. Run PackageManager and WMI work on MTA worker threads, and never block on `.get()` in the GUI thread.
- `QT_NO_KEYWORDS` avoids clashes between `emit`/`signals`/`slots` and third-party headers.
- An elevated window can't receive drag-and-drop from non-elevated Explorer (Windows integrity levels). Use a crosshair target and file pickers instead.

## Alternatives considered

| Option | Win32 access | Windows 11 look | Size / startup | Dev speed |
|---|---|---|---|---|
| **Qt 6 Widgets (C++)** | Native | Good (windows11 style) | ~30 MB, fast | Medium |
| .NET WinForms (BCU) | P/Invoke, CsWin32 | Dated | Small; Native AOT unofficial | Fastest |
| WPF | P/Invoke | Fluent theme (.NET 9+) | No AOT, slower cold start | Fast |
| WinUI 3 (Windows App SDK) | P/Invoke + native WinRT | Best (true Fluent) | AOT; runtime dependency | Medium, tooling friction |
| Avalonia 12 | P/Invoke | Good Fluent theme, not native | AOT works | Fast |
| Rust (Slint / egui + windows-rs) | Complete but verbose | Slint decent, egui non-native | Small, fast | Slow; weak data grids |
| Win32 / WTL | Native | Classic controls; dark mode by hand | Smallest | Slowest |

**Conclusion:** C++20 with Qt 6.12 Widgets on MSVC fits Avel's priorities: fast runtime,
direct Win32 access and dense data views. The decision is recorded in [ADR 0001](../adr/0001-cpp20-qt6-widgets.md).
