// Host test for comm/crc8.h -- runs on the Mac, no embedded toolchain needed:
//
//   make -C spirant-daisy/v1/tests run
//
// The golden vectors below are the same literals asserted on the Pico side in
// spirant-parameter-values-rs/src/wire.rs (wire::tests::crc8_golden_vectors).
// They were cross-checked against an independent table-driven CRC-8/SMBUS
// implementation. If you change one side, change the other.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../comm/crc8.h"

static uint8_t crc8_str(const char* s)
{
    return crc8((const uint8_t*)s, strlen(s));
}

static void test_golden_vectors()
{
    const uint8_t empty[1] = {0};
    assert(crc8(empty, 0) == 0x00);  // empty input

    const uint8_t zero[] = {0x00};
    assert(crc8(zero, 1) == 0x00);

    const uint8_t ff[] = {0xFF};
    assert(crc8(ff, 1) == 0xF3);

    const uint8_t seq[] = {0x01, 0x02, 0x03, 0x04};
    assert(crc8(seq, 4) == 0xE3);

    // Canonical check value for CRC-8/SMBUS.
    assert(crc8_str("123456789") == 0xF4);

    printf("  golden vectors            ok\n");
}

static void test_full_length_frames()
{
    uint8_t buf[96];

    memset(buf, 0x00, sizeof(buf));
    assert(crc8(buf, sizeof(buf)) == 0x00);

    memset(buf, 0xFF, sizeof(buf));
    assert(crc8(buf, sizeof(buf)) == 0xBD);

    printf("  96-byte frames            ok\n");
}

static void test_detects_single_bit_flips()
{
    uint8_t frame[96];
    for(size_t i = 0; i < sizeof(frame); i++)
        frame[i] = (uint8_t)i;

    const uint8_t good = crc8(frame, sizeof(frame));

    for(int bit = 0; bit < 8; bit++)
    {
        uint8_t corrupted[96];
        memcpy(corrupted, frame, sizeof(frame));
        corrupted[42] ^= (uint8_t)(1 << bit);
        assert(crc8(corrupted, sizeof(corrupted)) != good);
    }

    printf("  single-bit detection      ok\n");
}

int main()
{
    printf("crc8_test\n");
    test_golden_vectors();
    test_full_length_frames();
    test_detects_single_bit_flips();
    printf("crc8_test PASSED\n");
    return 0;
}
