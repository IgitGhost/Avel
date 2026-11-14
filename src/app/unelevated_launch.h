#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace avel::app {

// Starting a program as the desktop's user, not elevated. avel.exe runs elevated, and whatever it
// starts with CreateProcess or ShellExecute is elevated too; a browser must never be. So the desktop's
// shell, Explorer, is asked to start it: the desktop's folder view (IShellWindows, SWC_DESKTOP) gives
// its automation object, whose Shell.Application (IShellDispatch2) runs ShellExecute inside Explorer,
// with Explorer's token and environment.

// The SID of the user the desktop's shell (the process of GetShellWindow) runs as; empty when there
// is no shell or its token can't be read. A program started by launchUnelevated runs as this user.
std::wstring shellUserSid();

// Asks the desktop's Explorer to start `program` with `arguments` (one command line). S_OK once
// Explorer has taken the request: whether the program started is Explorer's to tell (it shows its own
// error). Fails, starting nothing, when there is no desktop shell to ask. Call on a thread where COM
// may be used (the GUI thread); it is initialized for the call when it isn't.
HRESULT launchUnelevated(const std::wstring& program, const std::wstring& arguments);

// The arguments as one command line, each quoted when needed the way CommandLineToArgvW and the C
// runtime read them back.
std::wstring joinArguments(const std::vector<std::wstring>& arguments);

}  // namespace avel::app
