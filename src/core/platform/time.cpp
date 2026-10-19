#include "core/platform/time.h"

namespace avel::platform {

std::optional<std::chrono::sys_days> local_date(const FILETIME& time) {
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!::FileTimeToSystemTime(&time, &utc) || !::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{local.wYear}, std::chrono::month{local.wMonth},
                                           std::chrono::day{local.wDay}};
    return date.ok() ? std::optional(std::chrono::sys_days{date}) : std::nullopt;
}

}  // namespace avel::platform
