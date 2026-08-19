#pragma once

#include <stddef.h>
#include <stdint.h>

#include "crc8.h"
#include "param_table.h"

// --- System Bus response frame decode ---------------------------------------
// Decodes the fixed 97-byte GetAll/GetChanged response frame produced by
// serialize_frame() in spirant-parameter-values-rs/src/wire.rs.
//
//   byte  0        record count N (0..19)
//   bytes 1..5N    N records, 5 bytes each, ascending global_idx
//   bytes ..95     0xFF padding
//   byte  96       CRC-8 over bytes 0..95
//
// Each record is [global_idx: uint8][value: int32 big-endian], the value being
// an integer in the parameter's real unit (see param_table.h).
//
// The CRC is checked BEFORE any record is looked at (protocol section 5.2): a
// corrupt frame must not be partially applied. On mismatch the whole frame is
// discarded and the caller escalates to the GetAll fallback (section 4.4).

constexpr size_t  kFrameLen   = 97;
constexpr size_t  kRecordLen  = 5;
constexpr size_t  kCrcOffset  = kFrameLen - 1;  // 96
constexpr uint8_t kMaxRecords = kNumActiveParams;
constexpr uint8_t kPadByte    = 0xFF;

// One decoded parameter, value already clamped to the table range.
struct Record
{
    uint8_t global_idx;
    int32_t value;
};

enum class DecodeStatus
{
    kOk,           // frame verified; count records written to out[]
    kCrcMismatch,  // CRC failed -- discard, escalate per section 4.4
    kBadCount,     // count byte > 19 (includes the 0xFF-filled unknown-command frame)
};

struct DecodeResult
{
    DecodeStatus status;
    uint8_t      count;    // records written to out[]
    uint8_t      skipped;  // records naming an unknown or null global_idx
    uint8_t      clamped;  // records whose value fell outside the table range
};

// Decode `frame` into `out` (capacity `out_cap`, normally kMaxRecords).
//
// Records naming a null or out-of-range global_idx are skipped rather than
// rejecting the frame: the Pico should never emit one, so it means the two
// tables have diverged, and dropping the unknown record while applying the
// rest degrades better than dropping every parameter. Both skipped and clamped
// counts are returned so the caller can log them -- a nonzero count is a
// firmware bug worth surfacing, not a routine event.
inline DecodeResult decode_frame(const uint8_t* frame, Record* out, size_t out_cap)
{
    DecodeResult result = {DecodeStatus::kOk, 0, 0, 0};

    if(crc8(frame, kCrcOffset) != frame[kCrcOffset])
    {
        result.status = DecodeStatus::kCrcMismatch;
        return result;
    }

    const uint8_t count = frame[0];
    if(count > kMaxRecords)
    {
        result.status = DecodeStatus::kBadCount;
        return result;
    }

    for(uint8_t i = 0; i < count; i++)
    {
        const uint8_t* rec        = frame + 1 + (size_t)i * kRecordLen;
        const uint8_t  global_idx = rec[0];

        if(global_idx >= kNumSlots || !kParamTable[global_idx].active)
        {
            result.skipped++;
            continue;
        }

        // Big-endian int32. Assembled through uint32_t so the shifts stay
        // defined regardless of sign, then reinterpreted.
        const uint32_t raw = ((uint32_t)rec[1] << 24) | ((uint32_t)rec[2] << 16)
                             | ((uint32_t)rec[3] << 8) | (uint32_t)rec[4];
        int32_t value = (int32_t)raw;

        const ParamRange& range = kParamTable[global_idx];
        if(value < range.min)
        {
            value = range.min;
            result.clamped++;
        }
        else if(value > range.max)
        {
            value = range.max;
            result.clamped++;
        }

        if(result.count < out_cap)
        {
            out[result.count].global_idx = global_idx;
            out[result.count].value      = value;
            result.count++;
        }
    }

    return result;
}
