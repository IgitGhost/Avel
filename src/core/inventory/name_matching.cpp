// Ported from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin Szeniak, Apache License 2.0.
// See name_matching.h for the original source files.
#include "core/inventory/name_matching.h"

#include <algorithm>
#include <array>

#include "core/platform/text.h"

namespace avel::inventory {

namespace {

// String.Trim() followed by Trim(',', '.', ' ').
std::wstring_view trim_with_punctuation(std::wstring_view text) {
    text = platform::trim(text);
    while (!text.empty() && (text.front() == L',' || text.front() == L'.' || text.front() == L' ')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == L',' || text.back() == L'.' || text.back() == L' ')) {
        text.remove_suffix(1);
    }
    return text;
}

std::wstring_view trim_end(std::wstring_view text) {
    while (!text.empty() && platform::trim(text.substr(text.size() - 1)).empty()) {
        text.remove_suffix(1);
    }
    return text;
}

// Repeatedly removes any of `trimmers` from the end (case-insensitive), re-trimming whitespace and
// punctuation after every cut. Returns an empty string if a trimmer would consume everything.
template <size_t N>
std::wstring_view extended_trim_end_any(std::wstring_view text,
                                        const std::array<std::wstring_view, N>& trimmers) {
    if (text.empty()) {
        return {};
    }
    text = trim_with_punctuation(text);
    bool rerun = true;
    while (rerun) {
        rerun = false;
        for (const auto trimmer : trimmers) {
            if (!platform::iends_with(text, trimmer)) {
                continue;
            }
            if (text.size() <= trimmer.size()) {
                return {};
            }
            text = trim_with_punctuation(text.substr(0, text.size() - trimmer.size()));
            rerun = true;
            break;
        }
    }
    return text;
}

constexpr std::array<std::wstring_view, 14> kVersionCharacters = {L"1", L"2", L"3", L"4", L"5", L"6", L"7",
                                                                  L"8", L"9", L"0", L".", L",", L"-", L"_"};
constexpr std::array<std::wstring_view, 4> kNoiseSuffixes = {L"Application", L"Helper", L" v", L" CE"};

}  // namespace

int sift4_distance(std::wstring_view s1, std::wstring_view s2, int max_offset) {
    const int l1 = static_cast<int>(s1.size());
    const int l2 = static_cast<int>(s2.size());
    if (l1 == 0) {
        return l2;
    }
    if (l2 == 0) {
        return l1;
    }

    int c1 = 0;        // cursor for string 1
    int c2 = 0;        // cursor for string 2
    int lcss = 0;      // largest common subsequence
    int local_cs = 0;  // local common substring
    while (c1 < l1 && c2 < l2) {
        if (s1[c1] == s2[c2]) {
            ++local_cs;
        } else {
            lcss += local_cs;
            local_cs = 0;
            if (c1 != c2) {
                c1 = c2 = std::max(c1, c2);
            }
            // On a match, step both cursors back by one; they're incremented below, so a single
            // code path handles matches.
            for (int i = 0; i < max_offset && (c1 + i < l1 && c2 + i < l2); ++i) {
                if (c1 + i < l1 && s1[c1 + i] == s2[c2]) {
                    c1 += i - 1;
                    --c2;
                    break;
                }
                if (c2 + i < l2 && s1[c1] == s2[c2 + i]) {
                    --c1;
                    c2 += i - 1;
                    break;
                }
            }
        }
        ++c1;
        ++c2;
    }
    lcss += local_cs;
    return std::max(l1, l2) - lcss;
}

std::wstring strip_version_number(std::wstring_view name) {
    std::wstring_view input = name;
    size_t previous_length = 0;
    do {
        previous_length = input.size();

        input = platform::trim(input);
        if (input.empty()) {
            return {};
        }

        if (input.back() == L')') {
            const auto bracket = input.find_last_of(L'(');
            if (bracket != std::wstring_view::npos && bracket > 4) {
                input = trim_end(input.substr(0, bracket));
            }
        }

        input = trim_end(extended_trim_end_any(input, kVersionCharacters));

        if (platform::iends_with(input, L" v")) {
            input = input.substr(0, input.size() - 2);
        }

        input = trim_end(extended_trim_end_any(input, kNoiseSuffixes));
    } while (previous_length != input.size());

    return std::wstring(input);
}

std::optional<bool> compare_names(std::wstring_view a, std::wstring_view b, bool relaxed) {
    const size_t minimum_length = relaxed ? 4 : 5;
    if (a.size() < minimum_length || b.size() < minimum_length) {
        return std::nullopt;
    }
    if (relaxed && (a.starts_with(b) || b.starts_with(a))) {
        return true;
    }
    const int changes = sift4_distance(a, b, 3);
    return changes == 0 || changes < static_cast<int>(a.size() / 6);
}

}  // namespace avel::inventory
