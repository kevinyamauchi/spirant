#pragma once

#include <stdint.h>

// --- USB MIDI host front end (plan sections 2.2 and 2.3) ---------------------
//
// Wraps ESP32_Host_MIDI's USBConnection transport. Arduino- and library-facing;
// the parsing and the state it feeds are in usb_midi_parser.* and
// monitor_state.*, which stay host-testable.
//
// ## Why this bypasses MIDIHandler
//
// The library's documented path is `midiHandler.addTransport(&usb)` then
// `midiHandler.task()` and iterate `getQueue()`. We register our own callback on
// the transport instead and never construct that path. Three reasons, in order
// of weight:
//
//  1. **It would add a second queue on the hot path.** Design doc section 5 and
//     plan section 2.3 both require the transport ring to be the *sole*
//     cross-core boundary. MIDIHandler is a second buffer downstream of it,
//     capped at 100 events (MIDIHandlerConfig::maxEvents, default 20) -- below
//     the transport ring's own 64, so it becomes the binding constraint while
//     being the one we did not design around.
//
//  2. **Its event type allocates.** MIDIEventData carries four std::strings
//     ("NoteOn", "C", "C4", ...) inside a std::deque. At the breath rates plan
//     section 2.5 expects, that is thousands of heap allocations per second on
//     the realtime path, for strings a monitor page does not need.
//
//  3. **We want the parse host-tested.** Our parser compiles with the system
//     compiler and has tests; the library's does not and cannot.
//
// USBConnection on its own is exactly the piece worth having: it owns the
// USB-host stack, the core-0 polling task and the 64-entry ring buffer.
//
// ## What is not observable, and why it matters
//
// USBConnection::enqueueMidiMessage() **discards silently** when its ring is
// full -- it returns false and its only caller ignores that. So a true overrun
// counter cannot be built from outside the library. MonitorState instead tracks
// the ring's high-water mark and counts how often it came within one slot of
// full, which warns before data is lost. If the exit criteria ever need a hard
// overrun count, that needs a patch to the library, not to this file.

namespace usb_midi
{

/// Starts the USB host stack and the library's core-0 polling task, and wires
/// our callbacks. Returns false if the host stack would not start -- check it,
/// the most likely cause is the board still being in device mode (USB Mode must
/// be "USB-OTG (TinyUSB)" and USB CDC On Boot "Disabled").
bool begin();

/// Drain the transport ring, parse everything in it, and fold the batch into
/// the monitor snapshot under a single lock. Call from the drain loop.
///
/// Returns the number of packets consumed.
uint16_t drain(uint32_t now_ms, uint32_t now_us);

/// True once a MIDI IN endpoint has been claimed and a transfer allocated --
/// not merely once a device appeared. Plan section 2.2's trap, and the library
/// reproduces it: its own dispatchConnected() fires after _processConfig()
/// whether or not that found a usable endpoint.
bool endpointReady();

/// Current depth of the transport's ring buffer, and its fixed capacity.
uint16_t queueDepth();
uint16_t queueCapacity();

/// Packets the staging buffer had to drop because a single drain pass produced
/// more than it holds. Should stay zero; a non-zero value means the drain
/// period is too long for the traffic.
uint32_t stagingOverflows();

}  // namespace usb_midi
