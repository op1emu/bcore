#pragma once

#include <stdint.h>
#include <array>
#include <functional>
#include <tuple>
#include <vector>

/* Common to all DSP32 instructions.  */
#define BIT_MULTI_INS 0x0800

template<class OpCodeType, class Visitor>
class InstructionMatcher {
    using RetType = typename Visitor::return_type;
    using ArgsType = std::vector<std::tuple<OpCodeType, OpCodeType>>;
    using Proxy = std::function<RetType(Visitor&, OpCodeType, const ArgsType&)>;

public:
    InstructionMatcher(Proxy proxy, const std::string& pattern)
        : _proxy(proxy), _value(0), _mask(0) {
        parseMask(pattern);
        parseArgs(pattern);
    }

    bool matches(OpCodeType instruction) const { return (instruction & _mask) == _value; }
    RetType decode(Visitor& visitor, OpCodeType instruction) const { return _proxy(visitor, instruction, _args); }

protected:
    OpCodeType _value;
    OpCodeType _mask;
    ArgsType _args;
    Proxy _proxy;

private:
    bool parseMask(const std::string& pattern) {
        for (size_t i = 0, bit = 0; i < pattern.size(); i++) {
            char c = pattern[pattern.size() - 1 - i];
            switch (c) {
            case '1':
                _mask |= 1 << bit;
                _value |= 1 << bit;
                bit++;
                break;
            case '0':
                _mask |= 1 << bit;
                bit++;
                break;
            case ' ':
                break;
            default:
                bit++;
                break;
            }
        }
        return true;
    }

    bool parseArgs(const std::string& pattern) {
        char arg = 0;
        _args.resize(1);
        for (size_t i = 0, bit = 0; i < pattern.size(); i++) {
            char c = pattern[pattern.size() - 1 - i];
            if (arg != 0 && arg != c) {
                arg = 0;
                _args.emplace_back(0, 0);
            }
            if (c == ' ') {
                continue;
            }
            if (c != '0' && c != '1') {
                auto& [mask, shift] = _args.back();
                mask |= 1 << bit;
                if (arg != c) {
                    shift = bit;
                }
                arg = c;
            }
            bit++;
        }
        const auto& [mask, shift] = _args.back();
        if (mask == 0 && shift == 0) {
            _args.pop_back();
        }
        // reverse args to match order in pattern
        std::reverse(_args.begin(), _args.end());
        return true;
    }
};

template<class OpCodeType, class Visitor, size_t... index, class RetType, class... Args>
static constexpr auto createProxy(std::integer_sequence<size_t, index...>, RetType (Visitor::*function)(Args...)) {
    return [function](Visitor& visitor, OpCodeType instruction, const std::vector<std::tuple<OpCodeType, OpCodeType>>& args) -> RetType {
        (void)instruction; // Suppress unused warning for zero-argument functions
        (void)args;        // Suppress unused warning for zero-argument functions
        if constexpr (sizeof...(Args) == 0) {
            return (visitor.*function)();
        } else {
            return (visitor.*function)(static_cast<Args>((instruction & std::get<0>(args[index])) >> std::get<1>(args[index]))...);
        }
    };
}

// deduce arity of a function
template<typename F> struct function_info;
template<typename R, typename... Args> struct function_info<R(*)(Args...)> { static constexpr size_t args_count = sizeof...(Args); };
template<typename R, typename C, typename... Args> struct function_info<R(C::*)(Args...)> { static constexpr size_t args_count = sizeof...(Args); };
template<typename R, typename C, typename... Args> struct function_info<R(C::*)(Args...) const> { static constexpr size_t args_count = sizeof...(Args); };

template<class OpCodeType, class Function, class Visitor>
static constexpr InstructionMatcher<OpCodeType, Visitor> createMatcher(Function&& function, const std::string& asm_template, const std::string& pattern) {
    (void)asm_template; // Currently unused
    constexpr size_t args_count = function_info<std::decay_t<Function>>::args_count;
    using index = std::make_index_sequence<args_count>;
    auto proxy = createProxy<OpCodeType, Visitor>(index(), function);
    return InstructionMatcher<OpCodeType, Visitor>(proxy, pattern);
}

template<class Visitor>
bool decodeInstruction16(Visitor& visitor, uint16_t instruction) {
#define DEF_INSN(opcode, asm_template, binary_pattern) \
    createMatcher<uint16_t, decltype(&Visitor::decode_##opcode), Visitor>(&Visitor::decode_##opcode, asm_template, binary_pattern),

    static const std::vector<InstructionMatcher<uint16_t, Visitor>> matchers = {
        #include "instruction16.def.h"
    };
#undef DEF_INSN

    const auto matches = [instruction](const auto& matcher) { return matcher.matches(instruction); };
    auto iter = std::find_if(matchers.begin(), matchers.end(), matches);
    if (iter == matchers.end()) {
        return false;
    }
    return iter->decode(visitor, instruction);
}

template<class Visitor>
bool decodeInstruction32(Visitor& visitor, uint32_t instruction) {
#define DEF_INSN(opcode, asm_template, binary_pattern) \
    createMatcher<uint32_t, decltype(&Visitor::decode_##opcode), Visitor>(&Visitor::decode_##opcode, asm_template, binary_pattern),

    static const std::vector<InstructionMatcher<uint32_t, Visitor>> matchers = {
        #include "instruction32.def.h"
    };
#undef DEF_INSN

    const auto matches = [instruction](const auto& matcher) { return matcher.matches(instruction); };
    auto iter = std::find_if(matchers.begin(), matchers.end(), matches);
    if (iter == matchers.end()) {
        // No pattern matched: semantically illegal, advance 2 bytes (matches reference rv=0 behavior)
        return false;
    }
    return iter->decode(visitor, instruction);
}

template<class Visitor>
int decodeInstruction(Visitor& visitor, uint32_t pc) {
    visitor.set_pc(pc);
    uint16_t instruction = visitor.iftech(pc);

    if ((instruction & 0xc000) != 0xc000 || ((instruction & 0xff00) == 0xf800) || ((instruction & 0xff00) == 0xf900) || ((instruction & 0xff00) == 0xff00)) {
        // 16bit opcode
        if (!decodeInstruction16(visitor, instruction)) {
            visitor.decode_unknown_16(instruction);
        }
        return 2;
    }

    uint16_t instruction2 = visitor.iftech(pc + 2);
    uint32_t full_instruction = (static_cast<uint32_t>(instruction) << 16) | instruction2;

    // Check for parallel (multi-issue VLIW) instruction
    bool is_parallel = (instruction & BIT_MULTI_INS) != 0
                    && (instruction & 0xe800) != 0xe800;

    if (is_parallel) {
        visitor.on_parallel_begin();

        // Slot 0: 32-bit DSP instruction
        if (!decodeInstruction32(visitor, full_instruction)) {
            visitor.on_parallel_abort();
            visitor.decode_unknown_32(full_instruction);
            return 2;
        }

        bool legal = true;

        // Helper: decode a parallel slot. Slots are expected to be 16-bit;
        // if the word has 32-bit encoding bits set and matches a 32-bit pattern,
        // decode it (but mark the parallel as illegal per reference behavior).
        auto decode_parallel_slot = [&](uint32_t slot_pc, bool& slot_legal) {
            uint16_t sw = visitor.iftech(slot_pc);
            bool is_32bit_word = (sw & 0xc000) == 0xc000
                              && (sw & 0xff00) != 0xf800
                              && (sw & 0xff00) != 0xf900
                              && (sw & 0xff00) != 0xff00;
            if (is_32bit_word) {
                // Slot is 32-bit encoded → always illegal parallel, but try to decode text
                uint16_t sw2 = visitor.iftech(slot_pc + 2);
                uint32_t full_slot = (static_cast<uint32_t>(sw) << 16) | sw2;
                if (!decodeInstruction32(visitor, full_slot)) {
                    visitor.decode_unknown_32(full_slot);
                }
                slot_legal = false;
            } else {
                if (!decodeInstruction16(visitor, sw)) {
                    visitor.decode_unknown_16(sw);
                    slot_legal = false;
                }
            }
        };

        // Slot 1: at pc+4
        visitor.set_pc(pc + 4);
        visitor.on_parallel_next_slot();
        decode_parallel_slot(pc + 4, legal);

        // Slot 2: at pc+6
        visitor.set_pc(pc + 6);
        visitor.on_parallel_next_slot();
        decode_parallel_slot(pc + 6, legal);

        visitor.on_parallel_end();
        return legal ? 8 : 2;  // illegal parallel → advance only 2 bytes
    }

    // 32bit opcode
    if (!decodeInstruction32(visitor, full_instruction)) {
        // Pattern matched but semantically illegal — emit illegal, advance 2 bytes
        visitor.decode_unknown_32(full_instruction);
        return 2;
    }
    return 4;
}
