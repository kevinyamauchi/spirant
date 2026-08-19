# spirant-hw-interface-esp32

ESP32-S3 firmware for the Spirant hardware interface, on a **LilyGO
T-Display-S3**. Implements **phases 1 and 2** of
[`plans/esp32_implementation_plan.md`](plans/esp32_implementation_plan.md):

- **Phase 1** — the UI subsystem: display plus encoders.
- **Phase 2** — USB MIDI host input, rendered on a monitor page. **MIDI output
  is explicitly out of scope**: nothing is sent to the Daisy and there is no TRS
  path yet.

Phase 1 came first because phase 2 puts the USB port into host mode and takes
the CDC console away with it (plan §0.1). The display and the out-of-band UART
log built there are the instruments phase 2 is verified with.

> **Before you flash phase 2, change the board settings.** USB Mode and USB CDC
> On Boot both move, and the firmware will not compile until they do — see
> [Board settings](#2-board-settings). That is deliberate; the combination they
> guard against silently destroys the encoder bus.

---

## Layout

```
spirant_hw_interface/
  spirant_hw_interface.ino     setup() / loop() — wiring and the MIDI drain
  src/
    config.h                   pins, addresses, tunables, feature flags
    log.h / log.cpp            USB CDC + UART log sink            (§1.1, §2.0)
    display/
      lgfx_t_display_s3.h      LGFX_Device subclass, ST7789/I8080 (§1.2)
      theme.h                  shared palette and geometry
      screen.h                 the Screen interface               (§2.4)
      display_manager.*        sprite bank + dirty-region dispatch (§1.3, §2.4)
      param_screen.*           the 6×4 parameter page + profiler  (§1.3)
      monitor_screen.*         the MIDI monitor page              (§2.4)
    diag/
      i2c_scan.*               bus survey and pin decision        (§1.4)
    encoders/
      encoder_input.*          Seesaw driver + level-triggered ISR (§1.5, §1.6)
    midi/
      midi_types.h             decoded message                    (§2.3)
      usb_midi_parser.*        USB-MIDI packet decode, host-tested (§2.3)
      monitor_state.*          latest-value-wins snapshot, host-tested (§2.0)
      monitor_access.*         the mutex around it                (§2.0)
      usb_midi_host.*          USBConnection subclass + drain     (§2.2, §2.3)
    params/
      param_specs.h            the 6×4 parameter table
      parameter_values.*       runtime state + change flags       (§1.7)
    tasks/
      ui_task.*                core-1 UI task                     (§1.8)
tests/
  Makefile
  parameter_values_test.cpp    host tests for §1.7
  usb_midi_parser_test.cpp     host tests for §2.3
  monitor_state_test.cpp       host tests for §2.0 / §2.4
archive/
  spirant_hw_interface_phase1.ino.bak
```

`config.h` is the only file you should need to edit on the bench.

`archive/` exists because the Arduino IDE concatenates **every** `.ino` in a
sketch folder. A second copy of the sketch kept alongside the first gives you
two `setup()`s and two `loop()`s and the sketch stops compiling — so snapshots
live outside `spirant_hw_interface/`.

---

## Arduino IDE setup

### 1. Board support

**Tools → Board → Boards Manager →** install **esp32** by Espressif. Then
**Tools → Board → esp32 → LilyGo T-Display-S3**.

### 2. Board settings

**Every board-menu default is correct for phase 2 as well.** This is the same
configuration `ESP32_Host_MIDI`'s own USB-Host example specifies, which is what
the library is tested against.

| Setting | Value | Default? | Note |
|---|---|---|---|
| USB Mode | Hardware CDC and JTAG | ✔ | Does **not** gate hosting — see below. |
| USB CDC On Boot | Enabled | ✔ | Gives a USB console until the PHY handover. Keeps `Serial` off the I²C pins. |
| USB Firmware MSC On Boot | Disabled | ✔ | Inert in this USB mode; must stay off in OTG mode. |
| USB DFU On Boot | Disabled | ✔ | Same. |
| Core Debug Level | None | ✔ | **Load-bearing**, and unrelated to USB mode — see below. |
| Upload Mode | UART0 / Hardware CDC | ✔ | Not "USB-OTG CDC"; that waits for a port the app never presents. |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) | ✔ | The sketch is ~640 KB; room to spare. |
| Arduino Runs On | Core 1 | ✔ | `loop()` shares core 1 with the UI task — the §1.8 arrangement. |

PSRAM is not a board-menu option here; the board definition fixes it.

#### USB Mode does not decide whether the board can host

The ESP32-S3 has **two** USB controllers sharing one PHY and one pin pair: the
fixed-function **USB-Serial/JTAG**, and the host-capable **USB-OTG**.
`ARDUINO_USB_MODE` only selects which one Arduino uses for its *device-side*
`Serial`. Either way, `usb_host_install()` claims the OTG controller and
switches the PHY to it.

> An earlier version of this README and of the `#error` guards claimed
> USB-OTG mode was **required** for hosting and rejected Hardware CDC outright.
> That was wrong, and it blocked the configuration the library itself
> recommends.

Hardware CDC is the better choice for two reasons. First, every TinyUSB device
start-up in the core is gated on `!ARDUINO_USB_MODE` (`cores/esp32/main.cpp`):

```c
#if ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE
  Serial.begin();
#endif
...
#if ARDUINO_USB_ON_BOOT && !ARDUINO_USB_MODE
  USB.begin();          // TinyUSB device init: claims the OTG controller
#endif
```

so in Hardware CDC mode `USB.begin()` is **never called** and CDC/MSC/DFU On
Boot cannot claim the controller at all. In OTG mode any of them can, which is
what the guards below are for.

Second, `Serial` resolves to `HWCDCSerial` rather than UART0, so USB logging is
safe *and* you get a console over the USB-C port.

#### What the USB console covers — and what it doesn't

It works from boot **until `usb_host_install()` takes the OTG PHY**, then goes
silent for the rest of the session. So it carries the boot banner, the I²C
survey and the redraw profile, but **not the enumeration descriptor dump**,
which happens after the handover. The UART sink on GPIO 17 is what covers that.

Writes are made non-blocking in `log_init()` via `Serial.setTxTimeoutMs(0)`.
HWCDC's disconnected path already drops rather than blocks, but the transition
window — host gone, SOF watchdog not yet expired — can otherwise wait up to
~2 s, and a stall of that length inside the core-0 USB task during enumeration
would be far worse than a dropped log line.

#### ⚠️ Core Debug Level must still be `None`

This constraint is **not** removed by Hardware CDC mode, and it's easy to
assume it is. The prebuilt IDF sets `CONFIG_ESP_CONSOLE_UART_NUM=0`, so
`ESP_LOG` output goes to UART0 — GPIO 43/44, SDA/SCL — regardless of which USB
mode Arduino is using. Only the *Arduino `Serial`* half of the hazard goes away.

#### 🛑 The one combination that destroys the encoder bus

Disabling CDC-on-boot makes the ESP32 core do this (`HardwareSerial.h`):

```c
#else   // !ARDUINO_USB_CDC_ON_BOOT -- Serial is used from UART0
#define Serial Serial0
```

`Serial` silently becomes **UART0 — GPIO 43/44, which is the I²C bus**. With
`LOG_SINK_USB` at `1`, `log_init()`'s `Serial.begin()` would attach the UART
peripheral to those pins and every log line afterwards would drive SDA. The
symptom is *"the encoders died"*, which points nowhere near the cause.

**Four `#error` guards, all verified to fire** by compiling the bad
combinations:

| Guard | In | Catches |
|---|---|---|
| `LOG_SINK_USB` vs `ARDUINO_USB_CDC_ON_BOOT` | `src/log.cpp` | The `Serial`-on-the-I²C-bus combination above |
| `ARDUINO_USB_CDC_ON_BOOT` **in OTG mode** | `src/config.h` | A device stack claiming the OTG controller before `setup()` |
| `ARDUINO_USB_MSC_ON_BOOT` **in OTG mode** | `src/config.h` | Same, via mass storage |
| `ARDUINO_USB_DFU_ON_BOOT` **in OTG mode** | `src/config.h` | Same, via DFU |

The last three are **conditional on `ARDUINO_USB_MODE == 0`**, because in
Hardware CDC mode those options are inert. Behaviour across all four USB
configurations:

| USB Mode | CDC On Boot | Result |
|---|---|---|
| Hardware CDC and JTAG | Enabled | ✅ **builds** — the recommended configuration |
| Hardware CDC and JTAG | Disabled | ❌ guard: `Serial` would be UART0; set `LOG_SINK_USB 0` |
| USB-OTG (TinyUSB) | Enabled | ❌ guard: device stack claims the OTG controller |
| USB-OTG (TinyUSB) | Disabled | ❌ guard: `Serial` would be UART0; set `LOG_SINK_USB 0` |

The bottom row is a legitimate configuration — it's what phase 2 originally
shipped with. Set `LOG_SINK_USB 0` and it builds; the guard message says so.

The `config.h` guards live there rather than beside the USB code so they fire on
the **first translation unit compiled**. A guard placed after a library
`#include` is a guard a failing library header can mask — which happened here
once, when a core shipping no USB-host component turned a board-setting question
into a confusing `usb/usb_host.h: No such file or directory`.

As-built, verified by a clean compile on this machine:

```
Core version:      esp32 3.3.10          <- NOT 3.3.11; see the regression note below
FQBN:              esp32:esp32:lilygo_t_display_s3   (all menu defaults)
Flash / RAM:       643,019 bytes (20%) / 38,388 bytes global (11%)
                   (phase 1 was 386,743 / 26,740)
```

#### ⚠️ The core 3.3.11 USB-host regression

Core **3.3.11** moved the prebuilt ESP-IDF from v5.5.4 to v5.5.5, which enables
`CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK`. With that feature compiled in,
leaving `usb_host_config_t::enum_filter_cb` at `NULL` — the documented default —
**stalls enumeration silently**: the client never receives
`USB_HOST_CLIENT_EVENT_NEW_DEV`, no device mounts, nothing is logged. It affects
any USB-host code, not just MIDI (espressif/arduino-esp32#12778, #12783).

You are covered twice over:

- **Core 3.3.10** has the feature off (`# CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK is not set`).
- **ESP32_Host_MIDI 7.2.0** — the pinned version — registers an accept-all
  filter behind that same macro, so it works on 3.3.11 too.

So either core is fine, but keep the library at ≥ 7.2.0 if you move to 3.3.11.

The ~58 KB of sprite buffers are allocated from the heap at boot and are not in
that global figure. The +145 KB of flash is almost entirely `ESP32_Host_MIDI`:
the Arduino IDE compiles every source in a library, and `MIDIHandler.cpp`
defines a global `midiHandler` object that drags in `std::deque`, `std::map` and
`std::string` even though this firmware never calls it (see
`src/midi/usb_midi_host.h` for why it does not).

### 3. Libraries

**Tools → Manage Libraries…** — these are already installed and were used for
the verifying build:

| Library | Author | Version | Used for |
|---|---|---|---|
| LovyanGFX | lovyan03 | 1.2.26 | Display |
| Adafruit seesaw Library | Adafruit | 1.7.9 | Encoders |
| Adafruit BusIO | Adafruit | 1.17.4 | (seesaw dependency) |
| ESP32_Host_MIDI | sauloverissimo | 7.2.0 | USB host + its ring buffer only |

Plan §1.0 asks for deps pinned by version; that is what these are. If you
update one, update the table.

**Only `USBConnection` from `ESP32_Host_MIDI` is used.** Its `MIDIHandler`
aggregator is deliberately bypassed — it would add a second queue on the hot
path, its event type allocates `std::string`s per message, and its parse is not
host-testable. The reasoning is written out at the top of
`src/midi/usb_midi_host.h`; read it before "simplifying" this to the library's
documented `addTransport()` pattern.

### 4. Open and flash

Open `spirant_hw_interface/spirant_hw_interface.ino`. The IDE compiles
everything under `src/` recursively, so there is no library manifest to
maintain.

**In host mode the USB-C port is no longer a serial console**, so every flash
needs manual bootloader entry: hold **BOOT**, tap **RST**, release **BOOT**.
Confirm that works *while still in device mode*, before you need it. The log
comes out of GPIO 17 at 115200 — that adapter is now your only console.

---

## Wiring

Display, backlight and power-enable are on-board; nothing to wire. What you
add:

| Signal | ESP32 pin | To | Notes |
|---|---|---|---|
| I²C SDA | GPIO 43 | Seesaw SDA | STEMMA QT / Qwiic connector |
| I²C SCL | GPIO 44 | Seesaw SCL | STEMMA QT / Qwiic connector |
| Seesaw INT | GPIO 16 | Seesaw INT | Active low, open drain; internal pull-up is enabled in firmware |
| 3V3 / GND | — | Seesaw | Via Qwiic. The header 3V3 rail is gated by `PIN_POWER_ON` (GPIO 15), which the firmware drives high |
| Log UART TX | GPIO 17 | USB-UART adapter RX | 115200 8N1 |
| Log UART RX | GPIO 18 | USB-UART adapter TX | Not used yet; reserved |

Free after this: GPIO 1, 2, 3, 10, 11, 12, 13, 21 — eight pins for phase 3's
TRS MIDI TX and phase 4's handshake lines, closing design doc §2.5's budget
with room to spare.

### The I²C pin decision (plan §0.2, §1.4) — resolved

**This unit's Qwiic connector is on GPIO 43/44.** A scan of 17/18 found
nothing. So: **I²C takes the connector at 43/44, and the log UART moves to
17/18.**

Plan §0.2 recommends the opposite — hand-wire I²C to 17/18 and leave the
connector unused, to keep 43/44 (the default UART0 pins) for the log console.
That trade-off does not actually exist. ESP32 UARTs route through the GPIO
matrix, and `log.cpp` drives `Serial1` with explicit pins, so the log console
can sit on any free GPIO. There is no reason to give up a plug-and-play bus.

Two consequences worth knowing:

- **The ROM bootloader prints on U0TXD (GPIO 43) at reset**, which is now SDA.
  With SCL idle high, that appears on the bus as a burst of spurious
  START/STOP conditions with no clock edges, so nothing is latched. It is over
  before `setup()` runs.
- **Core Debug Level must stay `None`.** Routing ESP_LOG output to UART0 would
  drive SDA during normal operation and corrupt the bus. Nothing else does:
  the core calls `Serial0.setPins()` before `setup()`, but that only *stores*
  the pins — GPIO 43/44 are attached to the UART peripheral only by a
  `Serial0.begin()`, which this firmware never calls.

`config.h` still has the alternate-pair scan enabled, now set to **44/43 — the
same pins in the opposite order** — to settle which of the two is SDA. Exactly
one of the two boot scans should report `0x49`. Set `I2C_SCAN_ALT_PINS 0` once
you know.

---

## Phase 1 bring-up, in plan order

Flash once and read the log; most of phase 1 reports itself. This section is
kept as the record of how phase 1 was verified, and as the procedure to fall
back on if the UI ever misbehaves — set `USB_MIDI_ENABLED 0` and the phase 1
board settings and you are back here.

**§1.1 — log sink.** Both sinks were on for this step (`LOG_SINK_USB`,
`LOG_SINK_UART`); `LOG_SINK_USB` is now 0 and must stay so. Attach a USB-UART
adapter to **GPIO 17** at 115200 and confirm you get byte-identical output on
both. **This was done in phase 1, not phase 2** — it is the phase 2
prerequisite and the one thing you cannot debug once USB host mode takes the
console.

**§1.2 — display.** You should get a splash, then the parameter page. Check
orientation, that colours are not inverted, and that there is no offset band at
either edge. If it reads upside down, change `DISPLAY_ROTATION` from 1 to 3. If
pixels are torn or speckled, drop `LCD_BUS_FREQ_HZ` to 6000000 before
suspecting anything else.

**§1.3 — redraw profiling.** Runs automatically at boot
(`DISPLAY_PROFILE_ON_BOOT`) and logs:

```
profile: value region  min=..us avg=..us max=..us (target <2000)
profile: full page     min=..us avg=..us max=..us (target <20000)
profile: push=..us then waitDMA=..us -> async (DMA overlaps)
```

Record the numbers below and write them back into the design doc (plan §8.6).

**§1.4 — I²C survey.** Runs at boot. Expect exactly one device, the Seesaw at
`0x49`. If more than one turns up, the firmware says so — and design doc §4/§7.3's
"one consumer, no mutex needed" conclusion needs revisiting.

The alternate pin pair is probed too, unless it collides with the log UART, in
which case the log tells you to set `LOG_SINK_UART 0` to probe it.

**§1.5 — polled encoders.** Set `ENCODER_USE_INTERRUPT 0` in `config.h` and
reflash. Encoders are then read purely on the 20 ms tick. This isolates Seesaw
driver problems from ISR problems — if something is wrong here, the interrupt
is not the cause. Confirm all four produce correct signed deltas and that both
page-nav switches work.

**§1.6 — level-triggered interrupt.** Set `ENCODER_USE_INTERRUPT` back to 1.
The specific failure to test for is **a second change arriving while the first
is being serviced**: spin two encoders simultaneously and confirm neither is
dropped. Then a 60 s aggressive four-encoder spin — watch `detents`, `glitch`
and `i2cerr` in the stats line, and confirm no lockup.

**§1.7 — parameter model.** Host tests:

```bash
make -C spirant-hw-interface-esp32/tests run
```

**§1.8 — task split and priorities.** Phase 1 validated this with a synthetic
load in `loop()` burning 500 µs every 2 ms on core 1 below the UI task. Phase 2
replaced it with the real MIDI drain, which is the same arrangement under real
traffic. Watch the footer and the stats line:

```
stats: wakes=.. svc=.. isr=.. detents=.. glitch=.. i2cerr=.. lat=..us val=..us page=..us drain_late=..us/..
```

`lat` is ISR-to-service latency — the UI task not being starved. `drain_late` is
the MIDI drain's worst scheduling lateness — the drain not being starved by
display redraws. Both should stay small; neither should grow without bound.

---

## Powering the board for USB host — §2.1, RESOLVED

**The T-Display-S3 cannot supply VBUS from any onboard rail.** This is the
single thing that blocks USB host on this board, and it is hardware, not
firmware.

Measured on this unit: **VBUS = 0 V at the USB-C connector** while the board was
powered and running normally. Confirmed against the schematic (`schematic/T_Display_S3.pdf`
in LilyGO's repo), Power block:

```
   VBAT ──[Q4 SI2307]──┐
 (3.7-4.2V, via P3)     ├── +5V net ──▶ U7 AP2112K-3.3V ──▶ 3V3
   VBUS ──▶|───────────┘                 (and P2 pin 1 = the "5V" header pin)
            D3 IN5819
      (anode VBUS, cathode +5V)
   VBUS ──[R14 100K]── GND        (bleed: undriven VBUS sits at 0 V)
```

Three things follow, and together they close the question:

1. **D3 blocks +5V → VBUS.** The 5V header pin is an *output*, fed from USB.
2. **There is no boost converter anywhere on the board.** On battery the "+5V"
   net is actually 3.7–4.2 V — the name is a misnomer; it is just the LDO input.
   So even without D3, no 5 V exists to supply.
3. **GPIO 19/20 (USB D+/D−) are not broken out.** Headers P1/P2 carry only
   GPIO 1/2/3/10/11/12/13, U0TXD/U0RXD, IIC_SDA/SCL, the touch-panel pins,
   +5V, GND and 3V3 — so the connector cannot be bypassed without soldering.
This settles a contradiction in
[LilyGO issue #205](https://github.com/Xinyuan-LilyGO/T-Display-S3/issues/205):
a user reports the 5V pin is output-only and that feeding it caused damage
reports, while a LilyGO maintainer replies that it "is the same line as the USB
5V." The measurement supports the former — there is a Schottky between them.

Battery power is **strictly worse** for this purpose: a LiPo is 3.0–4.2 V and
the board has a charger but no boost, so there is no 5 V rail on the board at
all.

### What to do instead

Two workable routes, cheapest first.

**1. A self-powered USB hub** between the board and the instrument. The hub
supplies VBUS to the EWI from its own mains supply; the board keeps powering
itself as it does now. **External hubs are supported by this IDF build** — the
shipped sdkconfig has:

```
CONFIG_USB_HOST_HUBS_SUPPORTED=y
CONFIG_USB_HOST_HUB_MULTI_LEVEL=y
```

Caveat: some hubs gate their downstream ports on seeing upstream VBUS, which
this board never provides, so a given hub may simply stay dark. It costs
nothing to try and needs no wiring.

**2. Inject 5 V onto the connector's VBUS** — a USB-C OTG adapter with a power
input, or an equivalent injector cable. Because D3 is oriented VBUS → +5V, the
injection also forward-biases it and powers the whole board through the normal
USB path, so **one supply feeds both the board and the instrument**, exactly as
a USB charger does today.

- **Do not** also drive the 5V pin or the battery connector while injecting, and
  don't inject while the USB-C is connected to a computer.
- There is no CC controller wired on this board, so the host role is chosen
  purely in software — CC resistors matter only to whatever you plug in.

⚠️ **Do not feed the 5V pin while a battery is connected.** That pin is the +5V
net, which is also Q4's (SI2307) drain on the battery path. Depending on the
FET's gate state and body-diode orientation this can push current toward VBAT
outside the charger — a plausible mechanism for the damage reports in
[LilyGO issue #205](https://github.com/Xinyuan-LilyGO/T-Display-S3/issues/205).

### There is no software workaround

The OTG core's `DRVVBUS` (GPIO-matrix signal 63) and `VBUSVALID` (signal 61)
exist and are routable, so the silicon *can* drive and sense a VBUS switch —
LilyGO routed neither. IDF already powers the root port: `usb_host_install()`
does it unless `root_port_unpowered` is set, and `USBConnection::begin()` passes
a zero-initialised config, so `PRTPWR` is asserted and `DRVVBUS` is asserted
internally. It simply reaches no component. **0 V is the expected result, and
no config flag changes it.** Firmware also cannot *sense* VBUS here, since
nothing is wired to signal 61.

If a v2 board is ever spun (see `pcb/`), the fix is a current-limited USB power
switch (TPS2065 / MIC2025 class) with `EN` driven by a GPIO carrying `DRVVBUS`,
plus a VBUS divider into `VBUSVALID`. That would also make the §2.1 current-draw
measurement possible and turn a browning-out instrument into a reportable event.

---

## Phase 2 bring-up, in plan order

**§2.1 — host port and power.** See the section above; this is resolved on
paper and blocked on the physical injector. Once powered, watch the `i2cerr`
counter in the stats heartbeat *while the instrument is drawing current*, not
just at idle. The Seesaw runs off the header 3V3 rail gated by `PIN_POWER_ON`
(GPIO 15); if VBUS draw sags that rail, the encoder bus shows it first.

**§2.2 — enumeration, known-good device first.** Use a **generic
class-compliant USB MIDI keyboard before the EWI**. This separates "does the
library work here" from "does the EWI behave as expected" — two very different
debugging problems. At enumeration the log gives you everything §2.2 asks for:

```
usb: VID=041E PID=3F0E bcdUSB=0200 class=00/00/00 ep0=64 cfgs=1
usb: config wTotalLength=101 interfaces=2
usb:   if 0 alt 0 eps 0 class=01 sub=01 proto=00
usb:   if 1 alt 0 eps 2 class=01 sub=03 proto=00  <- MIDIStreaming
usb:     MS header bcdMSC=0100 (MIDI 1.0)
usb:     ep 81 IN  bulk mps=64 bInterval=0
usb:     ep 01 OUT bulk mps=64 bInterval=0
usb: claimed IN ep 81 mps=64 interval=1ms
```

`bcdMSC` is the authoritative MIDI 1.0 / 2.0 answer. Note that `USBConnection`
claims the first MIDIStreaming interface it meets regardless of alternate
setting, so on a MIDI 2.0 device it takes the MIDI 1.0 alt-0 fallback.

**"Enumerated" and "usable" are different claims** and the header shows which
you have: `no device` → `no MIDI ep` (enumerated, nothing claimed) → `VID:PID`
(endpoint ready). Only the last means data can arrive.

**§2.3 — parse and drain.** Set `USB_MIDI_LOG_MESSAGES 1` for single-message
checks; leave it `0` for anything sustained, because 115200 cannot carry breath
rates and the drain would block on the UART. The heartbeat carries the health
numbers either way:

```
midi: msg=.. ../s cc2=.. ../s minint=..us unparsed=.. ring=../64 hwm=.. nearfull=.. batch=.. stage_ovf=.. ch=0001
```

- `cc2 N/s` — **the authoritative breath rate.** This is the measurement §2.5
  wants and §6.4's bandwidth budget is waiting on. Divide 64 by it for the §2.3
  ring margin.
- `minint` — smallest observed gap between CC2 messages, µs. **A bound on burst
  spacing, not a rate.** Every message in one drain pass is folded in with the
  same timestamp, so `minint` cannot see inside a burst; `0` means "never
  resolved", not "instant". Use `cc2 N/s` for rate.
- `hwm` — deepest the transport ring was ever seen. **The other phase 2 number
  that matters.** See the margin note below.
- `nearfull` — times the ring came within one slot of full. Should be 0.
- `batch` — most packets consumed in one drain pass.
- `stage_ovf` — packets dropped because one drain produced more than the staging
  buffer holds (80). Must be 0.
- `unparsed` — packets the parser declined to model. Not an error; a rising
  count is information about the device.

**§2.4 — the monitor page.** **Switch 2** toggles between the parameter page
and the monitor. Switches 0 and 3 keep paging parameters and do nothing on the
monitor. Play the keyboard and confirm notes, the breath bar and the bend bar
track without perceptible lag.

**§2.5 — EWI substitution.** Swap the instrument in and record what it actually
sends. `ch=` is a bitmask of channels seen; `cc2 N/s` is the breath rate; the CC
row on the monitor page shows the controller set.

### ⚠️ The ring-buffer margin is thinner than the plan says

Plan §2.3 computes ≈11× margin from a 64-entry ring against an assumed 2 ms CC2
rate. The ring really is 64 (`USBConnection::QUEUE_SIZE`, a protected constant —
not configurable without patching the library), but §2.5's own pessimistic rate
changes the answer:

| CC2 rate | Buffer holds | vs 11.7 ms worst UI stall |
|---|---|---|
| 2 ms (assumed) | 128 ms | ~11× |
| **500 µs (§2.5's warning)** | **32 ms** | **~2.7×** |

2.7× is thin. Worse, **the library discards silently when its ring fills** —
`enqueueMidiMessage()` returns false and its only caller ignores it — so a true
overrun count cannot be built from outside. `hwm` and `nearfull` are the
substitutes, and they warn *before* data is lost rather than after.

If `nearfull` is ever non-zero, the levers in order of cost: shrink the
worst-case UI stall (the full-page repaint), move the drain to core 0, or patch
`QUEUE_SIZE` in the library. Do not simply accept it.

---

## Bring-up log

Fill these in on the bench. Plan §8 lists them as design-doc amendments.

Phase 1 is complete; these are the as-built figures.

```
Qwiic/STEMMA connector on: [x] 43/44          ← confirmed; 17/18 scan was empty
I²C pins confirmed:        SDA 43   SCL 44    ← this order
Devices on the bus:        0x49 only (Seesaw) → no-mutex conclusion holds
                                                 for the BUS (see plan §2.0)

Value region redraw:       min 1332 avg 1422 max 1539 µs   (target < 2000) OK
Full page redraw:          min 11661 avg 11668 max 11688 µs (target < 20000) OK
DMA genuinely async:       [ ] yes  [ ] no    ← still to record

60 s four-encoder stress:  detents ______  glitches ______  i2c errors ______
Worst ISR→service latency: ______ µs
Worst synthetic-load late: ______ µs

UART log sink verified:    [x] yes   ← the phase 2 prerequisite, on GPIO 17
Free GPIOs after phase 1:  1, 2, 3, 10, 11, 12, 13, 21   (eight, for §2.5+)
```

Phase 1 build as flashed: 386,743 bytes flash (12%), 26,740 bytes global RAM
(8%), ~37.4 KB of sprites from the heap.

### Phase 2 — to fill in on the bench

Nothing below has been measured. The firmware is written and compile-checked;
every line here needs hardware.

```
§2.1  host port and power
  VBUS at USB-C connector:      [x] 0 V measured -- board cannot source it
  Root cause:                   D3 (IN5819) blocks +5V pin -> VBUS. Schematic
                                confirmed; no software workaround exists.
  Injector used:                ______________________________
  VBUS after injection:         ______ V   (expect ~5 V at the device end)
  EWI bus-powered?              [ ] yes  [ ] needs external supply
  Measured EWI current draw:    ______ mA
  i2cerr while EWI drawing:     ______   (must stay at its idle value)
  Display artifacts under load: [ ] none  [ ] seen: ______
  Flashing with instrument attached?  [ ] works  [ ] must unplug

§2.2  enumeration — KEYBOARD FIRST
  Keyboard  VID:PID  ______:______  bcdMSC ______  ep ______ bInterval ______
  Held 10 minutes without dropout:   [ ] yes  [ ] no
  EWI Solo  VID:PID  ______:______  bcdMSC ______  ep ______ bInterval ______

§2.3  drain health, sustained play
  ring hwm / 64:                ______      (nearfull must be 0)
  nearfull:                     ______
  max drain batch:              ______
  staging overflows:            ______      (must be 0)
  worst drain lateness:         ______ µs
  unparsed packets:             ______

§2.4  monitor page
  Notes / breath / bend track with no perceptible lag:  [ ] yes  [ ] no
  Screen toggle (switch 2) feels deliberate:            [ ] yes  [ ] no
  Worst monitor full repaint (page= in stats):          ______ µs

§2.5  EWI characterisation  → write these back into the plan and §6.4
  Measured CC2 rate (cc2 ../s): ______ /s   ← §6.4 assumes 500/s (2 ms)
  CC2 min interval (minint):    ______ µs   ← bound on burst spacing only
  Channels seen (ch= mask):     ______
  CC set observed:              ______________________________
  Pitch bend range used:        ______ .. ______  (14-bit)
  MIDI 1.0 confirmed:           [ ] yes  [ ] it enumerated as 2.0
  Note-transition behaviour:    ______________________________

  RECOMPUTED MARGIN: 64 packets / (measured cc2 rate)     = ______ ms
                     vs 11.7 ms worst UI stall            = ______ x
                     If under ~3x, act on it — see the note above.

  Hot-unplug and replug recovers without reset:  [ ] yes  [ ] no
```

Phase 2 build as compiled: 531,275 bytes flash (16%), 38,380 bytes global RAM
(11%), ~58 KB of sprites from the heap.

---

## Configuration flags

All in `src/config.h`.

| Flag | Value | What it does |
|---|---|---|
| `LOG_SINK_USB` / `LOG_SINK_UART` | **1** / 1 | Log destinations. USB is safe with CDC On Boot Enabled — guarded. |
| `LOG_USB_WAIT_MS` | 1500 | How long boot waits for a USB console. Costs this on every boot with no host attached; drop it once you stop reading the boot log over USB. |
| `USB_MIDI_ENABLED` | 1 | 0 builds the phase 1 firmware with the phase 2 UI. |
| `USB_MIDI_LOG_ENUMERATION` | 1 | Full descriptor dump at enumeration (§2.2). |
| `USB_MIDI_LOG_MESSAGES` | 0 | Per-message log. A firehose — bring-up only. |
| `USB_RING_CAPACITY` | 64 | Mirror of the library's ring size, for the margin arithmetic. |
| `MIDI_DRAIN_PERIOD_MS` | 1 | Drain period. The drain blocks, never spins (§2.3). |
| `MONITOR_LOCK_TIMEOUT_MS` | 5 | Snapshot mutex wait. On timeout: stale render, dropped update. |
| `SCREEN_TOGGLE_SWITCH` | 2 | Which encoder switch toggles param page ↔ monitor. |
| `ENCODER_USE_INTERRUPT` | 1 | 0 gives the §1.5 polled-only mode. |
| `SEESAW_GPIO_INTERRUPTS` | 0 | Put the push-buttons on the shared INT line. See below. |
| `I2C_SCAN_ON_BOOT` | 1 | Run the §1.4 survey at boot. |
| `I2C_SCAN_ALT_PINS` | **0** | Answered in phase 1; off since. |
| `DISPLAY_PROFILE_ON_BOOT` | 1 | Run the §1.3 timing sweep at boot. |
| `DISPLAY_ROTATION` | 1 | 1 or 3; they differ only in which edge is up. |
| `UI_TICK_MS` | 20 | UI task wake interval — also the monitor page's rate limiter. |

`SYNTHETIC_LOAD_*` is **gone**, not switched off. It stood in for the MIDI
drain and `loop()` now runs the real thing; keeping it would have been a dead
flag competing with what it simulated. Its `ui_task::reportLoad()` hook survives
and carries the real drain's lateness to the footer.

---

## Where this deviates from the plan, and why

**Parameter model — 6 pages, not 4.** Plan §1.7 describes "16 parameters across
4 pages of 4" with "plain 7-bit `uint8_t` 0–127". The shipped model is
[`spirant-parameter-values-rs`](../spirant-parameter-values-rs): **6 pages**
mirroring the Daisy v0 signal chain, values as `int32_t` in real engineering
units (Cutoff Floor 20–500 Hz step 5, Damp LP 1000–18000 Hz step 250), and five
null slots. `src/params/` ports that, so the ESP32, the Pico and the Daisy
agree on one table. The consequence for later: phase 3's CC mapping must scale
real units into 0–127 rather than passing values through.

The table is duplicated in three languages across three MCUs. Changing a range
is a three-firmware change — see the sync contract note at the top of
`param_specs.h`.

**Page navigation — encoder 0 back, encoder 3 forward.** Plan §1.7 says
"encoder switch → page change". This follows the shipped Pico behaviour
instead, which gives you backward navigation — worth having across six pages.
Configurable via `PAGE_BACK_SWITCH` / `PAGE_FWD_SWITCH`.

**Seesaw GPIO interrupts default off.** Plan §1.6 calls for
`setGPIOInterrupts()` on the switches. The Pico firmware deliberately does not
enable them, so that a button press cannot perturb the rotation interrupt path;
the buttons are polled instead. `SEESAW_GPIO_INTERRUPTS` lets you try the plan's
version without editing any module. Buttons are polled on the UI tick either
way, so the UI is identical.

**Two additions not in the plan.** `encoder_input.cpp` rejects position jumps
larger than 64 counts in one service window as corrupted I²C reads — applying
one would slam a parameter to its rail, which is far more visible than dropping
it. And `rearm()` bounds consecutive INT re-fires: if the line will not
deassert after 64 services it stops re-arming and falls back to tick polling,
rather than spinning at UI-task priority. Both are counted and reported in the
stats line.

**No blink.** The T-Display-S3 has no user LED, so §1.0's end-to-end check is
the boot log line plus the splash screen.

**`slog_printf`, not `log_printf`.** Plan §1.1 names the function
`log_printf()`. The ESP32 core already declares one in `esp32-hal-log.h`, and
the two collide, so ours carries a prefix. `log_init()` is unchanged.

**No `arduino-cli` / `sketch.yaml`.** You are driving this from the Arduino
IDE. Board settings and library versions are pinned in the tables above
instead — keep them filled in.

### Phase 2

**`MIDIHandler` is bypassed.** Plan §2.2/§2.3 describe the library's documented
path — `addTransport()`, `midiHandler.task()`, iterate `getQueue()`. Only
`USBConnection` is used. Three reasons, in weight order: that path adds a second
queue on the hot path (design doc §5 requires the transport ring be the *sole*
cross-core boundary, and `MIDIHandler`'s queue defaults to 20 events, below the
ring's own 64, making it the binding constraint); `MIDIEventData` carries four
`std::string`s in a `std::deque`, so thousands of heap allocations per second at
breath rates; and our parse is host-tested, theirs cannot be. Full argument at
the top of `src/midi/usb_midi_host.h`.

**The monitor snapshot is behind a mutex, not a seqlock.** A seqlock is the
textbook answer for one-writer/one-reader latest-value-wins, and it is wrong
here: its reader spins until the writer finishes, and the reader is the *higher
priority* task on the *same core*. Preempting the writer mid-update means
spinning forever on a writer that can never be scheduled — a hard hang. A
FreeRTOS mutex is correct precisely because it does priority inheritance. The
drain takes it once per batch, not once per message. Written up in
`src/midi/monitor_access.h`.

**Overrun counting is high-water, not exact.** The library discards silently
when its ring fills and reports nothing, so `hwm` and `nearfull` are the honest
substitutes. See the margin note in the phase 2 bring-up section.

**A second `#error` guard beyond the plan.** Plan §2.0 specifies the
`LOG_SINK_USB` / CDC-on-boot guard. `USB_MIDI_ENABLED` vs `ARDUINO_USB_MODE` was
added alongside it: forgetting to switch USB Mode otherwise builds and runs
perfectly and simply never enumerates anything, which is a much worse afternoon
than a compile error. Both are verified to fire.

**The screen refactor was done first, as §2.4 advises.** `DisplayManager` no
longer knows what a parameter is; it owns the panel, a three-sprite bank and the
timing, and dispatches to a `Screen`. `worstValueUs()` deliberately ignores
region pushes made during a full repaint, so its §1.3 meaning — worst single
incremental update — survives into phase 2 and the numbers stay comparable.
