// Ported from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin Szeniak, Apache License 2.0.
// See related_entries.h for the original source files.
#include "core/inventory/related_entries.h"

#include <optional>
#include <string_view>

#include "core/inventory/name_matching.h"
#include "core/platform/text.h"

namespace avel::inventory {

namespace {

// BCU PathTools.PathsEqual: ignore surrounding separators, quotes and spaces, and case.
std::wstring_view trim_path_characters(std::wstring_view path) {
    constexpr std::wstring_view kTrim = L"\\/\"  ";
    while (!path.empty() && kTrim.find(path.front()) != std::wstring_view::npos) {
        path.remove_prefix(1);
    }
    while (!path.empty() && kTrim.find(path.back()) != std::wstring_view::npos) {
        path.remove_suffix(1);
    }
    return path;
}

bool paths_equal(std::wstring_view a, std::wstring_view b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    return platform::iequals(trim_path_characters(a), trim_path_characters(b));
}

// Path.GetFileName: the text after the last separator.
std::wstring_view last_path_component(std::wstring_view path) {
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? path : path.substr(separator + 1);
}

void add_score(int& score, int fail_score, int unsure_score, int success_score, std::optional<bool> result) {
    if (!result.has_value()) {
        score += unsure_score;
    } else {
        score += *result ? success_score : fail_score;
    }
}

std::optional<bool> is_64_bit(Architecture architecture) {
    switch (architecture) {
        case Architecture::X86:
        case Architecture::Arm:
            return false;
        case Architecture::X64:
        case Architecture::Arm64:
            return true;
        default:
            return std::nullopt;
    }
}

std::optional<bool> same_bitness(const Program& a, const Program& b) {
    const auto a64 = is_64_bit(a.architecture);
    const auto b64 = is_64_bit(b.architecture);
    if (!a64 || !b64) {
        return std::nullopt;
    }
    return *a64 == *b64;
}

// BCU compares timestamps (true within an hour, unsure otherwise). Avel only knows the install
// day, which BCU treats as "lacking a time part": equal days match, anything else is unsure.
std::optional<bool> compare_dates(const Program& a, const Program& b) {
    if (!a.install_date || !b.install_date || *a.install_date != *b.install_date) {
        return std::nullopt;
    }
    return true;
}

}  // namespace

int relatedness_score(const Program& base, const Program& other) {
    if (paths_equal(base.install_location, other.install_location)) {
        return kCertainlyRelated;
    }
    if (!base.uninstall_string.empty() && paths_equal(base.uninstall_string, other.uninstall_string)) {
        return kCertainlyRelated;
    }

    int score = 0;

    if (!base.install_location.empty() && !other.install_location.empty()) {
        // Equal folders already returned above; nesting is slightly suspicious, unrelated worse.
        add_score(score, -8, 0, -3, platform::icontains(base.install_location, other.install_location));
    }

    add_score(score, -5, 0, 3, same_bitness(base, other));
    add_score(score, -3, -1, 5, compare_dates(base, other));
    add_score(score, -2, 0, 5, compare_names(base.display_version, other.display_version, true));
    add_score(score, -5, 0, 5, compare_names(base.publisher, other.publisher));

    // Was `base` installed from inside `other`'s install folder?
    if (base.install_location.empty() && !base.install_source.empty() && other.install_location.size() >= 5) {
        add_score(score, 0, 0, 5, platform::icontains(base.install_source, other.install_location));
    }

    const std::wstring base_trimmed = strip_version_number(base.display_name);
    const std::wstring other_trimmed = strip_version_number(other.display_name);

    const auto name_similarity = compare_names(base.display_name, other.display_name);
    add_score(score, -5, -2, 8, name_similarity);
    if (!name_similarity.value_or(false)) {
        add_score(score, -5, -2, 8, compare_names(base_trimmed, other_trimmed));
    }

    const std::wstring_view base_name_for_folder =
        base_trimmed.size() < 5 ? std::wstring_view(base.display_name) : std::wstring_view(base_trimmed);
    add_score(score, -2, -2, 5,
              compare_names(base_name_for_folder, last_path_component(other.install_location)));

    return score;
}

}  // namespace avel::inventory
