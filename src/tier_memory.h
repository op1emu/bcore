#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mem.h"

// The instructions a translation read (Core::set_tiering). A tier-0
// compilation reads the block's code through a RecordingMemory, and its
// promotion to tier 1 reads the same halfwords again through a
// ReplayMemory: the promoted block runs exactly the code the tier-0 block
// ran, even if guest memory changed in between, and its translation calls
// nothing in the host (the host's reads may have side effects, and are
// counted). The translator also asks the Memory about its page table and
// callbacks: both pass those questions on, so a promotion samples them as
// a translation at that time would.
// In a namespace of its own: hosts and tests define Memory classes of their
// own, and two classes of one name in one program break each other.
namespace bcore_tier {

using CodeLog = std::vector<std::pair<uint32_t, uint16_t>>;

class ForwardingMemory : public Memory {
public:
    explicit ForwardingMemory(Memory& real) : real_(real) {}
    uint32_t base() const override { return real_.base(); }
    uint32_t size() const override { return real_.size(); }
    uint8_t read8(uint32_t addr) const override { return real_.read8(addr); }
    uint32_t read32(uint32_t addr) const override { return real_.read32(addr); }
    void write8(uint32_t addr, uint8_t val) override { real_.write8(addr, val); }
    void write16(uint32_t addr, uint16_t val) override { real_.write16(addr, val); }
    void write32(uint32_t addr, uint32_t val) override { real_.write32(addr, val); }
    void data_alignment_fault(uint32_t pc, uint32_t addr, uint32_t status) override {
        real_.data_alignment_fault(pc, addr, status);
    }
    const uint8_t* raw() const override { return real_.raw(); }
    uintptr_t fast_base() const override { return real_.fast_base(); }
    bool is_fastmem() const override { return real_.is_fastmem(); }
    uint32_t rawmem_limit() const override { return real_.rawmem_limit(); }
    uint8_t* const* page_table() const override { return real_.page_table(); }
    bool callbacks_preserve_registers() const override { return real_.callbacks_preserve_registers(); }

protected:
    Memory& real_;
};

// Reads through, and logs every halfword read.
class RecordingMemory : public ForwardingMemory {
public:
    RecordingMemory(Memory& real, CodeLog& log) : ForwardingMemory(real), log_(log) {}
    uint16_t read16(uint32_t addr) const override {
        const uint16_t value = real_.read16(addr);
        log_.emplace_back(addr, value);
        return value;
    }

private:
    CodeLog& log_;
};

// Answers halfword reads from a log only; missed() says whether any read
// fell outside it (its answer is then 0, and the translation is not to be
// used).
class ReplayMemory : public ForwardingMemory {
public:
    ReplayMemory(Memory& real, const CodeLog& log) : ForwardingMemory(real) {
        for (const auto& [addr, value] : log) halfwords_.emplace(addr, value);
    }
    uint16_t read16(uint32_t addr) const override {
        auto it = halfwords_.find(addr);
        if (it == halfwords_.end()) {
            missed_ = true;
            return 0;
        }
        return it->second;
    }
    bool missed() const { return missed_; }

private:
    std::unordered_map<uint32_t, uint16_t> halfwords_;
    mutable bool missed_ = false;
};

}  // namespace bcore_tier
