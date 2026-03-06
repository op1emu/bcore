#pragma once

#include "mem.h"
#include "mmr.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unistd.h>
#include <sys/mman.h>
#include <vector>

class FlatMemory : public Memory {
public:
    // Normal constructor: flat buffer backed by std::vector
    FlatMemory(uint32_t base, uint32_t size, CpuState* cpu)
        : base_(base), data_(size, 0),
          fastmem_(false), mmap_base_(nullptr), mmap_size_(0), cpu_(cpu) {
    }

    // Fastmem constructor: mmap-backed buffer covering [0, max(program_end, EVT_END, CEC_END))
    FlatMemory(uint32_t program_base, uint32_t program_size, bool fastmem, CpuState* cpu)
        : base_(fastmem ? 0u : program_base),
          fastmem_(fastmem), mmap_base_(nullptr), mmap_size_(0), cpu_(cpu) {
        if (fastmem) {
            uint32_t program_end = program_base + program_size;
            uint32_t evt_end     = EVT_BASE + EVT_SIZE;
            uint32_t cec_end     = CEC_MMR_BASE  + CEC_MMR_SIZE;
            mmap_size_ = static_cast<size_t>(std::max({program_end, evt_end, cec_end}));
            long page = sysconf(_SC_PAGESIZE);
            mmap_size_ = (mmap_size_ + static_cast<size_t>(page - 1))
                         & ~static_cast<size_t>(page - 1);
            mmap_base_ = static_cast<uint8_t*>(
                mmap(nullptr, mmap_size_, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0));
            if (mmap_base_ == MAP_FAILED) {
                perror("FlatMemory mmap");
                abort();
            }
        } else {
            data_.resize(program_size, 0);
        }
    }

    ~FlatMemory() override {
        if (fastmem_ && mmap_base_ && mmap_base_ != MAP_FAILED)
            munmap(mmap_base_, mmap_size_);
    }

    FlatMemory(const FlatMemory&) = delete;
    FlatMemory& operator=(const FlatMemory&) = delete;

    uint32_t base() const override { return base_; }

    uint32_t size() const override {
        return fastmem_ ? static_cast<uint32_t>(mmap_size_)
                        : static_cast<uint32_t>(data_.size());
    }

    uintptr_t fast_base() const override {
        if (fastmem_) return reinterpret_cast<uintptr_t>(mmap_base_);
        return reinterpret_cast<uintptr_t>(data_.data()) - base_;
    }

    bool is_fastmem() const override { return fastmem_; }

    // Bulk load for ELF segment loading — concrete-only, not virtual
    void load(uint32_t addr, const uint8_t* buf, uint32_t len) {
        if (fastmem_) {
            if (static_cast<size_t>(addr) + len > mmap_size_) {
                fprintf(stderr, "FlatMemory::load: out of bounds 0x%08x+%u\n", addr, len);
                abort();
            }
            memcpy(mmap_base_ + addr, buf, len);
        } else {
            uint32_t offset = addr - base_;
            if (offset + len > data_.size()) {
                fprintf(stderr, "FlatMemory::load: out of bounds 0x%08x+%u\n", addr, len);
                abort();
            }
            memcpy(data_.data() + offset, buf, len);
        }
    }

    uint8_t read8(uint32_t addr) const override {
        if (user_mmr_check(cpu_, addr)) return 0;
        if (fastmem_) return mmap_base_[addr];
        if (is_stub_mmr(addr)) return 0;
        uint32_t offset = addr - base_;
        if (offset >= data_.size()) {
            fprintf(stderr, "FlatMemory::read8: out of bounds 0x%08x\n", addr);
            fault(addr);
            return 0;
        }
        return data_[offset];
    }

    uint16_t read16(uint32_t addr) const override {
        if (user_mmr_check(cpu_, addr)) return 0;
        if (fastmem_) {
            uint16_t v; memcpy(&v, mmap_base_ + addr, 2); return v;
        }
        if (is_stub_mmr(addr)) return 0;
        uint32_t offset = addr - base_;
        if (offset > data_.size() - 2) {
            fprintf(stderr, "FlatMemory::read16: out of bounds 0x%08x\n", addr);
            fault(addr);
            return 0;
        }
        return static_cast<uint16_t>(data_[offset]) |
               (static_cast<uint16_t>(data_[offset + 1]) << 8);
    }

    uint32_t read32(uint32_t addr) const override {
        if (user_mmr_check(cpu_, addr)) return 0;
        if (fastmem_) {
            if (cec_is_mmr_addr(addr)) return cec_mmr_read(cpu_, addr);
            if (evt_is_addr(addr))     return evt_read(addr);
            uint32_t v; memcpy(&v, mmap_base_ + addr, 4); return v;
        }
        if (cec_is_mmr_addr(addr)) return cec_mmr_read(cpu_, addr);
        if (evt_is_addr(addr))     return evt_read(addr);
        if (is_stub_mmr(addr)) return 0;
        uint32_t offset = addr - base_;
        if (offset > data_.size() - 4) {
            fprintf(stderr, "FlatMemory::read32: out of bounds 0x%08x\n", addr);
            fault(addr);
            return 0;
        }
        return static_cast<uint32_t>(data_[offset]) |
               (static_cast<uint32_t>(data_[offset + 1]) << 8) |
               (static_cast<uint32_t>(data_[offset + 2]) << 16) |
               (static_cast<uint32_t>(data_[offset + 3]) << 24);
    }

    void write8(uint32_t addr, uint8_t val) override {
        if (user_mmr_check(cpu_, addr)) return;
        if (fastmem_) { mmap_base_[addr] = val; maybe_notify(addr, 1); return; }
        if (is_stub_mmr(addr)) return;
        uint32_t offset = addr - base_;
        if (offset >= data_.size()) {
            fprintf(stderr, "FlatMemory::write8: out of bounds 0x%08x\n", addr);
            fault(addr);
            return;
        }
        data_[offset] = val;
        maybe_notify(addr, 1);
    }

    void write16(uint32_t addr, uint16_t val) override {
        if (user_mmr_check(cpu_, addr)) return;
        if (fastmem_) { memcpy(mmap_base_ + addr, &val, 2); maybe_notify(addr, 2); return; }
        if (is_stub_mmr(addr)) return;
        uint32_t offset = addr - base_;
        if (offset > data_.size() - 2) {
            fprintf(stderr, "FlatMemory::write16: out of bounds 0x%08x\n", addr);
            fault(addr);
            return;
        }
        data_[offset]     = val & 0xFF;
        data_[offset + 1] = (val >> 8) & 0xFF;
        maybe_notify(addr, 2);
    }

    void write32(uint32_t addr, uint32_t val) override {
        if (user_mmr_check(cpu_, addr)) return;
        if (fastmem_) {
            if (cec_is_mmr_addr(addr)) { cec_mmr_write(cpu_, addr, val); return; }
            if (evt_is_addr(addr))     { evt_write(addr, val); return; }
            memcpy(mmap_base_ + addr, &val, 4); maybe_notify(addr, 4); return;
        }
        if (cec_is_mmr_addr(addr)) { cec_mmr_write(cpu_, addr, val); return; }
        if (evt_is_addr(addr))     { evt_write(addr, val); return; }
        if (is_stub_mmr(addr)) return;
        uint32_t offset = addr - base_;
        if (offset > data_.size() - 4) {
            fprintf(stderr, "FlatMemory::write32: out of bounds 0x%08x\n", addr);
            fault(addr);
            return;
        }
        data_[offset]     = val & 0xFF;
        data_[offset + 1] = (val >> 8) & 0xFF;
        data_[offset + 2] = (val >> 16) & 0xFF;
        data_[offset + 3] = (val >> 24) & 0xFF;
        maybe_notify(addr, 4);
    }

    const uint8_t* raw() const override {
        return fastmem_ ? mmap_base_ : data_.data();
    }
    uint8_t* raw_mut() {
        return fastmem_ ? mmap_base_ : data_.data();
    }

    void set_write_notify(std::function<void(uint32_t, uint32_t)> cb,
                          uint32_t lo, uint32_t hi) {
        notify_cb_ = std::move(cb);
        notify_lo_ = lo;
        notify_hi_ = hi;
    }

private:
    void maybe_notify(uint32_t addr, uint32_t size) const {
        if (!notify_cb_) return;
        if (addr >= notify_hi_ || addr + size <= notify_lo_) return;
        notify_cb_(addr, size);
    }

    // Returns true for system MMR addresses that are not mapped to a known
    // peripheral (CEC, EVT). Such accesses are silently ignored (stub behavior)
    // since many hardware registers (cache control, DMA, timers, etc.) don't
    // need emulation for functional correctness.
    static bool is_stub_mmr(uint32_t addr) {
        if (addr < 0xFFC00000u) return false;
        if (cec_is_mmr_addr(addr)) return false;
        if (evt_is_addr(addr))     return false;
        return true;
    }

    // Raise an exception for an invalid guest address.
    // All other unmapped addresses raise VEC_DCPLB_MISS (data CPLB miss, 0x26).
    // cpu_->pc must already hold the faulting instruction address.
    void fault(uint32_t addr) const {
        if (!cpu_) return;
        cec_exception(cpu_, VEC_DCPLB_MISS);
    }

    uint32_t base_;
    std::vector<uint8_t> data_;
    bool fastmem_;
    uint8_t* mmap_base_;
    size_t mmap_size_;
    CpuState* cpu_;
    std::function<void(uint32_t, uint32_t)> notify_cb_;
    uint32_t notify_lo_ = 0;
    uint32_t notify_hi_ = 0;
};
