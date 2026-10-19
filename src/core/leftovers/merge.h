#pragma once

#include <vector>

#include "core/leftovers/leftover.h"

namespace avel::leftovers {

// Combines items found more than once (the same identity, possibly by different scanners or for
// different programs): their evidence is merged, each piece counted once, and their owners joined.
void merge_duplicates(std::vector<Leftover>& leftovers);

// Sorts the items by category and location (as a tree: each folder or key directly followed by
// what is inside it), then points each one at the nearest other item that
// contains it (Leftover::inside), so removal can skip items their selected container takes along.
void link_containment(std::vector<Leftover>& leftovers);

}  // namespace avel::leftovers
