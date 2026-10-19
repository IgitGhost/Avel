#pragma once

#include <string_view>

namespace avel::cli {

// Write UTF-8 text. A real console gets UTF-16 through WriteConsoleW, so Armenian and Cyrillic
// render regardless of the console code page; pipes and files get the UTF-8 bytes unchanged.
void print(std::string_view text);
void print_error(std::string_view text);

// Width of the console window in columns, or 0 when output is redirected.
int console_width();

}  // namespace avel::cli
