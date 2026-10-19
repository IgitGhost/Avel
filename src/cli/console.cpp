#include "cli/console.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "core/platform/text.h"

namespace avel::cli {

namespace {

void write(FILE* stream, DWORD std_handle_id, std::string_view text) {
    HANDLE handle = ::GetStdHandle(std_handle_id);
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && ::GetConsoleMode(handle, &mode)) {
        const std::wstring wide = platform::to_utf16(text);
        DWORD written = 0;
        ::WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        return;
    }
    std::fwrite(text.data(), 1, text.size(), stream);
}

}  // namespace

void print(std::string_view text) { write(stdout, STD_OUTPUT_HANDLE, text); }

void print_error(std::string_view text) { write(stderr, STD_ERROR_HANDLE, text); }

int console_width() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!::GetConsoleScreenBufferInfo(::GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return 0;
    }
    return info.srWindow.Right - info.srWindow.Left + 1;
}

}  // namespace avel::cli
