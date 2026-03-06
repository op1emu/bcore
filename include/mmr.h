#pragma once

#include <cstdint>

// ── CEC MMR region ──────────────────────────────────────────────────────────

static constexpr uint32_t CEC_MMR_BASE          = 0xFFE02100u;
static constexpr uint32_t CEC_MMR_SIZE          = 5 * 4;       // 5 registers × 4 bytes

// CEC register indices into cec_mmr[5]
static constexpr uint32_t CEC_IDX_EVT_OVERRIDE  = 0;
static constexpr uint32_t CEC_IDX_IMASK         = 1;
static constexpr uint32_t CEC_IDX_IPEND         = 2;
static constexpr uint32_t CEC_IDX_ILAT          = 3;
static constexpr uint32_t CEC_IDX_IPRIO         = 4;

inline bool cec_is_mmr_addr(uint32_t addr) {
    return addr >= CEC_MMR_BASE && addr < CEC_MMR_BASE + CEC_MMR_SIZE;
}

// ── EVT region ──────────────────────────────────────────────────────────────

static constexpr uint32_t EVT_BASE = 0xFFE02000u;
static constexpr uint32_t EVT_SIZE = 16 * 4;  // 16 vectors × 4 bytes

inline bool evt_is_addr(uint32_t addr) {
    return addr >= EVT_BASE && addr < EVT_BASE + EVT_SIZE;
}

// ── Exception cause codes ───────────────────────────────────────────────────

static constexpr uint32_t VEC_UNDEF_I           = 0x21u;
static constexpr uint32_t VEC_ILGAL_I           = 0x22u;
static constexpr uint32_t VEC_MISALIGNI         = 0x23u;
static constexpr uint32_t VEC_ILL_RES           = 0x2Eu;
static constexpr uint32_t VEC_DCPLB_MISS        = 0x26u;
static constexpr uint32_t VEC_UNCOV             = 0x25u;

// ── System MMR space ────────────────────────────────────────────────────────

static constexpr uint32_t BFIN_MMR_BASE = 0xFFC00000u;

// ── Forward declarations of MMR dispatch functions ──────────────────────────
// Implementations in src/cec.cpp and src/evt.cpp.

struct CpuState;
class Memory;

// CEC MMR read/write (called by FlatMemory for addresses in CEC MMR range)
uint32_t cec_mmr_read(CpuState* cpu, uint32_t addr);
void     cec_mmr_write(CpuState* cpu, uint32_t addr, uint32_t val);

// EVT read/write (called by FlatMemory for addresses in EVT range)
uint32_t evt_read(uint32_t addr);
void     evt_write(uint32_t addr, uint32_t val);

// CEC exception dispatch (called by FlatMemory::fault)
extern "C" void cec_exception(CpuState* cpu, uint32_t excp);

// User-mode MMR access check (called by FlatMemory for all reads/writes)
extern "C" bool user_mmr_check(CpuState* cpu, uint32_t addr);

// CEC initialization and memory registration
void cec_init();
void cec_set_mem(Memory* mem);
