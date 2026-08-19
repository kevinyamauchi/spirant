#pragma once

#include <stddef.h>
#include <stdint.h>

// --- System Bus frame integrity ---------------------------------------------
// SMBus PEC: polynomial 0x07, initial value 0x00, no input/output reflection,
// no final XOR (CRC-8/SMBUS; check value 0xF4 over "123456789").
//
// This must stay bit-identical to crc8() in
// spirant-parameter-values-rs/src/wire.rs -- the Pico appends the CRC and the
// Daisy verifies it, so a divergence between the two would present as every
// frame failing verification. The same golden vectors are asserted on both
// sides (tests/crc8_test.cpp here, wire::tests::crc8_golden_vectors there).
//
// Bitwise rather than table-driven: 96 bytes at 30 Hz is ~23 kB/s of CRC work,
// nothing worth spending 256 bytes of flash on, and the bitwise form is the
// one that reads as obviously equivalent to the Rust side.
inline uint8_t crc8(const uint8_t* data, size_t len)
{
    uint8_t crc = 0x00;
    for(size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for(int bit = 0; bit < 8; bit++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}
