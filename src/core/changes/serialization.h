// JSON helpers for the ChangeSet files (internal to avel_core and its tests).
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/changes/registry_backup.h"
#include "core/leftovers/leftover.h"

namespace avel::changes {

using Json = nlohmann::ordered_json;

// Registry and file names are UTF-16 and may hold unpaired surrogates, which UTF-8 can't carry.
// A name that survives the round trip is written as `field`; any other as `field_hex`
// (its UTF-16 code units in hex), so it comes back exactly.
void put_text(Json& json, std::string_view field, std::wstring_view text);
std::optional<std::wstring> get_text(const Json& json, std::string_view field);

std::string to_hex(const std::byte* data, std::size_t size);
std::string to_hex(const std::vector<std::byte>& data);
std::optional<std::vector<std::byte>> from_hex(std::string_view text);

// Writes `text` to `path` and flushes it to disk before returning. With `replace`, writes a
// temporary file next to it first and swaps it in, so readers never see a partial file.
bool write_file_durably(const std::wstring& path, std::string_view text, bool replace = false);
std::optional<std::string> read_file(const std::wstring& path);

// Locations, with registry addresses kept as (hive, sid, path) rather than display text.
Json to_json(const leftovers::Location& location);
bool from_json(const Json& json, leftovers::Location& location);

// Registry snapshots. from_json returns false for malformed input.
Json to_json(const ValueSnapshot& value);
Json to_json(const KeySnapshot& key);
bool from_json(const Json& json, ValueSnapshot& value);
bool from_json(const Json& json, KeySnapshot& key);

}  // namespace avel::changes
