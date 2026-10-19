// Ported from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin Szeniak, Apache License 2.0.
// See product_name.h for the original source files.
#include "core/leftovers/product_name.h"

#include <algorithm>

#include "core/inventory/name_matching.h"
#include "core/platform/text.h"

namespace avel::leftovers {

namespace {

void replace_all(std::wstring& text, std::wstring_view what, std::wstring_view with) {
    if (what.empty()) {
        return;
    }
    for (size_t at = text.find(what); at != std::wstring::npos; at = text.find(what, at + with.size())) {
        text.replace(at, what.size(), with);
    }
}

bool contains(std::wstring_view text, std::wstring_view part) {
    return text.find(part) != std::wstring_view::npos;
}

}  // namespace

ProductNames make_product_names(std::wstring_view display_name, std::wstring_view publisher) {
    return {inventory::strip_version_number(display_name), inventory::trim_publisher(publisher)};
}

int match_product_name(const ProductNames& names, std::wstring_view item) {
    const std::wstring product = platform::to_lower(names.name);
    std::wstring text(item);
    std::replace(text.begin(), text.end(), L'_', L' ');
    text = std::wstring(platform::trim(platform::to_lower(text)));
    const size_t lowest_length = std::min(product.size(), text.size());

    // Don't match short strings.
    if (lowest_length <= 4) {
        return -1;
    }

    const int result = inventory::sift4_distance(product, text, 1);
    if (result <= 1) {
        return result;
    }

    // If the product name contains the publisher's, try again without it.
    const std::wstring publisher = platform::to_lower(names.publisher);
    if (publisher.size() > 4 && contains(product, publisher)) {
        std::wstring trimmed = product;
        replace_all(trimmed, publisher, L"");
        trimmed = std::wstring(platform::trim(trimmed));
        if (trimmed.size() <= 4) {
            return -1;
        }
        const int trimmed_result = inventory::sift4_distance(trimmed, text, 1);
        if (trimmed_result <= 1) {
            return trimmed_result;
        }
    }

    if (contains(text, product) || contains(product, text)) {
        return 2;
    }

    // Hard cut-off if the difference is more than a third of the checked name.
    if (result < static_cast<int>(lowest_length / 3)) {
        return result;
    }
    return -1;
}

bool item_name_equals_company_name(const ProductNames& names, std::wstring_view item) {
    const std::wstring publisher = platform::to_lower(names.publisher);
    return publisher != platform::to_lower(names.name) && contains(publisher, platform::to_lower(item));
}

std::vector<Record> name_confidence(const ProductNames& names, std::wstring_view item, std::wstring_view parent_name,
                                    int level, bool parent_is_known_folder) {
    std::vector<Record> records;
    const int match = match_product_name(names, item);
    if (match < 0) {
        return records;
    }
    records.push_back({match < 2 ? Evidence::ProductNamePerfectMatch : Evidence::ProductNameDodgyMatch});
    records.push_back({Evidence::Depth, static_cast<std::int8_t>(level)});
    if (item_name_equals_company_name(names, item)) {
        records.push_back({Evidence::ItemNameEqualsCompanyName});
    }
    if (level > 0) {
        std::wstring parent(parent_name);
        std::replace(parent.begin(), parent.end(), L'_', L' ');
        if (contains(platform::to_lower(names.publisher), platform::to_lower(parent))) {
            records.push_back({Evidence::CompanyNameMatch});
        }
    }
    if (parent_is_known_folder) {
        records.push_back({Evidence::DirectlyInsideKnownFolder});
    }
    return records;
}

std::wstring_view name_without_extension(std::wstring_view name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring_view::npos ? name : name.substr(0, dot);
}

}  // namespace avel::leftovers
