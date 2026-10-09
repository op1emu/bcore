#include "core.h"

#include <sstream>

#include "bb_translator.h"
#include "decoder.h"
#include "disasm_visitor.h"
#include "jit_engine.h"

#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

Core::Core(CpuState* cpu, Memory* mem)
    : cpu_(cpu), mem_(mem) {}

Core::~Core() = default;

bool Core::init(int opt_level, int codegen_level) {
    opt_level_ = opt_level;
    codegen_level_ = codegen_level;

    jit_ = std::make_shared<JitEngine>();
    if (!jit_->init(opt_level_, codegen_level_))
        return false;

    // Use the Memory interface's is_fastmem() to determine mode.
    const bool fastmem = mem_->is_fastmem();
    // In fastmem mode, fast_base is the mmap base; JIT uses direct loads.
    // In non-fastmem mode, fast_base = data_.data() - base_; pass it for rawmem inline loads.
    const uint64_t fast_base = mem_->fast_base();

    translator_ = std::make_shared<BBTranslator>(
        mem_,
        /*unlimited=*/(cpu_->steps_remaining == 0),
        fastmem,
        fast_base);
    if (cpu_->steps_remaining)
        translator_->set_max_packets(cpu_->steps_remaining);
    applyModuleTarget();

    return true;
}

void Core::applyModuleTarget() {
    // Same source as the compiler: LLJIT's addIRModule rejects a module whose
    // non-default layout differs from its own.
    translator_->setModuleTarget(jit_->dataLayout(), jit_->targetTriple());
}

bool Core::run(uint32_t pc) {
    BbFunc fn = jit_->lookup(pc);
    if (!fn) {
        auto result = translator_->translate(pc);
        // The IR pipeline, once per module and before any dump, so a dump shows
        // the IR that is compiled. It used to run only inside the dump branch:
        // from the initial commit on, a nonzero opt_level changed the printed IR
        // and never the executed code.
        if (opt_level_ != 0)
            jit_->optimize_module(*result.module);
        if (dump_ir_) {
            llvm::errs() << "=== IR BB @ " << llvm::format_hex(pc, 10) << " ===\n";
            result.module->print(llvm::errs(), nullptr);
            llvm::errs() << "=== end IR ===\n";
        }
        if (!jit_->addModule(pc,
                             std::move(result.module),
                             std::move(result.context),
                             result.fallthrough_pc))
            return false;
        fn = jit_->lookup(pc);
        if (!fn)
            return false;
    }

    cpu_->did_jump = false;
    cpu_->pc = pc;
    cpu_->packets = 0;
    jit_->set_executing(pc);
    fn(cpu_, mem_);
    jit_->clear_executing();
    return true;
}

void Core::invalidate() {
    jit_ = std::make_shared<JitEngine>();
    // Init cannot fail here unless it failed in Core::init already (same host,
    // same levels); if it ever does, stop rather than dereference a null LLJIT.
    if (!jit_->init(opt_level_, codegen_level_))
        llvm::report_fatal_error("bcore: JIT re-initialization failed in invalidate()");
    applyModuleTarget();
}

std::tuple<std::string, uint32_t> Core::disassemble(uint32_t pc) {
    DisasmVisitor visitor;
    visitor.mem = mem_;

    std::ostringstream oss;
    visitor.set_out(&oss);

    int insn_len = decodeInstruction(visitor, pc);

    std::string text = oss.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    uint32_t next_pc = pc + static_cast<uint32_t>(insn_len);
    return {text, next_pc};
}

void Core::set_dump_ir(bool enable) {
    dump_ir_ = enable;
}
