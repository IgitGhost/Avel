#include "core/platform/text.h"

#include <windows.h>

#include <limits>
#include <stdexcept>
#include <system_error>

namespace avel::platform {

namespace {

int checked_length(size_t size) {
    if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("text conversion: input is too large");
    }
    return static_cast<int>(size);
}

[[noreturn]] void throw_last_error(const char* what) {
    throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), what);
}

}  // namespace

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    // Flags must stay 0: WC_ERR_INVALID_CHARS would reject unpaired surrogates instead of
    // replacing them.
    const int input_length = checked_length(text.size());
    const int output_length =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), input_length, nullptr, 0, nullptr, nullptr);
    if (output_length == 0) {
        throw_last_error("WideCharToMultiByte");
    }
    std::string result(static_cast<size_t>(output_length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), input_length, result.data(), output_length, nullptr,
                          nullptr);
    return result;
}

std::wstring to_utf16(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int input_length = checked_length(text.size());
    const int output_length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), input_length, nullptr, 0);
    if (output_length == 0) {
        throw_last_error("MultiByteToWideChar");
    }
    std::wstring result(static_cast<size_t>(output_length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), input_length, result.data(), output_length);
    return result;
}

bool iequals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return ::CompareStringOrdinal(a.data(), checked_length(a.size()), b.data(), checked_length(b.size()),
                                  TRUE) == CSTR_EQUAL;
}

bool icontains(std::wstring_view text, std::wstring_view part) {
    if (part.empty()) {
        return true;
    }
    if (part.size() > text.size()) {
        return false;
    }
    return ::FindStringOrdinal(FIND_FROMSTART, text.data(), checked_length(text.size()), part.data(),
                               checked_length(part.size()), TRUE) >= 0;
}

bool istarts_with(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

bool iends_with(std::wstring_view text, std::wstring_view suffix) {
    return text.size() >= suffix.size() && iequals(text.substr(text.size() - suffix.size()), suffix);
}

std::wstring_view trim(std::wstring_view text) {
    constexpr std::wstring_view kWhitespace = L" \t\r\n\v\f 　";
    auto is_space = [&](wchar_t c) { return c == L'\0' || kWhitespace.find(c) != std::wstring_view::npos; };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace avel::platform
