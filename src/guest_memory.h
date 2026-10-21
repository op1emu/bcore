#pragma once

#include <cstdint>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>

// Guest memory in loop blocks, said abstractly until the loops around it are
// known.
//
// A loop block (BBTranslator::set_loop_blocks) reaches guest RAM through two
// declarations rather than through the page table directly:
//
//   ptr bcore.guest.ptr(ptr table, i32 addr): the host address of guest
//       addr where its page is in the table, null where the access must call
//       the host (Memory::page_table());
//   i1 bcore.guest.aligned(i32 addr, i32 width): addr is a multiple of width.
//
// Both are pure functions of their operands while a block runs (the table's
// entries change only between runs), so they are declared readnone; nothing
// may be speculated from them, as a page that is not there has no host
// address. GuestLoopVersioning proves both once at a loop's entry for every
// iteration the loop can run and gives the loop a copy in which they are the
// constants they were proven to be: a host address that is a base plus an
// affine offset (which SCEV and the vectorizers understand) and true. The
// accesses the proof does not cover, and the loop the proof fails for, keep
// asking; lower_guest_memory then turns every remaining call into its
// definition, one page table load per access, as a plain block does it.

llvm::Value* guest_ptr(llvm::IRBuilder<>& b, llvm::Module& m, llvm::Value* table, llvm::Value* addr);
llvm::Value* guest_aligned(llvm::IRBuilder<>& b, llvm::Module& m, llvm::Value* addr, unsigned width);
// Whether the module has either declaration.
bool has_guest_memory(const llvm::Module& m);

// For each innermost loop whose iterations SCEV bounds (its symbolic maximum
// backedge-taken count) and that reaches guest memory at addresses affine in
// it, a copy guarded at entry: every such address of every iteration lies in
// one mapped page per access, aligned. Exact either way: the copy runs only
// where what it assumes holds, and is the loop otherwise.
struct GuestLoopVersioning : llvm::PassInfoMixin<GuestLoopVersioning> {
    llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager& am);
};

// The function attribute that counts a function's proven loops (BcoreStats).
constexpr const char* kLoopsProvenAttr = "bcore-loops-proven";
void count_in_attribute(llvm::Function& f, const char* attr);
unsigned attribute_count(const llvm::Function& f, const char* attr);

// Every bcore.guest.ptr and bcore.guest.aligned call as its definition; the
// declarations go.
void lower_guest_memory(llvm::Module& m);
