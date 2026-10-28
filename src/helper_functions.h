#pragma once

// The function each helper (helper_abi.h's kHelpers) is, as a loader
// registers it under the helper's name: its C function, or where it reaches
// host code and the helpers catch (kHelpersCatch), the wrapper that catches
// (host_bridge.h).

#include <utility>
#include <vector>

namespace bcore {

namespace bcore_abi {

// {name, function} for every helper, in kHelpers' order.
std::vector<std::pair<const char*, void*>> helper_functions();

}  // namespace bcore_abi

}  // namespace bcore
