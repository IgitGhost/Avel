#pragma once

#include <nlohmann/json.hpp>

#include <memory>

#include "core/changes/executor.h"
#include "core/changes/store.h"

namespace avel::cli {

// Opens %ProgramData%\Avel\Changes (administrators only), then settles sets an earlier run left
// unfinished and purges sets older than 30 days. Prints why and returns null if it can't.
std::unique_ptr<changes::ChangeStore> open_change_store();

// Options for running or undoing sets from the CLI. AVEL_TEST_FAULT_AFTER=<n> in the environment
// ends the process after n journal records, as a crash would (for the Sandbox's recovery test).
changes::ExecuteOptions cli_execute_options(bool allow_irreversible);

nlohmann::ordered_json item_status_to_json(const changes::ItemStatus& status);
nlohmann::ordered_json execute_result_to_json(const changes::ChangePlan& plan, const changes::ExecuteResult& result);
void print_execute_result(const changes::ChangePlan& plan, const changes::ExecuteResult& result);
nlohmann::ordered_json summary_to_json(const changes::SetSummary& summary);

}  // namespace avel::cli
