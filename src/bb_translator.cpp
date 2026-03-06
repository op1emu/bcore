#include "bb_translator.h"

#include <cstdio>

#include "mem.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>

#include "decoder.h"
#include "lift_visitor.h"

using namespace llvm;

BBTranslator::BBTranslator(Memory* mem,
                           bool unlimited, bool fastmem, uint64_t fast_base)
    : mem_(mem), unlimited_(unlimited), fastmem_(fastmem), fast_base_(fast_base) {}

BBTranslateResult BBTranslator::translate(uint32_t pc) {
    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("bb_module", *ctx);

    // Create function: void bb_0x<pc>(CpuState*, Memory*)
    char fname[32];
    snprintf(fname, sizeof(fname), "bb_0x%08x", pc);

    auto* i8ptr = Type::getInt8PtrTy(*ctx);
    auto* ft = FunctionType::get(Type::getVoidTy(*ctx), {i8ptr, i8ptr}, false);
    auto* fn = Function::Create(ft, Function::ExternalLinkage, fname, mod.get());

    auto* entry = BasicBlock::Create(*ctx, "entry", fn);
    IRBuilder<> builder(entry);

    auto arg_it = fn->arg_begin();
    Value* cpu_ptr = &*arg_it++;
    cpu_ptr->setName("cpu");
    Value* mem_ptr = &*arg_it++;
    mem_ptr->setName("mem");

    // Declare external functions used by the lifter
    auto declare_extern = [&](const char* name, FunctionType* ft) {
        mod->getOrInsertFunction(name, ft);
    };

    auto* void_ty = Type::getVoidTy(*ctx);
    auto* i32_ty = Type::getInt32Ty(*ctx);
    auto* i8_ty = Type::getInt8Ty(*ctx);
    auto* i16_ty = Type::getInt16Ty(*ctx);

    declare_extern("bfin_syscall", FunctionType::get(void_ty, {i8ptr, i8ptr}, false));
    declare_extern("mem_read8", FunctionType::get(i8_ty, {i8ptr, i32_ty}, false));
    declare_extern("mem_read16", FunctionType::get(i16_ty, {i8ptr, i32_ty}, false));
    declare_extern("mem_read32", FunctionType::get(i32_ty, {i8ptr, i32_ty}, false));
    declare_extern("mem_write8", FunctionType::get(void_ty, {i8ptr, i32_ty, i8_ty}, false));
    declare_extern("mem_write16", FunctionType::get(void_ty, {i8ptr, i32_ty, i16_ty}, false));
    declare_extern("mem_write32", FunctionType::get(void_ty, {i8ptr, i32_ty, i32_ty}, false));

    LiftVisitor visitor(builder, cpu_ptr, mem_ptr, mod.get(),
                        mem_, unlimited_,
                        fastmem_, fast_base_, mem_->rawmem_limit());

    uint32_t cur_pc = pc;
    int max_insns = 256; // safety limit per BB

    while (max_insns-- > 0 && !visitor.is_terminated()) {
        uint32_t insn_pc = cur_pc;
        auto& ctx = builder.getContext();
        char label_name[32];
        snprintf(label_name, sizeof(label_name), "insn_0x%08x_entry", insn_pc);
        auto* insn_entry = BasicBlock::Create(ctx, label_name, fn);
        snprintf(label_name, sizeof(label_name), "insn_0x%08x", insn_pc);
        auto* insn = BasicBlock::Create(ctx, label_name, fn);

        builder.CreateBr(insn_entry);
        builder.SetInsertPoint(insn);
        int bytes = decodeInstruction(visitor, cur_pc);  // sets visitor.current_pc = cur_pc
        cur_pc += bytes;
        visitor.set_fallthrough_pc(cur_pc);
        visitor.finalize_pending_exits(insn_pc, bytes);

        auto* block = builder.GetInsertBlock();
        builder.SetInsertPoint(insn_entry);
        visitor.emit_insn_len(bytes);
        builder.CreateBr(insn);
        builder.SetInsertPoint(block);
    }

    // Terminate the BB function.
    if (!visitor.is_terminated()) {
        builder.CreateRetVoid();
    } else {
        auto* block = builder.GetInsertBlock();
        if (!block->getTerminator())
            builder.CreateRetVoid();
    }

    // Verify the module
    std::string err_str;
    raw_string_ostream err_stream(err_str);
    if (verifyModule(*mod, &err_stream)) {
        fprintf(stderr, "Module verification failed for %s:\n%s\n",
                fname, err_str.c_str());
    }

    return {std::move(mod), std::move(ctx), visitor.fallthrough_pc()};
}
