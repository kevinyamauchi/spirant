// Host tests for the USB-MIDI packet parser (plan section 2.3).
//
//   make -C spirant-hw-interface-esp32/tests run
//
// src/midi/usb_midi_parser.* is deliberately free of Arduino.h and of the
// ESP32_Host_MIDI headers, so the decode that every later phase depends on can
// be exercised without a board.

#include <assert.h>
#include <stdio.h>

#include "../spirant_hw_interface/src/midi/usb_midi_parser.h"

namespace
{

int g_checks = 0;

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        ++g_checks;                                                            \
        if (!(cond))                                                           \
        {                                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            assert(false);                                                     \
        }                                                                      \
    } while (0)

MidiMessage parseOk(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    const uint8_t packet[4] = {b0, b1, b2, b3};
    MidiMessage   m{};
    const bool    ok = parseUsbMidiPacket(packet, m);
    CHECK(ok);
    return m;
}

bool parseRejects(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    const uint8_t packet[4] = {b0, b1, b2, b3};
    MidiMessage   m{};
    return !parseUsbMidiPacket(packet, m);
}

// --- Channel voice -----------------------------------------------------------

void test_note_on()
{
    // Cable 0, CIN 9 (note-on), channel 1 (status 0x90), note 60, velocity 100.
    const MidiMessage m = parseOk(0x09, 0x90, 60, 100);

    CHECK(m.type == MidiType::NoteOn);
    CHECK(m.channel == 0);
    CHECK(m.data1 == 60);
    CHECK(m.data2 == 100);
    CHECK(m.status == 0x90);
    CHECK(m.cable == 0);
    CHECK(m.isChannelMessage());

    printf("  note on ok\n");
}

void test_note_off()
{
    const MidiMessage m = parseOk(0x08, 0x85, 42, 64);

    CHECK(m.type == MidiType::NoteOff);
    CHECK(m.channel == 5);
    CHECK(m.data1 == 42);

    printf("  note off ok\n");
}

void test_note_on_velocity_zero_is_note_off()
{
    // The normalisation the whole active-note count depends on. A source that
    // releases with velocity-0 note-ons is entirely conformant, and if this
    // came through as NoteOn the held count would only ever rise.
    const MidiMessage m = parseOk(0x09, 0x90, 60, 0);

    CHECK(m.type == MidiType::NoteOff);
    CHECK(m.data1 == 60);
    CHECK(m.data2 == 0);
    CHECK(m.status == 0x90);  // The raw status is preserved, not rewritten.

    printf("  note on vel 0 -> note off ok\n");
}

void test_control_change()
{
    // Breath, the design's central signal: CC2 on channel 1, value 96.
    const MidiMessage m = parseOk(0x0B, 0xB0, 2, 96);

    CHECK(m.type == MidiType::ControlChange);
    CHECK(m.channel == 0);
    CHECK(m.data1 == 2);
    CHECK(m.data2 == 96);

    printf("  control change ok\n");
}

void test_program_and_channel_pressure_clear_byte3()
{
    // Both are one-data-byte messages; byte 3 is padding and must not leak into
    // data2, where a consumer would read it as a value.
    const MidiMessage pgm = parseOk(0x0C, 0xC3, 7, 0x7F);
    CHECK(pgm.type == MidiType::ProgramChange);
    CHECK(pgm.channel == 3);
    CHECK(pgm.data1 == 7);
    CHECK(pgm.data2 == 0);

    const MidiMessage at = parseOk(0x0D, 0xD0, 90, 0x7F);
    CHECK(at.type == MidiType::ChannelPressure);
    CHECK(at.data1 == 90);
    CHECK(at.data2 == 0);

    printf("  program change / channel pressure padding ok\n");
}

void test_poly_pressure()
{
    const MidiMessage m = parseOk(0x0A, 0xA2, 64, 30);

    CHECK(m.type == MidiType::PolyPressure);
    CHECK(m.channel == 2);

    printf("  poly pressure ok\n");
}

// --- Pitch bend --------------------------------------------------------------

void test_pitch_bend_centre()
{
    // Centre is LSB 0x00, MSB 0x40 -> 8192.
    const MidiMessage m = parseOk(0x0E, 0xE0, 0x00, 0x40);

    CHECK(m.type == MidiType::PitchBend);
    CHECK(m.bend14() == 8192);
    CHECK(m.bendSigned() == 0);

    printf("  pitch bend centre ok\n");
}

void test_pitch_bend_extremes()
{
    const MidiMessage lo = parseOk(0x0E, 0xE0, 0x00, 0x00);
    CHECK(lo.bend14() == 0);
    CHECK(lo.bendSigned() == -8192);

    const MidiMessage hi = parseOk(0x0E, 0xE0, 0x7F, 0x7F);
    CHECK(hi.bend14() == 16383);
    CHECK(hi.bendSigned() == 8191);

    // An asymmetric value, to catch an LSB/MSB swap that the symmetric cases
    // above would sail straight through.
    const MidiMessage mid = parseOk(0x0E, 0xE0, 0x01, 0x40);
    CHECK(mid.bend14() == 8193);
    CHECK(mid.bendSigned() == 1);

    printf("  pitch bend extremes ok\n");
}

// --- System ------------------------------------------------------------------

void test_system_realtime()
{
    // Timing clock, CIN 0xF.
    const MidiMessage m = parseOk(0x0F, 0xF8, 0, 0);

    CHECK(m.type == MidiType::SystemRealtime);
    CHECK(m.channel == MidiMessage::kChannelNone);
    CHECK(!m.isChannelMessage());

    printf("  system realtime ok\n");
}

void test_system_common()
{
    const MidiMessage spp = parseOk(0x03, 0xF2, 0x10, 0x20);
    CHECK(spp.type == MidiType::SystemCommon);
    CHECK(spp.channel == MidiMessage::kChannelNone);

    const MidiMessage tune = parseOk(0x05, 0xF6, 0, 0);
    CHECK(tune.type == MidiType::SystemCommon);

    printf("  system common ok\n");
}

// --- Rejections --------------------------------------------------------------

void test_rejected_packets()
{
    // SysEx fragments: the transport reassembles these and delivers them on a
    // different callback, so seeing one here means something is wrong. Rejected
    // rather than guessed at.
    CHECK(parseRejects(0x04, 0xF0, 0x7E, 0x00));
    CHECK(parseRejects(0x06, 0x7F, 0xF7, 0x00));
    CHECK(parseRejects(0x07, 0x01, 0x02, 0xF7));

    // A CIN-5 packet that is a genuine single-byte SysEx tail, not 0xF6.
    CHECK(parseRejects(0x05, 0xF7, 0x00, 0x00));

    // Reserved CINs.
    CHECK(parseRejects(0x00, 0x00, 0x00, 0x00));
    CHECK(parseRejects(0x01, 0x00, 0x00, 0x00));

    // CIN 0xF with a non-realtime status is a SysEx tail, not a message.
    CHECK(parseRejects(0x0F, 0x40, 0x00, 0x00));

    printf("  rejected packets ok\n");
}

void test_cable_number_decoded()
{
    // Cable 3, note-on. A device with several virtual cables must not have its
    // cable number folded into the CIN.
    const MidiMessage m = parseOk(0x39, 0x90, 60, 100);

    CHECK(m.cable == 3);
    CHECK(m.type == MidiType::NoteOn);
    CHECK(m.data1 == 60);

    printf("  cable number ok\n");
}

void test_all_channels_decode()
{
    for (uint8_t ch = 0; ch < 16; ++ch)
    {
        const uint8_t     status = static_cast<uint8_t>(0xB0 | ch);
        const MidiMessage m      = parseOk(0x0B, status, 2, 64);
        CHECK(m.channel == ch);
        CHECK(m.type == MidiType::ControlChange);
    }

    printf("  all 16 channels ok\n");
}

void test_type_names_never_null()
{
    const MidiType kAll[] = {
        MidiType::None,          MidiType::NoteOff,        MidiType::NoteOn,
        MidiType::PolyPressure,  MidiType::ControlChange,  MidiType::ProgramChange,
        MidiType::ChannelPressure, MidiType::PitchBend,    MidiType::SystemCommon,
        MidiType::SystemRealtime,
    };

    for (MidiType t : kAll)
    {
        const char* name = midiTypeName(t);
        CHECK(name != nullptr);
        CHECK(name[0] != '\0');
    }

    printf("  type names ok\n");
}

}  // namespace

int main()
{
    printf("usb_midi_parser_test\n");

    test_note_on();
    test_note_off();
    test_note_on_velocity_zero_is_note_off();
    test_control_change();
    test_program_and_channel_pressure_clear_byte3();
    test_poly_pressure();
    test_pitch_bend_centre();
    test_pitch_bend_extremes();
    test_system_realtime();
    test_system_common();
    test_rejected_packets();
    test_cable_number_decoded();
    test_all_channels_decode();
    test_type_names_never_null();

    printf("usb_midi_parser_test: %d checks passed\n", g_checks);
    return 0;
}
