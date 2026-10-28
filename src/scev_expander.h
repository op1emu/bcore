#pragma once

#include <llvm/Config/llvm-config.h>
#include <llvm/Transforms/Utils/ScalarEvolutionExpander.h>

namespace bcore {

// An SCEVExpander for se, which writes values named name: LLVM 15's takes
// the module's DataLayout as well, 23's finds it itself.
class ScevExpander : public llvm::SCEVExpander {
public:
    ScevExpander(llvm::ScalarEvolution& se, const llvm::DataLayout& dl, const char* name)
#if LLVM_VERSION_MAJOR >= 20
        : llvm::SCEVExpander(se, name) {
        (void)dl;
    }
#else
        : llvm::SCEVExpander(se, dl, name) {
    }
#endif
};

}  // namespace bcore
