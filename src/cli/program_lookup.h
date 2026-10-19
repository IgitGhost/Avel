#pragma once

#include <string>
#include <vector>

#include "core/inventory/inventory.h"

namespace avel::cli {

// The program's name for messages, or its key name or id when it has none.
std::string name_of(const inventory::Program& program);

// Finds programs by id, then by exact display name (both ignoring case). Reports problems and
// returns false if any query doesn't identify exactly one program.
bool resolve(const inventory::Inventory& inventory, const std::vector<std::wstring>& queries,
             std::vector<const inventory::Program*>& found);

}  // namespace avel::cli
