#include "evt.h"

#include <cstdio>
#include <cstdlib>

// EVT register storage — own device state, separate from CEC.
static uint32_t s_evt[16];

void evt_init() {
    for (int i = 0; i < 16; i++) s_evt[i] = 0u;
}

uint32_t evt_read(uint32_t addr) {
    return s_evt[(addr - EVT_BASE) / 4];
}

void evt_write(uint32_t addr, uint32_t val) {
    s_evt[(addr - EVT_BASE) / 4] = val;
}

uint32_t evt_get(uint32_t ivg) {
    if (ivg > 15) {
        fprintf(stderr, "evt_get: ivg %u out of range\n", ivg);
        abort();
    }
    return s_evt[ivg];
}

void evt_set(uint32_t ivg, uint32_t handler_addr) {
    if (ivg > 15) {
        fprintf(stderr, "evt_set: ivg %u out of range\n", ivg);
        abort();
    }
    s_evt[ivg] = handler_addr;
}
