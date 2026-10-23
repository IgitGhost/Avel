#include "core/tracker/trace.h"

namespace avel::tracker {

std::string_view to_string(ChangeKind change) {
    switch (change) {
        case ChangeKind::Created:
            return "created";
        case ChangeKind::Deleted:
            return "deleted";
        case ChangeKind::Modified:
            return "modified";
        case ChangeKind::Renamed:
            return "renamed";
    }
    return "unknown";
}

std::string_view to_string(NoiseReason reason) {
    switch (reason) {
        case NoiseReason::None:
            return "none";
        case NoiseReason::SystemActivity:
            return "system-activity";
        case NoiseReason::UserActivity:
            return "user-activity";
        case NoiseReason::Temporary:
            return "temporary";
        case NoiseReason::Avel:
            return "avel";
        case NoiseReason::Unresolved:
            return "unresolved";
    }
    return "unknown";
}

std::string_view to_string(KnownSpot spot) {
    switch (spot) {
        case KnownSpot::MachineEnvironment:
            return "machine-environment";
        case KnownSpot::UserEnvironment:
            return "user-environment";
        case KnownSpot::SharedDlls:
            return "shared-dlls";
    }
    return "unknown";
}

}  // namespace avel::tracker
