#include "wasm_object.h"

#include <cstring>

#include <llvm/BinaryFormat/Wasm.h>
#include <llvm/Object/Wasm.h>
#include <llvm/Support/Error.h>

namespace bcore {

namespace {

const char* kind_name(uint8_t kind) {
    switch (kind) {
    case llvm::wasm::WASM_EXTERNAL_FUNCTION: return "function";
    case llvm::wasm::WASM_EXTERNAL_TABLE: return "table";
    case llvm::wasm::WASM_EXTERNAL_MEMORY: return "memory";
    case llvm::wasm::WASM_EXTERNAL_GLOBAL: return "global";
    case llvm::wasm::WASM_EXTERNAL_TAG: return "tag";
    }
    return "import";
}

uint8_t external_kind(WasmImportKind kind) {
    switch (kind) {
    case WasmImportKind::Function: return llvm::wasm::WASM_EXTERNAL_FUNCTION;
    case WasmImportKind::Table: return llvm::wasm::WASM_EXTERNAL_TABLE;
    case WasmImportKind::Memory: return llvm::wasm::WASM_EXTERNAL_MEMORY;
    case WasmImportKind::Global: return llvm::wasm::WASM_EXTERNAL_GLOBAL;
    }
    return 0xff;
}

// A relocation whose value in the object is the module's own: an index.
bool final_value(uint8_t type) {
    switch (type) {
    case llvm::wasm::R_WASM_FUNCTION_INDEX_LEB:
    case llvm::wasm::R_WASM_TYPE_INDEX_LEB:
    case llvm::wasm::R_WASM_GLOBAL_INDEX_LEB:
    case llvm::wasm::R_WASM_TABLE_NUMBER_LEB:
        return true;
    }
    return false;
}

}  // namespace

std::string check_wasm_object(llvm::MemoryBufferRef object, const WasmImport* imports, size_t n,
                              const std::string& name) {
    auto file = llvm::object::ObjectFile::createWasmObjectFile(object);
    if (!file) return "not a WebAssembly object: " + llvm::toString(file.takeError());
    const llvm::object::WasmObjectFile& wasm = **file;
    for (const llvm::object::SectionRef& section : wasm.sections()) {
        const llvm::object::WasmSection& s = wasm.getWasmSection(section);
        switch (s.Type) {
        case llvm::wasm::WASM_SEC_CUSTOM:
            continue;  // what engines ignore (linking, relocations, producers, features)
        case llvm::wasm::WASM_SEC_TYPE:
        case llvm::wasm::WASM_SEC_IMPORT:
        case llvm::wasm::WASM_SEC_FUNCTION:
        case llvm::wasm::WASM_SEC_EXPORT:
        case llvm::wasm::WASM_SEC_CODE:
            break;
        default:
            return "a section of kind " + std::to_string(s.Type) + " (" +
                   llvm::wasm::sectionTypeToString(s.Type).str() + ")";
        }
        for (const llvm::wasm::WasmRelocation& r : s.Relocations)
            if (!final_value(r.Type))
                return "a relocation of type " + std::to_string(r.Type) + " (" +
                       llvm::wasm::relocTypetoString(r.Type).str() + ") in its " +
                       llvm::wasm::sectionTypeToString(s.Type).str() + " section";
    }
    for (const llvm::wasm::WasmImport& import : wasm.imports()) {
        bool given = false;
        for (size_t i = 0; i < n && !given; ++i)
            given = import.Module == "env" && import.Field == imports[i].name &&
                    import.Kind == external_kind(imports[i].kind);
        if (!given)
            return std::string("an import of ") + kind_name(import.Kind) + " " + import.Module.str() + "." +
                   import.Field.str();
    }
    for (const llvm::wasm::WasmExport& e : wasm.exports())
        if (e.Name == name && e.Kind == llvm::wasm::WASM_EXTERNAL_FUNCTION) return "";
    return "no function " + name + " among its exports";
}

}  // namespace bcore
