#pragma once

#include <cstdint>
#include "mmr.h"

// Event Vector Table (EVT) device.
//
// Semantics derived from refs/dv-bfin_evt.c (GNU binutils simulator).
// EVT is a separate device from the CEC: 16 writable 32-bit registers
// that hold handler addresses for each IVG level.
//
// EVT registers: EVT0–EVT15 at 0xFFE02000–0xFFE0203C
//   EVT0  = EMU handler
//   EVT1  = RST handler
//   EVT2  = NMI handler
//   EVT3  = EVX (exception) handler
//   EVT4  = IRPTEN (reserved, not dispatched)
//   EVT5  = IVHW (hardware error) handler
//   EVT6  = IVTMR (timer) handler
//   EVT7–EVT15 = general-purpose IVG handlers
//
// Access: 32-bit only. No side effects on read or write.
// Init: all entries zeroed at reset.

// Address constants and predicates defined in include/mmr.h (included above).

// Initialize all EVT entries to 0 (post-reset state).
void evt_init();

// Software-visible MMR read/write. addr must be in EVT range. No side effects.
uint32_t evt_read(uint32_t addr);
void     evt_write(uint32_t addr, uint32_t val);

// ── IVG-indexed access for CEC dispatch ──────────────────────────────────────

// Read/write EVT entry by IVG index (0–15). Mirrors cec_get_evt/cec_set_evt
// in the reference implementation (refs/dv-bfin_evt.h).
uint32_t evt_get(uint32_t ivg);
void     evt_set(uint32_t ivg, uint32_t handler_addr);
