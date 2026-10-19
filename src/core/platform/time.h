#pragma once

#include <windows.h>

#include <chrono>
#include <optional>

namespace avel::platform {

// The calendar date of a UTC file time in the computer's time zone: the date Settings shows.
std::optional<std::chrono::sys_days> local_date(const FILETIME& time);

}  // namespace avel::platform
