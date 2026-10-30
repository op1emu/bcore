#pragma once

// WebAssembly's cold blocks (Core::set_cold_threshold): a block's first runs
// make no module of its own; its pieces (cp_wasm.h) run one at a time, as
// functions of their stencils that every block shares.
//
// A stencil's piece function is the piece made a function (cpu, mem) -> i32:
// its code inside a block, then 0 (the piece went on to the next one), and
// after that block 1 (it left for the exit). Its value holes read their
// values instead, from where CodeLoader::run_pieces points
// (piece_values_address): the k-th relocation's at 4 * k. A load's or
// store's offset hole becomes an add to its address (an access that would
// have trapped past 4 GiB wraps instead). The locals are the block
// function's (kWasmLocals), then three more: the values' pointer and an i32
// and an i64 that hold a store's value while its address is moved.
//
// The functions are made when a block first needs them: those of the block
// that no block needed before, as a module (load_shared_functions). A stencil
// that returns or calls its block (an exit, a guard: Config::careful, no
// chaining and no hint have none) has none.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bcore::bcore_cp {

// Stencil id's piece function as a function body (its locals first); false
// if it has none.
bool piece_function(uint32_t id, std::vector<uint8_t>& body);

// The slots of the piece functions of the n stencils ids, made where they
// are not yet; false if one has none or a module fails.
bool piece_slots(const uint32_t* ids, size_t n, uint32_t* slots);

}  // namespace bcore::bcore_cp
