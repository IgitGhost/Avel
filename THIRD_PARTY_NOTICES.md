# Third-party notices

Avel uses the following third-party components. Each is the property of its respective authors
and is used under the license named here.

| Component | Used for | License | Source |
|---|---|---|---|
| Qt 6 (Core, Gui, Widgets, Svg, and the platform, style, icon-engine and image-format plugins) | GUI (`avel.exe`) | LGPL-3.0 | https://download.qt.io/official_releases/qt/ |
| Windows Implementation Library (WIL) | RAII wrappers for Win32 handles, registry keys, COM | MIT | https://github.com/microsoft/wil |
| JSON for Modern C++ (nlohmann/json) | JSON output of `avel-cli` | MIT | https://github.com/nlohmann/json |
| GoogleTest | Unit tests only (not shipped) | BSD-3-Clause | https://github.com/google/googletest |

Avel also uses libraries that are part of Windows and its SDK (C++/WinRT headers, XmlLite,
Windows Installer, the Shell API). They are not redistributed.

## Qt and the LGPL

Avel links to Qt dynamically: the Qt DLLs shipped next to `avel.exe` can be replaced with a
compatible build of Qt. Qt's source code is available from the link above, and the full LGPL-3.0
text ships with every release in `licenses/LGPL-3.0.txt`. Qt's version and license information
can also be viewed from the GUI under **Help → About Qt**.

This table is updated whenever a dependency is added to `vcpkg.json` or `src/app/CMakeLists.txt`.
