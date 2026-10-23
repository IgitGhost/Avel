// Records kept as one folder each under a root, the way the ChangeStore keeps ChangeSets
// (ADR 0004) and the TraceStore keeps traces.
#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace avel::platform {

// Times as the records' JSON files hold them: seconds since 1970 to read back, ISO 8601 in UTC
// ("2026-10-23T14:05:09Z") for people.
std::int64_t unix_seconds(std::chrono::system_clock::time_point time);
std::chrono::system_clock::time_point from_unix(std::int64_t seconds);
std::string iso_time(std::chrono::system_clock::time_point time);

// A new record's id, also its folder's name: YYYYMMDD-HHMMSS-<8 hex digits>, the time in UTC and
// a random suffix, so ids sort by creation time.
std::wstring new_record_id(std::chrono::system_clock::time_point now);

// `id` can only name a folder directly below the root: not empty, no '\', '/' or ':', and not "."
// or "..". Ids that come from outside (the command line, the GUI) are checked before use.
bool is_safe_record_id(std::wstring_view id);

// Holds <folder>\lock open, exclusively, for as long as the record is in use. It deletes itself
// when closed, also when the process is killed. Null if another process holds it.
wil::unique_hfile take_folder_lock(const std::wstring& folder);

// The names of the folders directly below `root`, leaving out junctions and links. Unsorted.
std::vector<std::wstring> record_folders(const std::wstring& root);

// %ProgramData%\Avel\<name>. Empty if ProgramData can't be found.
std::wstring program_data_folder(std::wstring_view name);

}  // namespace avel::platform
