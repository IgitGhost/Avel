# ADR 0001: C++20 + Qt 6 Widgets, with a Qt-free engine

- **Status:** Accepted
- **Date:** 2026-10-18

## Context
Avel needs:
- deep, low-overhead access to Win32: the registry, MSI, Restart Manager, Task Scheduler COM, the USN journal, DismAPI, and C++/WinRT for AppX;
- a fast-starting, responsive UI that shows large, sortable tables;
- three UI languages (English, Russian, Armenian).

Runtime speed is a stated priority. See [the Qt evaluation](../research/qt-evaluation.md) for the facts behind this decision.

## Decision
- **Language and toolchain:** C++20, MSVC, CMake (presets) + Ninja, and vcpkg in manifest mode with the `x64-windows-static-md` triplet.
- **GUI:** Qt 6.12 LTS **Widgets**, not Qt Quick. Linked dynamically (LGPLv3), with no GPL-only modules.
- **`avel_core` is a static library with no Qt dependency.** It returns data and codes, never user-facing text, so translation lives entirely in the GUI. The CLI (`avel-cli`) links only the core.
- **The GUI runs elevated** (`requireAdministrator` through Qt's generated manifest). The CLI runs `asInvoker` and checks elevation per command.
- **Apartment rule:** WinRT and WMI calls run on MTA worker threads, never on the Qt GUI thread (an STA).

## Consequences
- The fastest cold start and lowest memory of the options considered, and direct API access without a P/Invoke layer.
- Slower to develop than C#/.NET. Mitigation: WIL for RAII, and a disciplined core/UI split.
- The engine can be unit-tested with no GUI, and is reused by the CLI and the Sandbox scenarios.
- Release builds ship the Qt DLLs (about 30 MB) plus the LGPL text.
