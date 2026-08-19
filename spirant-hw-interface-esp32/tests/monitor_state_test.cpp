// Host tests for the MIDI monitor snapshot (plan sections 2.0 and 2.4).
//
//   make -C spirant-hw-interface-esp32/tests run
//
// This is the state the drain writes and the UI task renders. Its locking lives
// one layer up in monitor_access.h and needs FreeRTOS; the logic here does not,
// which is the whole reason the two are separate files.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../spirant_hw_interface/src/midi/monitor_state.h"
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

MidiMessage noteOn(uint8_t ch, uint8_t note, uint8_t vel)
{
    MidiMessage m{};
    m.type    = MidiType::NoteOn;
    m.channel = ch;
    m.data1   = note;
    m.data2   = vel;
    m.status  = static_cast<uint8_t>(0x90 | ch);
    return m;
}

MidiMessage noteOff(uint8_t ch, uint8_t note)
{
    MidiMessage m{};
    m.type    = MidiType::NoteOff;
    m.channel = ch;
    m.data1   = note;
    m.data2   = 0;
    m.status  = static_cast<uint8_t>(0x80 | ch);
    return m;
}

MidiMessage cc(uint8_t ch, uint8_t number, uint8_t value)
{
    MidiMessage m{};
    m.type    = MidiType::ControlChange;
    m.channel = ch;
    m.data1   = number;
    m.data2   = value;
    m.status  = static_cast<uint8_t>(0xB0 | ch);
    return m;
}

MidiMessage bend(uint8_t ch, uint16_t value14)
{
    MidiMessage m{};
    m.type    = MidiType::PitchBend;
    m.channel = ch;
    m.data1   = static_cast<uint8_t>(value14 & 0x7F);
    m.data2   = static_cast<uint8_t>((value14 >> 7) & 0x7F);
    m.status  = static_cast<uint8_t>(0xE0 | ch);
    return m;
}

// --- Defaults ----------------------------------------------------------------

void test_initial_state()
{
    MonitorState s;
    const MonitorSnapshot& d = s.data();

    CHECK(!d.usb_device_present);
    CHECK(!d.usb_endpoint_ready);
    CHECK(!d.note_seen);
    CHECK(!d.breath_seen);
    CHECK(!d.bend_seen);
    CHECK(d.active_note_count == 0);
    CHECK(d.lowest_active_note == 0xFF);
    // Centre, so the bend bar draws a centred hairline before anything arrives
    // rather than a full-left bar.
    CHECK(d.bend14 == 8192);
    CHECK(d.messages == 0);

    for (uint8_t i = 0; i < kMonitorCcSlots; ++i) CHECK(!d.ccs[i].used);

    printf("  initial state ok\n");
}

// --- Notes -------------------------------------------------------------------

void test_note_counting()
{
    MonitorState s;

    s.onMessage(noteOn(0, 60, 100), 10, 10000);
    CHECK(s.data().active_note_count == 1);
    CHECK(s.data().lowest_active_note == 60);
    CHECK(s.data().last_note == 60);
    CHECK(s.data().last_note_was_on);
    CHECK(s.data().note_seen);

    s.onMessage(noteOn(0, 64, 90), 11, 11000);
    s.onMessage(noteOn(0, 55, 80), 12, 12000);
    CHECK(s.data().active_note_count == 3);
    CHECK(s.data().lowest_active_note == 55);

    s.onMessage(noteOff(0, 55), 13, 13000);
    CHECK(s.data().active_note_count == 2);
    CHECK(s.data().lowest_active_note == 60);
    CHECK(!s.data().last_note_was_on);

    s.onMessage(noteOff(0, 60), 14, 14000);
    s.onMessage(noteOff(0, 64), 15, 15000);
    CHECK(s.data().active_note_count == 0);
    CHECK(s.data().lowest_active_note == 0xFF);

    printf("  note counting ok\n");
}

void test_duplicate_note_on_counted_once()
{
    // A retrigger without an intervening note-off must not double-count, or the
    // held figure drifts upward over a long session and never comes back.
    MonitorState s;

    s.onMessage(noteOn(0, 60, 100), 10, 10000);
    s.onMessage(noteOn(0, 60, 110), 11, 11000);
    CHECK(s.data().active_note_count == 1);

    s.onMessage(noteOff(0, 60), 12, 12000);
    CHECK(s.data().active_note_count == 0);

    printf("  duplicate note on ok\n");
}

void test_unmatched_note_off_does_not_underflow()
{
    // Plugging in mid-phrase, or a dropped note-on, must not wrap the count to
    // 255.
    MonitorState s;

    s.onMessage(noteOff(0, 60), 10, 10000);
    CHECK(s.data().active_note_count == 0);

    s.onMessage(noteOff(0, 72), 11, 11000);
    CHECK(s.data().active_note_count == 0);
    CHECK(s.data().lowest_active_note == 0xFF);

    printf("  unmatched note off ok\n");
}

void test_note_on_velocity_zero_through_parser()
{
    // End to end: the parser's normalisation and the counter agree. This is the
    // pairing that actually matters on hardware.
    MonitorState  s;
    const uint8_t on[4]  = {0x09, 0x90, 60, 100};
    const uint8_t off[4] = {0x09, 0x90, 60, 0};

    MidiMessage m{};
    CHECK(parseUsbMidiPacket(on, m));
    s.onMessage(m, 10, 10000);
    CHECK(s.data().active_note_count == 1);

    CHECK(parseUsbMidiPacket(off, m));
    s.onMessage(m, 11, 11000);
    CHECK(s.data().active_note_count == 0);

    printf("  velocity-0 release through parser ok\n");
}

// --- Breath ------------------------------------------------------------------

void test_breath_tracking()
{
    MonitorState s;

    s.onMessage(cc(0, 2, 40), 10, 10000);
    CHECK(s.data().breath_seen);
    CHECK(s.data().breath == 40);
    CHECK(s.data().breath_updates == 1);

    s.onMessage(cc(0, 2, 90), 11, 10500);
    CHECK(s.data().breath == 90);
    CHECK(s.data().breath_updates == 2);
    CHECK(s.data().breath_min_interval_us == 500);

    // A slower update must not raise the minimum.
    s.onMessage(cc(0, 2, 95), 20, 13000);
    CHECK(s.data().breath_min_interval_us == 500);

    // A faster one must lower it.
    s.onMessage(cc(0, 2, 99), 21, 13200);
    CHECK(s.data().breath_min_interval_us == 200);

    // Breath never occupies a generic CC slot; it has its own bar.
    for (uint8_t i = 0; i < kMonitorCcSlots; ++i) CHECK(!s.data().ccs[i].used);

    printf("  breath tracking ok\n");
}

void test_breath_same_batch_timestamp_ignored()
{
    // Every message in one drain pass is folded in with the same timestamp, so
    // a zero gap means "same batch", not "simultaneous". Counting it would drive
    // breath_min_interval_us to 0 the first time two CC2s shared a drain, and
    // 0 would then be reported as the measured rate.
    MonitorState s;

    s.onMessage(cc(0, 2, 10), 10, 10000);
    s.onMessage(cc(0, 2, 20), 10, 10000);
    s.onMessage(cc(0, 2, 30), 10, 10000);

    CHECK(s.data().breath_updates == 3);
    CHECK(s.data().breath_min_interval_us == 0xFFFFFFFFUL);  // Still unmeasured.

    printf("  breath same-batch timestamps ignored ok\n");
}

void test_breath_rate_is_timestamp_independent()
{
    // The authoritative CC2 rate: a count over a wall-clock window, so it is
    // correct even when whole bursts share a timestamp. This is the figure
    // section 2.5 reports back into the section 6.4 bandwidth budget.
    MonitorState s;

    s.tick(1000);

    // 400 breath messages and 100 of something else, all in batches of 4 that
    // share a timestamp -- the case that defeats the interval measurement.
    for (int i = 0; i < 100; ++i)
    {
        for (int j = 0; j < 4; ++j) s.onMessage(cc(0, 2, 64), 1000, 50000);
        s.onMessage(cc(0, 74, 1), 1000, 50000);
    }

    s.tick(2000);

    CHECK(s.data().breath_per_sec == 400);
    CHECK(s.data().messages_per_sec == 500);
    CHECK(s.data().breath_min_interval_us == 0xFFFFFFFFUL);  // Never resolved, as expected.

    // And it resets per window rather than accumulating.
    s.tick(3000);
    CHECK(s.data().breath_per_sec == 0);

    printf("  breath rate is timestamp-independent ok\n");
}

// --- Pitch bend --------------------------------------------------------------

void test_bend_tracking()
{
    MonitorState s;

    s.onMessage(bend(0, 8192), 10, 10000);
    CHECK(s.data().bend_seen);
    CHECK(s.data().bend14 == 8192);
    CHECK(s.data().bend_min == 8192);
    CHECK(s.data().bend_max == 8192);

    s.onMessage(bend(0, 2000), 11, 11000);
    s.onMessage(bend(0, 15000), 12, 12000);
    CHECK(s.data().bend14 == 15000);
    CHECK(s.data().bend_min == 2000);
    CHECK(s.data().bend_max == 15000);

    printf("  bend tracking ok\n");
}

// --- Controllers -------------------------------------------------------------

void test_cc_slots()
{
    MonitorState s;

    s.onMessage(cc(0, 74, 10), 10, 10000);
    s.onMessage(cc(0, 11, 20), 11, 11000);
    CHECK(s.data().ccs[0].used);
    CHECK(s.data().ccs[0].number == 74);
    CHECK(s.data().ccs[0].value == 10);
    CHECK(s.data().ccs[1].number == 11);

    // Same controller again updates in place rather than taking a second slot.
    s.onMessage(cc(0, 74, 55), 12, 12000);
    CHECK(s.data().ccs[0].number == 74);
    CHECK(s.data().ccs[0].value == 55);
    CHECK(s.data().ccs[0].updates == 2);
    CHECK(!s.data().ccs[2].used);

    printf("  cc slots ok\n");
}

void test_cc_slot_recycling()
{
    // A device sending more controllers than there are slots should show its
    // current ones, not the first five it ever sent.
    MonitorState s;

    const uint8_t numbers[kMonitorCcSlots] = {20, 21, 22, 23, 24};
    for (uint8_t i = 0; i < kMonitorCcSlots; ++i)
    {
        s.onMessage(cc(0, numbers[i], static_cast<uint8_t>(i)), 100 + i, 0);
    }
    for (uint8_t i = 0; i < kMonitorCcSlots; ++i) CHECK(s.data().ccs[i].used);

    // Refresh everything except slot 0, making it the least recently updated.
    for (uint8_t i = 1; i < kMonitorCcSlots; ++i)
    {
        s.onMessage(cc(0, numbers[i], 99), 200 + i, 0);
    }

    s.onMessage(cc(0, 90, 7), 300, 0);

    CHECK(s.data().ccs[0].number == 90);
    CHECK(s.data().ccs[0].value == 7);
    CHECK(s.data().ccs[0].updates == 1);

    // And the others are untouched.
    for (uint8_t i = 1; i < kMonitorCcSlots; ++i) CHECK(s.data().ccs[i].number == numbers[i]);

    printf("  cc slot recycling ok\n");
}

// --- Traffic -----------------------------------------------------------------

void test_channel_mask()
{
    MonitorState s;

    s.onMessage(noteOn(0, 60, 100), 10, 0);
    s.onMessage(cc(9, 74, 20), 11, 0);
    s.onMessage(bend(15, 8192), 12, 0);

    CHECK(s.data().channels_seen == ((1u << 0) | (1u << 9) | (1u << 15)));

    // System messages carry no channel and must not set a bit -- kChannelNone
    // is 0xFF, which would shift out of range if it were used blindly.
    MidiMessage rt{};
    rt.type    = MidiType::SystemRealtime;
    rt.channel = MidiMessage::kChannelNone;
    s.onMessage(rt, 13, 0);

    CHECK(s.data().channels_seen == ((1u << 0) | (1u << 9) | (1u << 15)));
    CHECK(s.data().realtime_messages == 1);

    printf("  channel mask ok\n");
}

void test_message_rate_window()
{
    MonitorState s;

    s.tick(1000);  // Opens the window.
    for (int i = 0; i < 250; ++i) s.onMessage(cc(0, 2, 64), 1000, 0);

    s.tick(1500);
    CHECK(s.data().messages_per_sec == 0);  // Window not yet elapsed.

    s.tick(2000);
    CHECK(s.data().messages_per_sec == 250);
    CHECK(s.data().messages == 250);

    printf("  message rate window ok\n");
}

void test_queue_health()
{
    MonitorState s;

    s.onDrain(4, 64, 4);
    CHECK(s.data().queue_hwm == 4);
    CHECK(s.data().queue_capacity == 64);
    CHECK(s.data().queue_near_full == 0);
    CHECK(s.data().drain_max_batch == 4);

    s.onDrain(2, 64, 2);
    CHECK(s.data().queue_hwm == 4);  // High-water, not current.

    // capacity - 1 is the closest thing to an overrun warning available: the
    // transport discards silently once full and reports nothing.
    s.onDrain(63, 64, 63);
    CHECK(s.data().queue_hwm == 63);
    CHECK(s.data().queue_near_full == 1);
    CHECK(s.data().drain_max_batch == 63);

    printf("  queue health ok\n");
}

void test_unparsed_counted()
{
    MonitorState s;

    s.onUnparsed();
    s.onUnparsed();
    CHECK(s.data().messages_unparsed == 2);
    CHECK(s.data().messages == 0);  // Counted separately, not as traffic.

    printf("  unparsed counted ok\n");
}

// --- Connection lifecycle ----------------------------------------------------

void test_usb_lifecycle()
{
    MonitorState s;

    s.onUsbDevicePresent(0x1234, 0x5678);
    CHECK(s.data().usb_device_present);
    // Present is not ready: the device enumerated, but nothing has claimed a
    // MIDI IN endpoint yet (plan section 2.2).
    CHECK(!s.data().usb_endpoint_ready);
    CHECK(s.data().usb_vid == 0x1234);
    CHECK(s.data().usb_connect_count == 1);

    s.onUsbEndpointReady(0x81, 64, 1);
    CHECK(s.data().usb_endpoint_ready);
    CHECK(s.data().usb_endpoint_addr == 0x81);
    CHECK(s.data().usb_interval_ms == 1);

    s.onUsbDisconnected();
    CHECK(!s.data().usb_device_present);
    CHECK(!s.data().usb_endpoint_ready);
    CHECK(s.data().usb_disconnect_count == 1);

    printf("  usb lifecycle ok\n");
}

void test_disconnect_releases_held_notes()
{
    // Unplugging mid-phrase must not leave a note latched forever -- the
    // hot-replug exit criterion would show it as a permanently stuck note.
    MonitorState s;

    s.onUsbDevicePresent(1, 2);
    s.onUsbEndpointReady(0x81, 64, 1);
    s.onMessage(noteOn(0, 60, 100), 10, 0);
    s.onMessage(noteOn(0, 67, 100), 11, 0);
    CHECK(s.data().active_note_count == 2);

    s.onUsbDisconnected();
    CHECK(s.data().active_note_count == 0);
    CHECK(s.data().lowest_active_note == 0xFF);

    // And the count still works after a replug.
    s.onUsbDevicePresent(1, 2);
    s.onUsbEndpointReady(0x81, 64, 1);
    s.onMessage(noteOn(0, 62, 100), 20, 0);
    CHECK(s.data().active_note_count == 1);

    printf("  disconnect releases held notes ok\n");
}

// --- Snapshot ----------------------------------------------------------------

void test_copy_is_a_real_copy()
{
    // The UI task renders from a copy taken under lock; if that copy aliased
    // the live state the lock would be pointless.
    MonitorState s;
    s.onMessage(cc(0, 2, 42), 10, 0);

    MonitorSnapshot snap;
    s.copyTo(snap);
    CHECK(snap.breath == 42);

    s.onMessage(cc(0, 2, 99), 11, 0);
    CHECK(snap.breath == 42);
    CHECK(s.data().breath == 99);

    printf("  snapshot copy ok\n");
}

void test_reset_clears_everything()
{
    MonitorState s;

    s.onUsbDevicePresent(1, 2);
    s.onMessage(noteOn(0, 60, 100), 10, 0);
    s.onMessage(cc(0, 74, 30), 11, 0);
    s.onDrain(50, 64, 50);

    s.reset();

    CHECK(s.data().messages == 0);
    CHECK(s.data().active_note_count == 0);
    CHECK(s.data().lowest_active_note == 0xFF);
    CHECK(s.data().bend14 == 8192);
    CHECK(s.data().queue_hwm == 0);
    CHECK(s.data().breath_per_sec == 0);
    CHECK(s.data().breath_min_interval_us == 0xFFFFFFFFUL);
    CHECK(!s.data().usb_device_present);
    CHECK(!s.data().ccs[0].used);

    printf("  reset ok\n");
}

}  // namespace

int main()
{
    printf("monitor_state_test\n");

    test_initial_state();
    test_note_counting();
    test_duplicate_note_on_counted_once();
    test_unmatched_note_off_does_not_underflow();
    test_note_on_velocity_zero_through_parser();
    test_breath_tracking();
    test_breath_same_batch_timestamp_ignored();
    test_breath_rate_is_timestamp_independent();
    test_bend_tracking();
    test_cc_slots();
    test_cc_slot_recycling();
    test_channel_mask();
    test_message_rate_window();
    test_queue_health();
    test_unparsed_counted();
    test_usb_lifecycle();
    test_disconnect_releases_held_notes();
    test_copy_is_a_real_copy();
    test_reset_clears_everything();

    printf("monitor_state_test: %d checks passed\n", g_checks);
    return 0;
}
