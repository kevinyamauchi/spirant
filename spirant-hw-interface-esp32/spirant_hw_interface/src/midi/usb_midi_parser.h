#pragma once

#include <stdint.h>

#include "midi_types.h"

// --- USB-MIDI 1.0 event packet parser ----------------------------------------
//
// Decodes the 4-byte USB-MIDI event packets that USBConnection hands us
// (plan section 2.3). Arduino-free; host-tested in tests/usb_midi_parser_test.cpp.
//
// Packet layout, USB Device Class Definition for MIDI Devices 1.0, section 4:
//
//   byte 0 : bits 7:4 = cable number, bits 3:0 = Code Index Number (CIN)
//   byte 1 : MIDI status byte (for channel messages)
//   byte 2 : first data byte
//   byte 3 : second data byte
//
// Two consequences of this framing that matter to the rest of the project:
//
//  1. **Running status cannot appear here.** Every channel-message packet
//     carries its own status byte in byte 1, because the CIN has to agree with
//     it. Plan section 2.5 asks whether the EWI "uses running status on the
//     wire" -- on USB the answer is structurally no. The question is still live
//     for phase 3's TRS output, which is a real MIDI wire.
//
//  2. **Packets are fixed-width.** A message never straddles two packets
//     (except SysEx, which USBConnection reassembles before we see it), so the
//     parser is stateless. That is what makes it trivially host-testable.

/// Decode one 4-byte packet.
///
/// Returns true and fills `out` for channel-voice, system-common and
/// system-realtime messages. Returns false -- leaving `out` untouched -- for
/// packets carrying nothing we model: SysEx fragments (CIN 0x4-0x7, already
/// handled by the transport's reassembly), cable events, and reserved CINs.
///
/// A false return is not an error. Count them separately from parse failures:
/// a rising count of unmodelled packets is information about the device, not a
/// bug. See MonitorState::messagesUnparsed().
bool parseUsbMidiPacket(const uint8_t packet[4], MidiMessage& out);

/// Short human-readable name for a decoded type, for logs and the monitor page.
/// Never returns nullptr.
const char* midiTypeName(MidiType type);
