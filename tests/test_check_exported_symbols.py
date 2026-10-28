#!/usr/bin/env python3
"""Tests of check_exported_symbols.py's reading of a demangled symbol:
python3 -m unittest tests/test_check_exported_symbols.py"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from check_exported_symbols import foreign_names  # noqa: E402


class ForeignNamesTest(unittest.TestCase):
    def test_bcore_std_and_llvm_names_pass(self):
        for symbol in [
            "bcore::JitEngine::lookup(unsigned int)",
            "bcore::bcore_cp::kCode",
            "std::vector<unsigned int, std::allocator<unsigned int>>::_M_realloc_insert(unsigned int&&)",
            "std::_Hashtable<unsigned int, std::pair<unsigned int const, bcore::BBInfo>>::find(unsigned int const&)",
            "llvm::PatternMatch::BinaryOp_match<llvm::PatternMatch::apint_match, 28u, false>::match<llvm::Value>(unsigned int, llvm::Value*)",
            "vtable for bcore::Memory",
            "typeinfo name for bcore::CodeArenaPool",
            "BcoreEventSink::onTranslateEnd(unsigned int, unsigned long)",
            "std::_Function_handler<void (llvm::Instruction*), llvm::SCEVExpander::SCEVExpander(llvm::ScalarEvolution&)::'lambda'(llvm::Instruction*)>::_M_invoke(std::_Any_data const&, llvm::Instruction*&&)",
            "std::__cxx11::basic_string<char> bcore::name[abi:cxx11](bool)",
            "__clang_call_terminate",
            "___clang_call_terminate",
            "std::enable_if<!is_same_v<llvm::ModuleToFunctionPassAdaptor, llvm::PassManager<llvm::Module, llvm::AnalysisManager<llvm::Module>>>, void>::type llvm::PassManager<llvm::Module, llvm::AnalysisManager<llvm::Module>>::addPass<llvm::ModuleToFunctionPassAdaptor>(llvm::ModuleToFunctionPassAdaptor&&)",
        ]:
            self.assertEqual(foreign_names(symbol), [], symbol)

    def test_a_global_type_in_a_template_argument(self):
        self.assertEqual(
            foreign_names("std::_Hashtable<unsigned int, std::pair<unsigned int const, BBInfo>>::find(unsigned int const&)"),
            ["BBInfo"])

    def test_a_global_type_in_a_parameter(self):
        self.assertEqual(foreign_names("bcore::cec_raise(CpuState*, unsigned int)"), ["CpuState"])

    def test_a_global_function_or_c_name(self):
        self.assertEqual(foreign_names("mem_read8"), ["mem_read8"])
        self.assertEqual(foreign_names("_mem_read8"), ["_mem_read8"])  # Mach-O's C name
        self.assertEqual(foreign_names("attribute_count(llvm::Function const&, char const*)"), ["attribute_count"])

    def test_the_vtable_of_a_global_type(self):
        self.assertEqual(foreign_names("vtable for ArenaMemoryManager"), ["ArenaMemoryManager"])
        self.assertEqual(foreign_names("typeinfo for Memory"), ["Memory"])


if __name__ == "__main__":
    unittest.main()
