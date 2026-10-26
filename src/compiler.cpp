#include "compiler.h"
#include "bb_translator.h"
#include "guest_memory.h"
#include "guest_speculation.h"
#include "steady_skip.h"

#include <cstdint>
#include <mutex>
#include <set>

#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>

// A block function's entry count (emit), and the count under which the
// splitter calls a basic block cold: a path behind a rare branch (weighted
// 1:2^20) counts 2^10 at most, one behind as many as 19 even branches still
// 2^11. Process-wide LLVM options, set once; without them the splitter would
// ask for a profile summary instead.
static constexpr uint64_t kEntryCount = uint64_t(1) << 30;
static constexpr const char* kColdCount = "2048";

static void split_by_counts() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& options = llvm::cl::getRegisteredOptions();
        auto set = [&](const char* name, const char* value) {
            auto it = options.find(name);
            if (it != options.end()) it->second->addOccurrence(0, name, value);
        };
        set("mfs-psi-cutoff", "0");
        set("mfs-count-threshold", kColdCount);
    });
}

Compiler::Compiler() = default;
Compiler::~Compiler() = default;

bool Compiler::init(int opt_level, int codegen_level, bool split, bool stencil) {
    opt_level_ = opt_level;
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    // The IR pipeline and the backend are separate knobs: the backend level
    // decides instruction selection and register allocation, the IR pipeline
    // what the backend is given, and each has its own compile-time cost.
    const int cg = codegen_level < 0 ? opt_level : codegen_level;
    const llvm::CodeGenOpt::Level cg_level =
        cg <= 0 ? llvm::CodeGenOpt::None :
        cg == 1 ? llvm::CodeGenOpt::Less :
        cg == 3 ? llvm::CodeGenOpt::Aggressive :
                  llvm::CodeGenOpt::Default;

    auto builder = stencil
        ? llvm::Expected<llvm::orc::JITTargetMachineBuilder>(
              llvm::orc::JITTargetMachineBuilder(llvm::Triple("x86_64-unknown-linux-gnu")))
        : llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!builder) {
        llvm::errs() << "Failed to detect host: " << builder.takeError() << "\n";
        return false;
    }
    if (!stencil && !cpu_.empty()) {
        builder->setCPU(cpu_);
        builder->getFeatures() = llvm::SubtargetFeatures(features_);
    }
    const llvm::Triple& triple = builder->getTargetTriple();
    // Only on x86-64 does the splitter's hot code reach the cold region: on
    // AArch64 a conditional branch reaches 1 MiB (the cold region lies
    // hundreds away), and on Mach-O LLVM's splitter fails outright.
    if (!triple.isX86()) split = false;
    split_ = split;
    if (stencil) {
        builder->setCPU("x86-64");
        builder->setCodeModel(llvm::CodeModel::Small);
        builder->setRelocationModel(llvm::Reloc::Static);
    } else if (triple.isOSDarwin()) {
        // RuntimeDyld's Mach-O AArch64 linking makes no stub for a branch
        // out of reach (the helpers lie beyond 128 MiB): the large model
        // calls them through the GOT. No host exception crosses generated
        // code there (helper_abi.h's kHelpersCatch).
        builder->setCodeModel(llvm::CodeModel::Large);
        builder->setRelocationModel(llvm::Reloc::PIC_);
    }
    // No Swift async frame record (block_abi.h: the Memory is `swiftasync`
    // only to be passed in a register C keeps; nothing reads it from a frame).
    builder->getOptions().SwiftAsyncFramePointer = llvm::SwiftAsyncFramePointerMode::Never;
    builder->setCodeGenOptLevel(cg_level);
    // The machine function splitter moves a block's cold basic blocks into a
    // section of their own (.text.split.*), which the arena keeps apart from
    // the hot code, so that hot paths lie densely next to each other. It acts
    // on functions with profile counts: emit gives every block one, and
    // the lifter's branch weights (rarely() in lift_visitor.cpp) make its rare
    // paths' counts fall under the threshold set here.
    builder->getOptions().EnableMachineFunctionSplitter = split;
    if (split) split_by_counts();
    auto machine = builder->createTargetMachine();
    if (!machine) {
        llvm::errs() << "Failed to create TargetMachine: " << machine.takeError() << "\n";
        return false;
    }
    machine_ = std::move(*machine);

    // The IR pipeline's TTI describes exactly the CPU and features the code is
    // generated for. Without a TargetMachine, PassBuilder's TargetIRAnalysis
    // is the generic implementation: zero vector registers for
    // LoopVectorize/SLP, no x86 unrolling preferences, generic
    // SimplifyCFG/LICM/JumpThreading costs.
    // Clang's -O1 leaves both vectorizers off and -O2/-O3 turn both on; the
    // PipelineTuningOptions defaults (loop vectorization on, SLP off) match
    // neither, so follow clang.
    llvm::PipelineTuningOptions tuning;
    tuning.LoopVectorization = opt_level >= 2 && !stencil;
    tuning.LoopInterleaving = opt_level >= 2 && !stencil;
    tuning.SLPVectorization = opt_level >= 2 && !stencil;
    pb_ = std::make_unique<llvm::PassBuilder>(machine_.get(), tuning);
    // PassBuilder(TM) registers the same TargetIRAnalysis itself; registering
    // it first makes that explicit (the later default registration is then a
    // no-op).
    fam_.registerPass([this] { return machine_->getTargetIRAnalysis(); });
    pb_->registerModuleAnalyses(mam_);
    pb_->registerCGSCCAnalyses(cgam_);
    pb_->registerFunctionAnalyses(fam_);
    pb_->registerLoopAnalyses(lam_);
    pb_->crossRegisterProxies(lam_, fam_, cgam_, mam_);

    stream_ = std::make_unique<llvm::raw_svector_ostream>(object_);
    backend_ = std::make_unique<llvm::legacy::PassManager>();
    if (machine_->addPassesToEmitMC(*backend_, mc_, *stream_, /*DisableVerify=*/true)) {
        llvm::errs() << "The target cannot emit object files\n";
        return false;
    }
    return true;
}

// What the default O1 pipeline does to a block's straight-line code, without
// its module passes (IPSCCP, the inliner's CGSCC walk, globals), loop passes
// and the per-function passes that find nothing there: about a tenth of a
// promotion's compile time on the boot.
const char* const Compiler::kLightPipeline =
    "function(sroa,early-cse<memssa>,instcombine,simplifycfg,gvn,dse,adce,simplifycfg)";

void Compiler::optimize(llvm::Module& mod, bool light) {
    if (opt_level_ == 0) {
        // No pipeline, but a loop block's guest memory still lowered to the
        // page table, as the pipeline would have (guest_memory.h).
        lower_guest_memory(mod);
        return;
    }

    const llvm::OptimizationLevel lvl =
        opt_level_ == 1 ? llvm::OptimizationLevel::O1 :
        opt_level_ == 3 ? llvm::OptimizationLevel::O3 :
                          llvm::OptimizationLevel::O2;
    // Results of this module only: the next module may be allocated where
    // this one was.
    auto forget = [this] {
        lam_.clear();
        fam_.clear();
        cgam_.clear();
        mam_.clear();
    };

    // A loop block's guest memory (guest_memory.h): its loops in canonical
    // form and its shadow in SSA values first, so that SCEV sees the
    // registers as inductions, then proven copies, then the rest as plain
    // accesses. The default pipeline then cleans up after both.
    const bool guest_memory = has_guest_memory(mod);  // lower_guest_memory takes the declarations
    if (guest_memory) {
        llvm::ModulePassManager pre;
        llvm::cantFail(pb_->parsePassPipeline(
            pre, "function(sroa,early-cse<memssa>,instcombine,simplifycfg,loop-simplify,lcssa)"));
        pre.run(mod, mam_);
        llvm::ModulePassManager versioning;
        versioning.addPass(llvm::createModuleToFunctionPassAdaptor(GuestLoopVersioning()));
        versioning.run(mod, mam_);
        lower_guest_memory(mod);
        // The proven copies cleaned up, their saturating sums speculated
        // (guest_speculation.h), and the copies that asked for it
        // vectorized: only those, whatever the opt level.
        llvm::ModulePassManager spec;
        llvm::cantFail(pb_->parsePassPipeline(spec, "function(instcombine,simplifycfg,loop-simplify,lcssa)"));
        spec.addPass(llvm::createModuleToFunctionPassAdaptor(SatReductionSpeculation()));
        llvm::cantFail(pb_->parsePassPipeline(spec, "function(adce,instcombine,loop-simplify,lcssa)"));
        spec.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::LoopVectorizePass(
            llvm::LoopVectorizeOptions().setVectorizeOnlyWhenForced(true).setInterleaveOnlyWhenForced(true))));
        llvm::cantFail(pb_->parsePassPipeline(spec, "function(instcombine,simplifycfg)"));
        spec.run(mod, mam_);
        mam_.clear();
        fam_.clear();
    }
    // Loops that wait skip to their last iteration (steady_skip.h): last, on
    // the loops as the passes above left them (the speculative and proven
    // copies count by their addresses, and stay out).
    bool loop_blocks = false;
    for (auto& f : mod) loop_blocks |= f.hasFnAttribute("bcore-loop-block");
    if (steady_skip_ && loop_blocks) {
        llvm::ModulePassManager steady;
        // A loop block without guest memory had none of the above: its
        // shadow in SSA values first.
        llvm::cantFail(pb_->parsePassPipeline(steady, guest_memory
            ? "function(loop-simplify,lcssa)"
            : "function(sroa,early-cse<memssa>,instcombine,simplifycfg,loop-simplify,lcssa)"));
        steady.addPass(llvm::createModuleToFunctionPassAdaptor(SteadyLoopSkip(steady_skip_ == 1)));
        steady.run(mod, mam_);
        mam_.clear();
        fam_.clear();
    }
    llvm::ModulePassManager mpm;
    if (light && !loop_blocks)
        llvm::cantFail(pb_->parsePassPipeline(mpm, kLightPipeline));
    else
        mpm = pb_->buildPerModuleDefaultPipeline(lvl);
    if (!post_pipeline_.empty())
        llvm::cantFail(pb_->parsePassPipeline(mpm, post_pipeline_));  // checked by set_post_pipeline
    mpm.run(mod, mam_);
    forget();
}

std::unique_ptr<llvm::MemoryBuffer> Compiler::emit(llvm::Module& mod) {
    // For the splitter only, after the IR pipeline (see init()).
    if (split_)
        for (llvm::Function& function : mod)
            if (!function.isDeclaration() && !function.hasFnAttribute(kNoSplitAttr)) function.setEntryCount(kEntryCount);
    object_.clear();
    backend_->run(mod);
    if (object_.empty()) return nullptr;
    return llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(object_.data(), object_.size()),
                                                mod.getModuleIdentifier());
}

bool Compiler::set_post_pipeline(const std::string& pipeline) {
    if (!pipeline.empty()) {
        llvm::ModulePassManager check;
        if (auto err = pb_->parsePassPipeline(check, pipeline)) {
            llvm::consumeError(std::move(err));
            return false;
        }
    }
    post_pipeline_ = pipeline;
    return true;
}

std::string Compiler::dataLayout() const { return machine_->createDataLayout().getStringRepresentation(); }
std::string Compiler::targetTriple() const { return machine_->getTargetTriple().str(); }

ModuleCounts count_module(const llvm::Module& mod) {
    ModuleCounts counts;
    for (const llvm::Function& f : mod) {
        if (f.isDeclaration()) continue;
        for (const llvm::BasicBlock& block : f) {
            ++counts.ir_blocks;
            counts.ir_instructions += block.size();
        }
        if (f.hasFnAttribute(BBTranslator::kLoopBlockAttr)) counts.loop_blocks++;
        counts.loops_proven += attribute_count(f, kLoopsProvenAttr);
        counts.loops_speculated += attribute_count(f, kLoopsSpeculatedAttr);
        counts.loops_steady += attribute_count(f, kLoopsSteadyAttr);
        counts.dag_circular += attribute_count(f, BBTranslator::kCircularAttr);
        // A vectorized loop: its latch says so (as does its scalar
        // remainder's) and does vector work. A speculative copy counts once,
        // however many vector loops it became (kSpeculatedLoopMD).
        std::set<const llvm::MDNode*> copies;
        for (const llvm::BasicBlock& block : f) {
            auto* loop = block.getTerminator() ? block.getTerminator()->getMetadata(llvm::LLVMContext::MD_loop) : nullptr;
            if (!loop) continue;
            bool vectorized = false, vector = false;
            const llvm::MDNode* copy = nullptr;
            for (auto& op : loop->operands())
                if (auto* node = llvm::dyn_cast<llvm::MDNode>(op.get()); node && node->getNumOperands() &&
                    llvm::isa<llvm::MDString>(node->getOperand(0))) {
                    const llvm::StringRef name = llvm::cast<llvm::MDString>(node->getOperand(0))->getString();
                    if (name == "llvm.loop.isvectorized") vectorized = true;
                    if (name == kSpeculatedLoopMD) copy = node;
                }
            for (auto& inst : block) vector |= inst.getType()->isVectorTy();
            if (!vectorized || !vector) continue;
            if (copy) copies.insert(copy);
            else counts.loops_vectorized++;
        }
        counts.loops_vectorized += copies.size();
    }
    return counts;
}
