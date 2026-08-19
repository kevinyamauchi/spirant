#pragma once

#include <stdint.h>

// --- Parsed MIDI message -----------------------------------------------------
//
// The decoded form of one USB-MIDI event packet (plan section 2.3). Deliberately
// free of Arduino.h and of the ESP32_Host_MIDI headers, so tests/ can compile
// the parser with the system compiler -- same arrangement as src/params/.
//
// This is NOT the library's MIDIEventData. See usb_midi_host.h for why we parse
// the transport's raw packets ourselves rather than going through MIDIHandler.

enum class MidiType : uint8_t
{
    None = 0,         ///< Nothing decoded; the packet carried no channel message.
    NoteOff,
    NoteOn,           ///< Never with velocity 0 -- see parseUsbMidiPacket().
    PolyPressure,
    ControlChange,
    ProgramChange,
    ChannelPressure,
    PitchBend,
    SystemCommon,     ///< MTC quarter frame, song position, song select.
    SystemRealtime,   ///< Clock, start, stop, continue, active sensing, reset.
};

/// One decoded channel-voice or system message.
///
/// `data1`/`data2` keep their raw 7-bit values; the accessors below give the
/// composed forms. For system messages `channel` is kChannelNone.
struct MidiMessage
{
    static const uint8_t kChannelNone = 0xFF;

    MidiType type;
    uint8_t  channel;  ///< 0-15, or kChannelNone for system messages.
    uint8_t  data1;    ///< Note number / controller number / bend LSB.
    uint8_t  data2;    ///< Velocity / controller value / bend MSB.
    uint8_t  status;   ///< The raw status byte, as it appeared on the wire.
    uint8_t  cable;    ///< USB-MIDI virtual cable number (packet byte 0, bits 7:4).

    /// Pitch bend as the raw 14-bit value, 0..16383, centre 8192.
    uint16_t bend14() const
    {
        return static_cast<uint16_t>(data1 | (static_cast<uint16_t>(data2) << 7));
    }

    /// Pitch bend relative to centre, -8192..8191.
    int16_t bendSigned() const
    {
        return static_cast<int16_t>(static_cast<int32_t>(bend14()) - 8192);
    }

    bool isChannelMessage() const { return channel != kChannelNone; }
};
