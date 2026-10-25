// The noise filter: sets aside what Windows, the user or Avel changed while a trace ran, so that
// the review shows what the installer did (ADR 0002). Noise stays in the trace with its reason,
// but is never offered for removal.
//
// The rules are data: folders, files, keys and values named by patterns. A pattern is a lowercase
// path whose components may hold '*' (any run of characters) and '?' (any one character); it
// matches the item it names and everything below it. default_noise_rules fills the tables for this
// machine's folders and users; the idle round in Windows Sandbox tunes them.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/platform/registry_address.h"
#include "core/tracker/trace.h"

namespace avel::leftovers {
struct ScanEnvironment;
}

namespace avel::tracker {

struct PathRule {
    std::wstring pattern;  // e.g. c:\users\me\appdata\local\packages\*\ac
    NoiseReason reason = NoiseReason::SystemActivity;
};

struct KeyRule {
    platform::Hive hive = platform::Hive::Machine;
    std::wstring sid;      // Hive::User: the user it applies to; empty for every user
    std::wstring pattern;  // Below the hive root, e.g. software\microsoft\tracing
    NoiseReason reason = NoiseReason::SystemActivity;
};

struct ValueRule {
    KeyRule key;        // The key holding the values (that key only, not its subkeys) and the reason
    std::wstring name;  // e.g. localprofileloadtime*
};

// The first rule that matches gives the reason.
struct NoiseRules {
    std::vector<PathRule> files;
    std::vector<KeyRule> keys;
    std::vector<ValueRule> values;
    // Keys that are never noise, with their subkeys and values, whatever else matches them: the
    // ones a program's removal relies on (Explorer's MuiCache, StartupApproved, App Paths).
    std::vector<KeyRule> never;
};

// The default rules for `environment`'s Windows folder, ProgramData, drives and users.
// `avel_paths` are Avel's own folders (its data folders and its install folder); they and
// Avel's keys count as NoiseReason::Avel.
NoiseRules default_noise_rules(const leftovers::ScanEnvironment& environment,
                               const std::vector<std::wstring>& avel_paths);

// `path` is normalized, as FileLocation holds it.
NoiseReason classify_file(const NoiseRules& rules, std::wstring_view path);

// Machine keys may be named through any ControlSet00n as well as CurrentControlSet, and a user's
// classes through "<sid>_Classes" as well as "Software\Classes".
NoiseReason classify_key(const NoiseRules& rules, const platform::RegistryAddress& key);

// A value rule for it, or else the rules for its key.
NoiseReason classify_value(const NoiseRules& rules, const platform::RegistryAddress& key,
                           std::wstring_view value_name);

// Sets TraceChange::noise on every change whose noise is None, by its location: files, keys and
// values. Services, tasks, firewall rules and packages are never noise; Unresolved stays.
void apply_noise(const NoiseRules& rules, std::vector<TraceChange>& changes);

}  // namespace avel::tracker
