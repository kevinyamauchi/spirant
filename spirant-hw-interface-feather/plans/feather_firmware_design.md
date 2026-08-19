# Spirant — Adafruit Feather RP2040 USB Host Firmware Design

Companion to [`plans/esp32_firmware_design.md`](../../plans/esp32_firmware_design.md).
Section numbering is deliberately parallel so the two documents diff cleanly;
where a section reaches the same conclusion by a different route, that is said
explicitly rather than restated.

**Status:** design only. Nothing here has been built.

Two marker conventions are used throughout:

- ***(measure)*** — a number derived on paper. Treat it as an order of magnitude
  and confirm it on hardware before anything depends on it.
- ***(verify)*** — a claim about a library, core or board that has not been
  checked against the actual toolchain.

Unmarked numbers are either measured figures quoted from the ESP32 firmware or
exact arithmetic from a datasheet.

---

## 0. Why this port, and what changes

The Feather RP2040 USB Host replaces the LilyGO T-Display S3 in the same role:
USB MIDI host bridge plus UI controller. The Daisy Pod side is unchanged, the
TRS wire is unchanged, and the GPIO handshake is unchanged.

Three things change materially:

| | T-Display S3 | Feather RP2040 USB Host |
|---|---|---|
| USB host | Hardware OTG peripheral | **PIO-driven** (pico-pio-usb) — §5.1 |
| Concurrency | 3 FreeRTOS tasks, 2 cores, preemptive priorities | **2 bare-metal cores, cooperative, no priorities** |
| Display | ST7789 over 8-bit parallel, on-board | ST7789 over SPI, external breakout |

The second row is the important one. The ESP32 design puts `loop()` (the MIDI
path) and the UI task on the *same* core and relies on priority and yield
discipline to keep them apart — esp32 §7.2 concedes they "can preempt each
other," and §7.4 names display redraw as "the least-bounded part of this
system." On the RP2040 the UI moves to a physically separate core, so that
entire risk class is **eliminated structurally rather than tuned**. Section 7
below is where most of the design effort went, and it is the section that
diverges most from its ESP32 counterpart.

The first row is the one real regression, and Section 5 covers it.

---

## 1. Role & Scope

Unchanged from esp32 §1. Two jobs on one board:

1. **USB MIDI host bridge** — receive notes, breath CC2 and pitch bend from the
   Akai EWI Solo over the USB Type-A port, forward to the Daisy Pod over TRS
   MIDI.
2. **UI controller** — read the 4-encoder Seesaw breakout over I²C, drive a 2.0"
   320×240 SPI TFT, and send parameter changes to the Daisy as CCs in the
   102–119 range.

```
 Akai EWI Solo --USB-A--> [Feather RP2040 USB Host] --TRS MIDI--> [Daisy Pod]
                             |     |            ^                      |
                    I2C/QT   |     | SPI        +--- GPIO handshake ----+
                             v     v                (DAISY_READY, DUMP_ACK)
                 [Seesaw quad encoder]  [2.0" ST7789 320x240]
```

TRS is one-way, Feather → Daisy. There is no MIDI IN on this link, so `Serial1`
has exactly one owner (Section 7).

---

## 2. Boot Sequence

The two-line GPIO handshake from esp32 §2 ports unchanged: `DAISY_READY` and
`DUMP_ACK`, both Daisy → Feather, open-drain with pull-ups, both debounced a few
ms. The Daisy state machine (esp32 §2.1) is untouched. The Feather state machine
is esp32 §2.2 verbatim.

Four notes specific to this board:

### 2.1 The dump crosses cores

The boot dump has to *send* parameter CCs, but `Serial1` is owned by core 1
while the boot state machine runs on core 0. This is free with the Section 7.4
design: core 0 calls `ParameterValues::markAllChangedMidi()` — a method that
already exists — then feeds the resulting changes to core 1 over Channel B.
Retry after an `ACK_TIMEOUT` is just another `markAllChangedMidi()`.

Cost, under running status (§6.3): the first CC carries its status byte (3
bytes, 960 µs) and the rest do not (2 bytes, 640 µs each). For nineteen
parameters that is 960 + 18 × 640 ≈ **12.5 ms** *(measure)* — comfortably inside
the 250 ms `ACK_TIMEOUT`, with two orders of margin.

**The parameter count is unresolved.** Nineteen is `kNumActiveParams`, but
esp32 §2.2 says the Daisy expects sixteen and CC 102–119 provides only
eighteen — see §10 #3. The dump as described **cannot be sent** until that is
settled: nineteen parameters do not fit eighteen controller numbers. The timing
above is insensitive to which number wins.

### 2.2 Core 1 must be up first

Core 0 must not begin the handshake until core 1's USB host and UART are live.
The `core0_booting` / `core1_booting` barrier pattern from the Adafruit
`EZ_USB_MIDI_HOST` example handles this — both `setup()` and `setup1()` clear
their own flag and spin on the other's.

### 2.3 Patch ownership

Unchanged from esp32 §2.3: the Feather is the source of truth at boot and loads
a compiled-in default patch. See Section 9.4 for why SD-card patch storage is
now a *better* v2 option here than it was on the ESP32.

### 2.4 Pin assignment — resolved

esp32 §2.5 left `DAISY_READY` / `DUMP_ACK` pins TBD because the T-Display S3
exposes only 13 usable header GPIOs. The Feather exposes roughly 18 and the
handshake lines are assigned in Section 8. **This open item is closed.**

---

## 3. Display Communication

**Hardware:** [Adafruit 2.0" 320×240 Color IPS TFT with microSD
breakout](https://learn.adafruit.com/2-0-inch-320-x-240-color-ips-tft-display) —
ST7789 controller, 4-wire SPI, 3–5 V level-shifted, microSD on the same SPI bus.

The controller family is the same as the T-Display S3's. Only the bus changes,
parallel → SPI, and the panel gets **larger**: 320×240 versus 320×170. esp32
§3's content spec (page name, four parameter values with labels, connection and
handshake status) is comfortably achievable — more so than on the original
board.

### 3.1 The I²C contention question, answered

esp32 §3 treats "the display is not on I²C" as a deliberate win over the Pico 2
design, where an SSD1306 OLED shared a bus with the encoder board and needed
careful mutex discipline.

An I²C OLED on the Feather would have reintroduced exactly that problem, because
**the Feather's header SDA/SCL and its STEMMA QT connector are the same bus**
(GP2/GP3, Section 8) — an OLED FeatherWing and a Seesaw on STEMMA QT would land
on one wire. Choosing SPI for the display avoids it entirely: different
peripheral, different pins, and the encoder I²C bus keeps exactly one consumer,
preserving esp32 §4's "no mutex required" property verbatim.

### 3.2 Redraw strategy

esp32 §3 mandates partial redraws only. **That still holds, but the reason
changes.** On the ESP32 it was to protect the breath stream (esp32 §7.4). Here
the display runs on a core that carries no MIDI, so full-frame cost is invisible
to MIDI timing and partial redraws are purely about UI responsiveness.

Frame arithmetic, 320×240 at 16 bpp = **153,600 bytes**:

| SPI clock | Full frame | 320×30 scratch region (19,200 B) |
|---|---|---|
| 30 MHz | ~41 ms | ~5.1 ms |
| 60 MHz | ~20 ms | ~2.6 ms |

*(measure)* — these are wire-time floors and ignore per-transaction overhead.

**Which clocks are actually reachable.** The RP2040's SPI divides `clk_peri` by
an *even* prescaler times an integer, and `clk_peri` follows `clk_sys`. At the
120 MHz `clk_sys` this design defaults to (§5.1), that yields **60 / 30 / 20 /
15 MHz** — and notably **not 24 MHz**, which is `Adafruit_ST7789`'s default and
what LovyanGFX will silently round away from. Set the clock explicitly. Start at
30 MHz and test upward; the figures above bracket the useful range.

For comparison, the ESP32's parallel bus measured **13.9 MB/s** (19,520 bytes in
~1,420 µs, recorded in `display_manager.h`). SPI at 30/60 MHz gives 3.75/7.5
MB/s, so **the Feather's display bus is 2–4× slower**. Every timing constant in
the ported `DisplayManager` — notably the `kScratchH = 30` sizing guidance and
the `worstValueUs` / `worstPageUs` budgets — must be re-derived, not copied.

The consequence that *is* new and pleasant: a full repaint on page change is now
free of MIDI risk. It costs UI latency only, so page switching can be as
expensive as it needs to be.

### 3.3 The full-screen sprite does not port

The ESP32 firmware allocates LovyanGFX sprites totalling roughly 45–50 KB out of
the S3's 512 KB. A *full-screen* 320×240 framebuffer would be 153.6 KB of the
RP2040's **264 KB total SRAM** — technically possible, but it would dominate the
memory budget and buy nothing, since partial redraws are the strategy regardless.

**Keep the sprite-bank design, keep the sprite sizes.** `DisplayManager`'s
existing 320×30 scratch (19,200 B) and 80×122 column (19,520 B) are unchanged in
cost and fit the RP2040 comfortably. Do not scale them up to match the taller
panel.

### 3.4 Library

Stay on **LovyanGFX**. It supports RP2040 *(verify)*, including SPI DMA, which
means `DisplayManager`'s `pushScratch()` / `waitDMA()` discipline ports as-is.
The only new file is an `LGFX_Device` subclass configuring an SPI bus and
ST7789 panel in place of
[`lgfx_t_display_s3.h`](../../spirant-hw-interface-esp32/spirant_hw_interface/src/display/lgfx_t_display_s3.h)'s
I8080 bus — see Section 9.2.

Switching to Adafruit_GFX (as the breakout's own guide uses) would mean
rewriting `display_manager.cpp`, `param_screen.cpp` and `monitor_screen.cpp`,
about 780 lines, for no gain. Don't.

### 3.5 Geometry

`theme.h` holds all geometry constants, so the panel change is a constants edit:

```
 y 0   +--------------------------------------+
       |  title                         note  |  header   28
 y 28  +--------------------------------------+
       |                                      |
       |             screen body              |  body    196   (was 123)
       |                                      |
 y 224 +--------------------------------------+
       | status line                          |  footer   16
 y 240 +--------------------------------------+
```

(28 + 196 + 16 = 240. The ESP32 original's own figures do not close — 29 + 123 +
16 = 168, not 170 — so port the *structure* from `theme.h`, not the constants.)

The extra 73 px of body is real estate the ESP32 layout did not have. Whether
the parameter page uses it for taller cells, a fifth row of detail, or more
monitor scrollback is a UI decision, not a firmware-architecture one.

### 3.6 Ownership

Unchanged from esp32 §3: display writes happen only from the UI context — which
on this board means **core 0 only** — never from an ISR and never from core 1.

---

## 4. I²C Encoder Board Communication

Hardware and driver are unchanged: Adafruit Quad Rotary Encoder Breakout
(Seesaw, product 5752) at `0x49`, driven by `Adafruit_Seesaw`. esp32 §4.1's
recommendation to use the stock library rather than reimplementing the register
protocol stands without modification, and for the same reasons.

Three things get *simpler*.

### 4.1 Plug-and-play bus, no pin decision

esp32 §4 and `config.h` record a drawn-out pin investigation: the T-Display S3
unit's STEMMA QT turned out to be on GPIO 43/44, which are also UART0, forcing
the log UART elsewhere and making `Core Debug Level = None` load-bearing.

On the Feather the STEMMA QT is on GP2/GP3, it is the board's primary I²C, and
nothing else contends for it. Plug the Seesaw in with a QT cable. **No pin
decision, no bootloader-chatter-on-SDA hazard, no debug-level constraint.**

`I2C_TIMEOUT_MS` should stay generous per esp32 §4 — a wedged Seesaw must not
hang the UI — though on this board a hung UI can no longer stall MIDI.

### 4.2 The level-triggered ISR can be deleted

esp32 §4 works around Arduino's edge-only `attachInterrupt()` by dropping to
ESP-IDF's `gpio_set_intr_type(..., GPIO_INTR_LOW_LEVEL)`, because the Seesaw INT
is level-triggered and an edge-only ISR risks missing a second change that
arrives while the first is being serviced.

The pico SDK *does* support `GPIO_IRQ_LEVEL_LOW`, so that workaround would port
directly. **But it should not be ported at all.** Core 0 now has nothing to do
but UI. Poll the INT pin in core 0's loop at ~1 kHz and read the encoders when
it is asserted: no ISR, no level-versus-edge subtlety, no re-arm discipline, no
missed-second-change failure mode.

The entire complexity of esp32 §4's ISR/task integration exists because the UI
shared a core with the MIDI path. It no longer does. `ENCODER_USE_INTERRUPT`
becomes a dead flag and
[`encoder_input.cpp`](../../spirant-hw-interface-esp32/spirant_hw_interface/src/encoders/encoder_input.cpp)
loses its ISR half.

Keep calling the Seesaw's own `enableEncoderInterrupt()` — that is what makes
INT assert. Only the host-side consumption changes, from ISR to poll.

### 4.3 Transaction cost stops mattering

`Adafruit_Seesaw::read()` does write / `delayMicroseconds(250)` / read. A full
four-encoder sweep is roughly **1–2 ms** of blocking. On the ESP32 that was a
jitter risk that esp32 §7.1 cites as a reason for the separate UI task. Here it
is invisible to MIDI.

---

## 5. MIDI Host Over USB Type-A

Built on [`EZ_USB_MIDI_HOST`](https://github.com/rppicomidi/EZ_USB_MIDI_HOST)
over `pico-pio-usb` and Adafruit TinyUSB, replacing `ESP32_Host_MIDI`.

### 5.1 The regression: PIO-driven USB

esp32 §7's threading table describes the USB task as "hardware-interrupt-driven,
minimal CPU at idle." **That is not true here.** pico-pio-usb drives full-speed
USB from PIO state machines with software-generated SOF, where the T-Display S3
had a dedicated OTG peripheral.

**What "bit-banged" does and does not mean.** It does *not* mean the CPU toggles
pins. The wire runs at 12 Mbit/s; a 120 MHz M0+ would have ten cycles per bit and
nothing would work. The **PIO state machines** perform NRZI encoding/decoding,
bit-stuffing and serialisation autonomously in hardware — that is what PIO is
for — and the CPU never sees an individual bit.

CPU cost is therefore **per packet, not per bit**. One interrupt-IN endpoint at
`bInterval = 1` produces roughly two to three packet events per 1 ms frame (SOF,
IN token, DATA-or-NAK): order 2,000–3,000 events per second, not 12 million.
Section 7.6 works the budget through.

Consequences that do bite:

- **CPU clock must be exactly 120 or 240 MHz** — pico-pio-usb needs the PIO
  divider to land on USB timing exactly, and the example sketch hard-fails at
  boot on anything else. This firmware should too.
- **240 MHz is out of spec.** The RP2040 was "originally run at 133 MHz, but
  later certified at 200 MHz"
  ([Wikipedia](https://en.wikipedia.org/wiki/RP2040)), so 240 MHz is a ~20%
  overclock past the certified maximum. Adafruit's own example for this exact
  board sanctions it and it is well-trodden, but it is a deliberate decision
  with a stability and thermal check attached — **not** the first lever to reach
  for if core 1 looks tight (§7.6). **Default to 120 MHz.**
- Core 1 is genuinely occupied, not idle-polling.
- **Core 1's foreground must never block.** The timing-critical work is
  ISR-driven, so brief foreground blocking survives, but sustained blocking
  backs the host stack up. See §7.6 for the enumerated hazards.
- Enumeration is less robust than a hardware OTG peripheral. **Budget bring-up
  time to test the EWI Solo specifically, early**, before any UI work depends on
  it.

Not a problem: PIO resource contention. The RP2040 has eight state machines
across two PIO blocks; pico-pio-usb host claims about three of one block, and
nothing else in this design wants PIO (the encoders are on I²C via the Seesaw).

### 5.2 MIDI 2.0 support is dropped

esp32 §5 negotiates MIDI 2.0/UMP with a MIDI 1.0 fallback, via
`USBMIDI2Connection`. `EZ_USB_MIDI_HOST` is **MIDI 1.0 only** — it wraps
`usb_midi_host` into Arduino MIDI Library byte streams and has no UMP path.

esp32 §5 already expects the EWI Solo to enumerate as MIDI 1.0, so this costs
nothing today. Recording it as a deliberate scope reduction rather than an
accident.

### 5.3 Where parsing happens — and an open decision

The library's data path is: `tuh_midi_rx_cb` fires on core 1 →
[`EZ_USB_MIDI_HOST::onRx`](https://github.com/rppicomidi/EZ_USB_MIDI_HOST/blob/main/EZ_USB_MIDI_HOST.h)
drains `tuh_midi_stream_read()` into a per-cable `tu_fifo` → the application calls
`usbhMIDI.readAll()`, which pops that FIFO one byte at a time through the
Arduino MIDI Library parser and fires callbacks.

**Both halves run on core 1 in this design** (Section 7.1), which makes the
`tu_fifo` a single-core structure and removes it from the cross-core analysis
entirely. That is a simplification over the stock Adafruit example, which splits
the FIFO across cores.

The open decision is what to do with the existing, host-tested
[`usb_midi_parser`](../../spirant-hw-interface-esp32/spirant_hw_interface/src/midi/usb_midi_parser.h):

- **Option A — use EZ's Arduino MIDI Library callbacks, retire the parser.**
  Idiomatic for the library. Loses `usb_midi_parser_test.cpp`. Requires an
  adapter from the library's callbacks to `MidiMessage`.
- **Option B — drop to `usb_midi_host` directly** (`tuh_midi_packet_read()`),
  which yields exactly the 4-byte USB-MIDI event packets `parseUsbMidiPacket()`
  already consumes. The tested parser ports verbatim; no packet → byte-stream →
  re-parse round trip; no `tu_fifo` at all. Costs the device/cable lifecycle
  management EZ provides, which would have to be reimplemented or borrowed.

**Lean: Option B**, because it preserves tested code and removes a redundant
conversion. But it needs a short spike to confirm `usb_midi_host` is cleanly
usable standalone under Adafruit TinyUSB on this board *(verify)*. If the spike
is awkward, Option A is a perfectly good fallback — `midi_types.h`'s
`MidiMessage` stays the internal currency either way.

### 5.4 The USB console survives

Worth calling out because it is a real workflow improvement. The ESP32 firmware
loses its CDC console the moment `usb_host_install()` takes the OTG PHY — the
`spirant-hw-interface-esp32` README opens with a warning about this, and
`config.h` documents the out-of-band UART log built to work around it.

On the Feather, **host mode is on separate PIO pins (GP16/17) and the native
USB-C device port is untouched.** `Serial` over USB CDC works from boot through
the whole session, including during enumeration. The dual-sink
[`log.h`](../../spirant-hw-interface-esp32/spirant_hw_interface/src/log.h)
design can collapse to the USB sink alone, and `USB_MIDI_LOG_ENUMERATION` can
finally log a descriptor dump to a console that is still listening.

One constraint: **log from core 0 only.** `Serial` CDC writes can block when the
host is not draining, and core 1 must never block (Section 5.1).

---

## 6. MIDI Output Over Mini TRS

`Serial1` at 31250 baud, TX-only on GP0. RX is unused, freeing GP1.

### 6.1 Single owner

**Core 1 owns `Serial1` exclusively.** Nothing on core 0 ever touches it. Both
logical sources from esp32 §6 converge on core 1:

1. **Performance data** — parsed on core 1 from the USB host, forwarded inline.
   Never crosses a core boundary at all.
2. **Parameter data** — CC 102–119, produced on core 0, delivered to core 1 via
   the Section 7.4 channel.

### 6.2 Output priority — esp32 §6.1, achieved structurally

esp32 §6.1 specifies a single outgoing queue with performance messages
prioritised ahead of pending parameter CCs. On this architecture that policy
needs no queue and no scheduler. Core 1's loop is:

```
loop1():
    USBHost.task()                        # service the host stack
    drain parsed MIDI                     # performance events
        -> TX serializer, inline          # unconditional, highest priority
        -> push to Channel A ring         # non-blocking, for the display
    drain Channel B ring -> pending param CC slots
    if Serial1.availableForWrite() > kReserveBytes:
        emit at most one pending param CC
```

Performance data is prioritised by construction. The `kReserveBytes` threshold
keeps TX FIFO headroom permanently available for the breath stream, so a burst
of knob turns can never delay it.

**`Serial1.write()` must never be allowed to block.** Gate every write on
`availableForWrite()`; never spin waiting. Raise the TX buffer with
`Serial1.setFIFOSize()` before `begin()` — the 32-byte default is far too small.
This is a **hard invariant, not hygiene**: a full TX FIFO blocks for up to 320 µs
*per byte*, and §7.6 identifies it as core 1's single largest blocking hazard —
the one most likely to disturb USB host timing.

### 6.3 Running status

esp32 §6.4 recommends running-status compression. It pays more than that section
assumes, because **all output is on a single MIDI channel** (settled): breath
CC2 and parameter CCs 102–119 are all status `0xBn` on the same channel, so the
status byte survives the interleave between the two sources. Every controller
message is 2 bytes, not 3. Only note on/off and pitch bend break the run.
(System Real Time bytes do not clear running status; System Common does.)

Bandwidth at the assumed 2 ms breath rate, 320 µs/byte:

| | Per message | Duty cycle |
|---|---|---|
| 3 bytes, no running status | 960 µs | ~48% |
| 2 bytes, running status | 640 µs | ~32% |

**Implementation note.** The Arduino MIDI Library's
`DefaultSettings::UseRunningStatus` is `false`, disabled specifically because it
is incompatible with USB MIDI — on a TRS wire it is exactly right, so it would
need a settings subclass for the UART instance. But the Section 6.2 headroom
check needs an exact byte count *before* writing, which `send()` does not
expose. **Recommend a small hand-rolled TX serializer on core 1** that owns
running-status state and byte accounting directly. It is perhaps 40 lines and it
is the one place in the system where exact control pays for itself.

### 6.4 Encoder CC throttling — mostly free

esp32 §6.2 rate-limits encoder CCs to ~15–20 Hz per encoder and sends only on
actual value change. The Section 7.4 channel gives both properties for free:
`ParameterValues` coalesces per slot (latest value wins until taken) and the
`changed_midi` flag is only set on a real change. An explicit per-CC minimum
interval is still worth having as a backstop, but it is no longer load-bearing.

### 6.5 Resolution — esp32 §6.3 needs correcting

esp32 §6.3 claims a 7-bit CC has more resolution than the old 0–100 range, so
"no precision is lost." **That is true for the 0–100 parameters and false for at
least one other.** Checking `kParamSpecs` against `(max - min) / step + 1`:

| Parameter | Range | Step | Distinct values | Fits 0–127? |
|---|---|---|---|---|
| Cutoff Floor | 20–500 Hz | 5 | 97 | yes |
| LFO Rate | 10–500 cHz | 5 | 99 | yes |
| Damping | 0–497 | 5 | 100 | yes |
| Damp LP | 1000–18000 Hz | 250 | 69 | yes |
| **Delay Time** | **40–750 ms** | **5** | **143** | **no** |

Delay Time has 143 reachable positions and does not fit in a 7-bit CC. Options:
coarsen its step to 10 (72 values), narrow the range, or use a 14-bit CC pair
for that one parameter. See Section 10.

### 6.6 Saturation policy

At the assumed 2 ms breath rate the wire runs at 32–48% duty (§6.3) and nothing
needs dropping. But §7.7 #2 concedes the real rate may be nearer 500 µs, at
which point 31250 baud is oversubscribed. Because core 1 must never block
(§7.6), "wait for room" is not an available answer — **the serializer has to
decide, and the decision belongs in the design rather than in whatever the code
happens to do.**

Three priority classes, highest first:

1. **Never dropped** — Note On, Note Off, Program Change, System Real Time.
   Discrete events whose loss is audible and unrecoverable; a dropped Note Off
   hangs a note indefinitely. These get a small software queue ahead of the UART
   FIFO. Note traffic is sparse relative to 31250 baud, so in practice it never
   fills — but if it does, that increments an instrumented error counter and is
   surfaced on the display. Silent loss here is a bug, not a policy.
2. **Thinned, never queued** — breath CC2 and pitch bend. Continuous
   controllers where only the newest value carries meaning. The serializer holds
   **at most one pending value per controller** and overwrites it in place; a
   superseded value is discarded rather than sent late. Sending stale
   controller values behind a backlog is worse than dropping them, because it
   converts a bandwidth problem into a latency problem.
3. **Dropped last, and self-healing** — parameter CCs 102–119. Already gated
   behind `kReserveBytes` (§6.2), so they only move when the wire is quiet. If
   one is lost, `ParameterValues` on core 0 still holds the truth and will
   re-offer it (§7.4) — this is the only class where loss repairs itself.

Note that pitch bend is three bytes and status `0xEn`, so it *breaks* running
status (§6.3) as well as consuming bandwidth. Thinning it is worth more per
message than thinning CC2.

---

## 7. Threading & Performance

This is the section that diverges most from its ESP32 counterpart.

### 7.1 Two cores, no scheduler

arduino-pico is bare metal. There are exactly two execution contexts — `loop()`
on core 0 and `loop1()` on core 1 — with no preemption and no priorities. The
ESP32's three-context arrangement must collapse to two.

**Do not add FreeRTOS to recover the three-task shape.** It would reintroduce
the scheduling-discipline problem the hardware just solved, and add jitter.

| Context | Core | Owns | Responsibilities |
|---|---|---|---|
| `loop1()` | 1 | USB host, `Serial1` | PIO-USB host task, MIDI parse, TRS output. **Never blocks.** |
| `loop()` | 0 | I²C, SPI, `Serial`, handshake GPIOs | Seesaw poll, `ParameterValues`, display render, boot state machine, logging. **Free to be slow.** |

### 7.2 Core split rationale

The assignment is inverted relative to the ESP32 (where USB was core 0), because
pico-pio-usb should run on the core that started it — `USBHost.begin()` is
called from `setup1()`.

The substantive change is not which core, but **what shares a core with what**.
On the ESP32, `loop()` and the UI task both live on core 1, and esp32 §7.2 is
explicit that "the priority/yield relationship in the table above is what
actually protects the MIDI path, not the core assignment alone."

Here there is no shared core and no priority relationship. A 41 ms full-frame
repaint, a 2 ms Seesaw sweep, a blocking SD write and a stalled USB CDC log call
all happen on core 0 and **cannot** affect MIDI timing. **esp32 §7.4's "known
risk to watch" — unbounded display redraw time creeping into the MIDI path — does
not exist on this board.** It does not need profiling to be kept safe, only to
be kept pleasant.

esp32 §7.1's justification for a separate UI task is answered by hardware
instead of by scheduling.

### 7.3 Core 0 loop structure and cadence

Core 1's loop is specified in §6.2 and budgeted in §7.6. Core 0 needs the same
treatment, because although it cannot affect MIDI timing (§7.2), its loop period
*is* the encoder-to-pixel latency the player feels.

```
loop():                                       # core 0
    poll Seesaw INT                           # every iteration
    if asserted:
        read encoders -> ParameterValues      # 1-2 ms (§4.3)
    takeMidiChanges() -> Channel B ring       # §7.4
    drain Channel A ring -> MonitorState
    poll DAISY_READY / DUMP_ACK, step boot state machine
    if now - lastRender >= kRenderPeriodMs:   # 33 ms, ~30 Hz
        render dirty regions                  # 2.6-5.1 ms each (§3.2)
    service log
```

**Cadence: a fixed ~30 Hz render tick, with everything else polled every
iteration.** Rendering is the expensive step and the only one worth gating on a
timer; the Seesaw poll, the ring drains and the handshake are cheap enough to
run unconditionally.

Worst-case loop period *(measure)*: a render tick touching four regions
(~20 ms) plus a Seesaw sweep (~2 ms) plus the drains — call it **~25 ms**, with a
page-change full repaint spiking to **~45 ms**. Encoder latency is therefore up
to one render period plus one sweep, roughly **35–40 ms** from detent to pixel.
That is acceptable for a parameter readout.

**If it feels sluggish, poll and read encoders more often than you render** —
the Seesaw read is 1–2 ms and the render is what costs. Raising the frame rate
is the wrong lever.

Two things this period does *not* bound:

- **MIDI latency.** Core 1 owns that end to end (§7.2).
- **Parameter CC delivery**, beyond one loop iteration. A knob turn waits at most
  one core 0 period (~25–45 ms) to reach Channel B, after which core 1 drains on
  its own schedule. That sits comfortably inside esp32 §6.2's 15–20 Hz
  (50–66 ms) throttle target, so the cadence is not the binding constraint.

### 7.4 Cross-core channels

Exactly two, both single-producer / single-consumer, both lock-free. RP2040 has
no data cache and both cores see SRAM coherently, so a `__dmb()` before
publishing an index is sufficient — **no spinlocks anywhere in this design**.

**Channel A — core 1 → core 0, MIDI events for display.**
A power-of-two SPSC ring of fixed-size records (`MidiMessage`, or a packed
4-byte form). Lossy is acceptable: core 0 coalesces repeated breath values as it
drains, which is what
[`MonitorState`](../../spirant-hw-interface-esp32/spirant_hw_interface/src/midi/monitor_state.h)
already does. Size it for core 0's worst stall: at a 2 ms breath rate, 256
entries is ~512 ms of buffer against a worst-case core 0 stall in the tens of
milliseconds.

An alternative worth noting: arduino-pico's `rp2040.fifo` wraps the hardware
inter-core FIFO, which is genuinely lock-free in silicon and packs a 3-byte MIDI
message into one `uint32_t`. But it is only **8 entries deep**, which is too
shallow to absorb a core 0 display stall. Use the SRAM ring.

**Channel B — core 0 → core 1, parameter CCs.**
Two pieces, and it matters which does what:

- **`ParameterValues` does the coalescing**, on core 0, and stays entirely on
  core 0. Per-slot `changed_midi` flags with latest-value-wins semantics,
  `takeMidiChanges()` to consume, `markAllChangedMidi()` for the boot dump.
  Sharing the object across cores would mean synchronising a whole struct;
  keeping it single-owner means the module ports **unchanged**.
- **A small SPSC ring is dumb transport.** Core 0 calls `takeMidiChanges()` and
  pushes the resulting `ParameterChange` records; core 1 drains them. It can be
  small precisely *because* the coalescing already happened upstream.

**Sizing is load-bearing, not a tuning knob.** `takeMidiChanges()` *clears*
`changed_midi` when it hands back the records. If the ring were then full, the
push would fail and the change would be gone permanently — `ParameterValues`
would no longer know the slot was dirty, and the Daisy would sit on a stale
value forever. That is precisely the failure this channel exists to prevent.

The fix is structural rather than defensive: **size the ring at ≥ `kTotalSlots`
(24 entries).** Coalescing guarantees at most one pending record per slot, so a
full `takeMidiChanges()` always fits and the overflow branch becomes
unreachable. At roughly 12 bytes per `ParameterChange` that is under 300 bytes —
there is no reason to economise here.

(The alternative — query free space before taking, and take at most that many —
gives the same guarantee with more code and a failure mode that has to be tested
rather than reasoned away. Prefer the sizing.)

### 7.5 Shared-resource discipline

Compared to esp32 §7.3, this list gets shorter:

- **`Serial1`** — core 1 only.
- **`Wire` (Seesaw)** — core 0 only, one consumer, no mutex (esp32 §4 property,
  preserved).
- **SPI (TFT, and SD in v2)** — core 0 only. Two consumers on one bus but one
  thread, so serialisation not synchronisation.
- **`Serial` (USB CDC log)** — core 0 only. Core 1 must not log.
- **`ParameterValues`, `MonitorState`** — core 0 only.
- **The two SPSC rings** — the only cross-core boundary in the system. `__dmb()`
  before index publication. No locks.

**`monitor_access.*` is deleted.** Its 121 lines exist solely to mutex
`MonitorState` between the ESP32's drain and UI task. With `MonitorState` owned
by core 0 alone, there is nothing to protect and `MONITOR_LOCK_TIMEOUT_MS` goes
away.

### 7.6 Core 1 budget, and why load is the wrong thing to worry about

"Is a 120 MHz M0+ enough to run a USB host *and* the MIDI path?" is the obvious
question. It splits into two, with very different answers.

**Our half is negligible, and provably so.** Per inbound MIDI message core 1
does: a 4-byte packet read (memcpy), `parseUsbMidiPacket` (a switch on the CIN
plus a few assignments), the running-status serializer writing two bytes into
`Serial1`'s TX ring, and one SPSC ring push with a `__dmb()`. Generously, **300–500
M0+ cycles per message**.

| Message rate | Cycles/s | % of 120 MHz |
|---|---|---|
| 500 msg/s (2 ms breath) | ~0.25 M | ~0.2% |
| 2000 msg/s (500 µs breath) | ~1.0 M | ~0.8% |

Multiply by four for flash cache misses and poor codegen and it is still under
4%. The conclusion is structural: **whatever pico-pio-usb costs, what this design
adds to core 1 is in the noise.** If the USB stack needed 95% of the core, moving
the parser to core 0 would not save it — and would cost §6.1's latency property.

**The honest caveat.** Per clock the M0+ is much weaker than the S3's Xtensa LX7
— roughly 0.95 versus ~2.3 DMIPS/MHz, so a 120 MHz M0+ core is about a quarter to
a fifth of a 240 MHz LX7 core, with no hardware divide and no FPU. That penalty
is real; it just applies to work this design does not do. Core 1's job is integer
byte-shuffling, which is what an M0+ is adequate at. Separately: **pico-pio-usb's
own cost cannot be bounded from the desk.** Whether it busy-waits through the
data phase or hands off to DMA changes the answer materially. Measure it (§7.7);
do not accept an estimate, including this document's.

**The real risk is blocking, not average load.** USB host timing is governed by
ISR latency. A core at 5% average that occasionally stalls for 2 ms is much worse
than a core at 60% that never stalls longer than 10 µs. Core 1's blocking
hazards, in priority order:

1. **`Serial1.write()` on a full TX FIFO** — up to 320 µs *per byte*. This is
   the one that matters. **The `availableForWrite()` gate in §6.2 is a hard
   invariant, not hygiene.** Never spin waiting on the UART.
2. **Any `Serial` / USB CDC logging** — forbidden on core 1 (§5.4), for this
   reason.
3. **XIP flash cache misses** on cold paths: hundreds of nanoseconds each,
   compounding inside an ISR. Mark core 1's hot path and the ring primitives
   `__not_in_flash_func()`.
4. **SRAM bus contention** with core 0's display DMA. The RP2040's six-bank
   striped fabric is built for this, but if rare jitter appears, move core 1's
   hot code and the rings into the dedicated `__scratch_x` / `__scratch_y`
   banks.
5. **Interrupt-disabled critical sections** — none by design; both cross-core
   rings are lock-free (§7.4).

**Levers, if measurement says core 1 is tight**, in the order they should be
reached for:

1. `CFG_TUH_HUB = 0`, `CFG_TUH_DEVICE_MAX = 1`, and disable unused TinyUSB host
   classes. Real per-frame savings, no downside for a single-device bridge —
   **do this regardless of measurement.**
2. `__not_in_flash_func()` on the core 1 hot path.
3. 240 MHz — doubles headroom, but see §5.1: it is past the certified maximum.
4. Move the TRS serializer to core 0. Last resort: it surrenders exactly what
   this architecture was chosen to protect, making TRS latency hostage to core
   0's worst-case display stall (~41 ms, §3.2).
5. Native USB host on the RP2040's own USB controller (host-capable in
   hardware, and TinyUSB has an rp2040 host port *(verify)* — untested on this
   Arduino core), eliminating pico-pio-usb. Costs the CDC console and the
   Type-A jack — i.e. the entire reason for this board. Noted and dismissed.

### 7.7 Measurement gates the plan

Section 7.2 removes the ESP32's headline risk, but four numbers still matter,
and the first one **gates everything else**.

1. **Core 1 worst-case contiguous foreground time.** Must stay well under a
   millisecond. This protects USB enumeration, not merely latency. Per §7.6 it
   cannot be predicted from the desk, so it must be measured **in the first
   milestone, before any display or encoder work** — that is when changing
   course is still cheap.
2. **Real EWI Solo CC2 rate.** The 2 ms figure is an assumption inherited from
   esp32 §6.4. If it is closer to 500 µs, §6.3's duty cycle exceeds 100% and the
   breath stream needs thinning on send — a problem no core architecture fixes,
   because 31250 baud is a hard ceiling.
3. **Display region push times** at the chosen SPI clock, to re-derive
   `DisplayManager`'s budgets (§3.2).
4. **Core 1 average load**, as context for #1.

**Instrumentation.** The ESP32 firmware already has the right instinct —
`DisplayManager::worstValueUs` / `worstPageUs` and `ui_task::reportLoad()` put
real worst-case figures on the footer rather than in a comment. Do the same for
core 1:

- `time_us_32()` around core 1's foreground work; track a running max; publish
  it to core 0 through the display ring; render it in the footer.
- A `loop1()` iteration counter — its rate reveals when the foreground is being
  starved by ISRs.
- A GPIO toggle at the top and bottom of the foreground block, for scope work.

**Bring-up order follows from this.** Milestone one is "the EWI enumerates and
MIDI reaches the TRS wire, with core 1 instrumented" — ahead of display, ahead of
encoders.

### 7.8 Flash writes — a hazard this design avoids

Noted because it is an RP2040-specific trap absent from the ESP32 design.

The RP2040 executes from external QSPI flash via XIP. Erasing or programming it
requires leaving XIP mode, so the *other* core must be parked
(`rp2040.idleOtherCore()`) for the duration — 50–100 ms for a 4 KB sector. During
that window core 1 is stopped: no SOF generation, no PIO servicing, dropped
in-flight data, and possibly a device that considers the bus suspended.

**Nothing in this design writes flash at runtime.** No `EEPROM.commit()`, no
LittleFS, no library that persists silently. If runtime persistence is ever
wanted, use the SD card on the display breakout (Section 9.4) — it is an
external SPI device on core 0 and never parks core 1.

---

## 8. Pin Map

| Function | Pin | Notes |
|---|---|---|
| USB host D+ / D− | GP16 / GP17 | Fixed by board; `PIN_USB_HOST_DP` |
| USB host 5 V enable | GP18 | `PIN_5V_EN`, must be driven HIGH |
| TFT SCK / MOSI | GP14 / GP15 | SPI1, Feather SPI header |
| TFT MISO | GP8 | Only required for the SD slot |
| TFT CS / DC / RST | GP10 / GP11 / GP12 | D10 / D11 / D12 |
| SD CS | GP13 | v2 only |
| TRS MIDI TX | GP0 | UART0 TX; GP1 (RX) unused |
| Seesaw SDA / SCL | GP2 / GP3 | STEMMA QT, plug-in |
| Seesaw INT | GP9 | D9, active low, needs internal pull-up |
| `DAISY_READY` | GP24 | D24 |
| `DUMP_ACK` | GP25 | D25 |
| Spare | GP1, GP5, GP6, GP26–29 | A0–A3 are GP26–29 |
| Reserved | GP7 (boot), GP20/21 (NeoPixel) | Do not use |

Two items to confirm on the bench:

- Whether arduino-pico's Feather RP2040 USB Host variant maps GP2/GP3 to `Wire`
  or `Wire1` *(verify)*. It is I²C1 in hardware, but Adafruit variants commonly
  alias the primary bus.
- Whether the TFT breakout exposes a backlight (LITE) pin worth PWM-ing. If so,
  note that PWM3B is unavailable on this board when picking the pin.

**Power budget** *(verify)*: the Feather sources 5 V to the EWI Solo through
GP18 *and* drives a 2.0" backlight, both off one USB input. Measure the EWI's
actual draw before assuming this is fine.

---

## 9. Code Reuse from `spirant-hw-interface-esp32`

The existing firmware is roughly 4,000 lines across well-separated modules. Most
of it ports.

### 9.1 Ports unchanged

| Module | Lines | Why it ports |
|---|---|---|
| `params/param_specs.h` | 108 | Arduino-free, host-tested, pure data |
| `params/parameter_values.*` | 349 | Arduino-free, host-tested; **is** Channel B (§7.4) |
| `midi/midi_types.h` | 56 | Arduino-free |
| `midi/monitor_state.*` | 478 | Arduino-free, host-tested; loses only its mutex |
| `display/screen.h`, `theme.h` | 117 | Interface + constants (theme needs new geometry) |
| `tests/` | — | All three host test suites keep running unchanged |

`parameter_values.*` deserves emphasis: the `changed_display` / `changed_midi`
split, latest-value-wins coalescing, `takeMidiChanges()` and
`markAllChangedMidi()` were designed for the ESP32's needs and happen to be
*exactly* the cross-core parameter channel this design wants. No changes.

### 9.2 Ports with mechanical changes

| Module | Change |
|---|---|
| `display/display_manager.*` | Unchanged logic; new panel geometry; **re-derive all timing constants** (§3.2) |
| `display/lgfx_t_display_s3.h` | Replaced by an SPI/ST7789 `LGFX_Device` subclass |
| `display/param_screen.*`, `monitor_screen.*` | Layout only, for the taller body |
| `encoders/encoder_input.*` | Seesaw driver kept; ISR half removed in favour of polling (§4.2) |
| `log.*` | Collapses toward the USB sink alone (§5.4) |
| `config.h` | New pin map (§8); FreeRTOS task settings deleted; new board guards |

### 9.3 Rewritten or new

- **`midi/usb_midi_host.*`** (486 lines) — fully rewritten against
  `EZ_USB_MIDI_HOST` / `usb_midi_host`. The `USBConnection` subclass has no
  analogue here.
- **`midi/usb_midi_parser.*`** — kept or retired depending on §5.3.
- **`tasks/ui_task.*`** (373 lines) — becomes `loop()` on core 0; the FreeRTOS
  task wrapper disappears.
- **`midi/monitor_access.*`** (121 lines) — **deleted** (§7.5).
- **New: SPSC ring** — one small header, used for both channels.
- **New: TRS output module** — running-status serializer plus the §6.2 priority
  and headroom policy. No ESP32 analogue; esp32 §6 was never built (its README
  scopes MIDI output out of phases 1–2).
- **New: boot state machine** — §2. Also never built on the ESP32.

### 9.4 SD-card patch storage (v2)

esp32 §2.3 defers patch storage to v2. The chosen display breakout includes a
microSD slot on the same SPI bus, on core 0. This is a **better** home for it
than anything on the ESP32 board: it is external storage, so it entirely
sidesteps the §7.8 XIP-flash hazard, and its blocking writes land on the core
that carries no MIDI.

---

## 10. Open Items

### Gating — must be answered before the architecture is committed

1. **Core 1 worst-case foreground time and average load** (§7.6, §7.7).
   pico-pio-usb's cost cannot be bounded from the desk. This is milestone one,
   ahead of display and encoders. Everything downstream assumes the answer is
   comfortable; if it is not, §7.6's lever list is the response.
2. **§5.3 parse path** — spike `usb_midi_host` standalone under Adafruit
   TinyUSB; decide Option A or B. Shapes whether `usb_midi_parser` and its host
   tests survive.

### Specification contradictions — resolve before implementing §2 or §6

3. **Parameter count versus CC range — a three-way disagreement.**
   `kParamSpecs` defines `kNumActiveParams = 19`. esp32 §2.2 says the Daisy
   should "expect 16." CC 102–119 is **18** controllers. The handshake's
   completion condition depends on which is right.
4. **Delay Time does not fit a 7-bit CC** (§6.5): 143 distinct values. Pick a
   coarser step, a narrower range, or a 14-bit pair.

### Measurement — needed to size things, not to choose the architecture

5. **Real EWI Solo CC2 rate** (§7.7). Everything in §6.3 rests on the 2 ms
   assumption; at 500 µs the TRS wire is oversubscribed regardless of design.
6. **SPI clock and measured region push times** (§3.2), to re-derive
   `DisplayManager`'s budgets.
7. **Power budget** — EWI draw plus backlight on one USB input (§8).

### Platform facts to confirm

8. **`Wire` versus `Wire1` on GP2/GP3** (§8).
9. **LovyanGFX RP2040 SPI + DMA** — confirm `waitDMA()` semantics hold (§3.4).
10. **TinyUSB host config knobs** — confirm `CFG_TUH_HUB = 0` and
    `CFG_TUH_DEVICE_MAX = 1` are reachable in this Arduino core, and apply them.
    Per §7.6 this is worth doing regardless of what measurement says.

### Decided, recorded here so it is not silently revisited

- **Clock: 120 MHz.** 240 MHz is past the RP2040's 200 MHz certification (§5.1)
  and is an escape hatch for §7.6, not a default.
- **No runtime flash writes** (§7.8). Persistence, if it ever arrives, goes to
  the SD card on the display breakout (§9.4).
- **MIDI 2.0 / UMP dropped** (§5.2).
- **TRS saturation policy** (§6.6): notes and Program Change never dropped;
  breath CC2 and pitch bend thinned to one pending value each; parameter CCs
  dropped last and re-offered by `ParameterValues`.
- **Core 0 cadence** (§7.3): fixed ~30 Hz render tick, everything else polled
  every iteration. Encoder-to-pixel latency ~35–40 ms by design.
- **Channel B ring sized ≥ `kTotalSlots`** (§7.4), making its overflow branch
  unreachable rather than merely unlikely.

---

## 11. References

### Libraries

| Source | Relevance |
|---|---|
| [`EZ_USB_MIDI_HOST`](https://github.com/rppicomidi/EZ_USB_MIDI_HOST) | USB MIDI host wrapper; `onRx` → `tu_fifo` → `readAll()` path (§5.3) |
| [`EZ_USB_MIDI_HOST_PIO_example`](https://github.com/rppicomidi/EZ_USB_MIDI_HOST/blob/main/examples/arduino/EZ_USB_MIDI_HOST_PIO_example) | Reference for `setup1()`/`loop1()` and the clock check (§5.1) |
| [`usb_midi_host`](https://github.com/rppicomidi/usb_midi_host) | Underlying driver; `tuh_midi_packet_read()` for §5.3 Option B |
| [`Adafruit_Seesaw`](https://github.com/adafruit/Adafruit_Seesaw) | Encoder driver, unchanged from esp32 §4.1 |
| [Arduino MIDI Library](https://github.com/FortySevenEffects/arduino_midi_library) | `DefaultSettings::UseRunningStatus = false` (§6.3) |
| [LovyanGFX](https://github.com/lovyan03/LovyanGFX) | Display; RP2040 SPI backend (§3.4) |

### Hardware

| Source | Relevance |
|---|---|
| [Feather RP2040 USB Host pinout](https://learn.adafruit.com/adafruit-feather-rp2040-with-usb-type-a-host/pinouts) | §8 pin map; GP16/17/18, GP2/3, SPI1 |
| [2.0" 320×240 IPS TFT](https://learn.adafruit.com/2-0-inch-320-x-240-color-ips-tft-display) | ST7789, 4-wire SPI, microSD on shared bus (§3, §9.4) |
| [Adafruit Quad Rotary Encoder (5752)](https://www.adafruit.com/product/5752) | Seesaw at `0x49` (§4) |
| [RP2040 (Wikipedia)](https://en.wikipedia.org/wiki/RP2040) | 200 MHz certification, 264 KB SRAM, eight PIO state machines (§5.1, §7.6) |
| [RP2040 datasheet](https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf) | Clock/voltage limits, SRAM bank striping, `__scratch_x`/`__scratch_y` (§7.6) |

### Prior art in this repo

| Path | Relevance |
|---|---|
| [`plans/esp32_firmware_design.md`](../../plans/esp32_firmware_design.md) | The document this one parallels |
| [`spirant-hw-interface-esp32/`](../../spirant-hw-interface-esp32/) | ~4,000 lines of working firmware; §9 catalogues what ports |
| [`spirant-hw-interface-esp32/plans/esp32_implementation_plan.md`](../../spirant-hw-interface-esp32/plans/esp32_implementation_plan.md) | Phase structure worth mirroring for the implementation plan |

> **Note on esp32 §5's line references.** They have drifted from the current
> `ESP32_Host_MIDI` source: `xTaskCreatePinnedToCore` is at
> `USBConnection.cpp:87` (cited as L60–66), `_usbTask` at `:198–213` (cited as
> L172–188), and the enqueue/dequeue pair at `:98–124` (cited as L74–100). Worth
> fixing there when that document is next touched.
