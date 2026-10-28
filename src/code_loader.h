#pragma once

// What makes a block's object file (Compiler::emit) a function this
// process runs: one CodeLoader per build, for the host it is built for.
// Every one has
//
//   bool init();
//   // The function `name` of pc's block in object (bb_0x<pc>, or a
//   // variant such as bb_0x<pc>.careful: BBTranslator::translate), or
//   // null, said why on stderr.
//   BbFunc load(uint32_t pc, std::unique_ptr<llvm::MemoryBuffer> object, const std::string& name);
//   // fn is no block's any more: JitEngine replaced it, between runs (Core
//   // replaces no block while one runs). fn may be code the loader did not
//   // load (JitEngine::addCode's), which it leaves alone.
//   void release(BbFunc fn);
//   // Runs fn on cpu and mem.
//   void enter(BbFunc fn, CpuState* cpu, Memory* mem) const;
//
// and what the rest of the engine needs of it alone besides (the native
// loader's code arenas, trampolines and listeners). JitEngine keeps what is
// the same for every one: which block each PC has, and the tables generated
// code reads.

#include "cpu_state.h"
#include "mem.h"

namespace bcore {

// A compiled block: void bb_0x<pc>(CpuState*, Memory*) in the blocks'
// calling convention (block_abi.h), in which a block that runs the next one
// itself (a musttail call, see CpuState::chain_table) is always a tail
// call. C++ cannot call one directly: CodeLoader::enter() does.
using BbFunc = void (*)(CpuState*, Memory*);

}  // namespace bcore

#if defined(__EMSCRIPTEN__)
#include "code_loader_wasm.h"
#else
#include "code_loader_native.h"
#endif
