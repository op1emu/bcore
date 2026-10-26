#pragma once

// How the copy-and-patch translator fills a hole in (cp_translator.cpp): a
// relocation of a stencil's code (cp_format.h) placed at `at`, with the
// hole's value. False where the value does not fit the field.

#include <cstdint>
#include <cstring>

#include "cp_format.h"

namespace bcore_cp {

// A 64-bit value hole (the others are 32-bit values, of which a 32-bit
// field holds the low half, whichever way it extends them).
inline bool wide(uint8_t hole) { return hole == kHoleCountOffset; }

inline uint32_t load32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

inline void store32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

inline bool patch_reloc(uint8_t* at, const Reloc& r, uint64_t value) {
    uint8_t* place = at + r.offset;
    switch (r.kind) {
    case kMovw0:
    case kMovw0Nc:
    case kMovw1:
    case kMovw1Nc:
    case kMovw2Nc:
    case kMovw3: {
        static constexpr unsigned kPart[] = {0, 0, 1, 1, 2, 3};
        static constexpr bool kChecked[] = {true, false, true, false, false, false};
        const unsigned k = kPart[r.kind - kMovw0];
        const uint64_t v = value + uint64_t(int64_t(r.addend));
        if (kChecked[r.kind - kMovw0] && (v >> (16 * (k + 1))) != 0) return false;
        const uint32_t imm = uint32_t(v >> (16 * k)) & 0xffff;
        store32(place, (load32(place) & ~(0xffffu << 5)) | imm << 5);
        return true;
    }
    case kBranch26: {
        const int64_t v = int64_t(value) + r.addend - int64_t(reinterpret_cast<uintptr_t>(place));
        if ((v & 3) || v < -(int64_t(1) << 27) || v >= (int64_t(1) << 27)) return false;
        store32(place, (load32(place) & 0xfc000000u) | (uint32_t(v >> 2) & 0x03ffffffu));
        return true;
    }
    case kAbs32:
    case kAbs32S: {
        const int64_t v = int64_t(value) + r.addend;
        if (wide(r.hole) && (r.kind == kAbs32 ? (v < 0 || v > int64_t(UINT32_MAX)) : (v < INT32_MIN || v > INT32_MAX)))
            return false;
        const uint32_t field = uint32_t(v);
        memcpy(place, &field, 4);
        return true;
    }
    case kPcRel32: {
        const int64_t v = int64_t(value) + r.addend - int64_t(reinterpret_cast<uintptr_t>(place));
        if (v < INT32_MIN || v > INT32_MAX) return false;
        const int32_t field = int32_t(v);
        memcpy(place, &field, 4);
        return true;
    }
    case kAbs64: {
        const uint64_t field = value + uint64_t(int64_t(r.addend));
        memcpy(place, &field, 8);
        return true;
    }
    }
    return false;
}

}  // namespace bcore_cp
