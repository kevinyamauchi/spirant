# Spirant ESP32 Firmware — Implementation Plan

Companion to `esp32_firmware_design.md`. Scope: **ESP32-S3 firmware only**. The
Daisy-side state machine, `DAISY_READY`/`DUMP_ACK` handling, and CC 102–119
parsing are handled separately; this plan stubs the Daisy side and never blocks
on it.

**Target board:** LilyGO T-Display-S3 (1.9" ST7789), USB host confirmed working
**Toolchain:** Arduino IDE (Arduino core for ESP32 3.3.11)
**Display library:** LovyanGFX 1.2.26
**Verification instrument:** the on-board display, plus an out-of-band UART log

| Phase | Status |
|---|---|
| 1 — UI subsystem: display + encoders | ✅ **Complete** (2026-08-08) |
| 2 — USB MIDI host in, displayed on screen | 🟡 **Code complete, compile-checked; bench verification pending** (2026-08-08) |
| 3 — TRS MIDI output and routing | Not started |
| 4 — Boot handshake | Not started |
| 5 — Integration | Not started |

As-built lives in `spirant-hw-interface-esp32/`; see its README for board
settings, wiring and the bring-up log. §1.9 and §2.6 record what was built
differently from this plan and why.

**Phase 2 is written but nothing in it has touched hardware.** Everything
§2.1, §2.2 and §2.5 ask for is a measurement, and the README's phase 2 bring-up
log is the blank form for them. §2.6 lists what is claimed versus what is
verified — read that before treating any of this as done.

**A note on the companion document.** This plan cites `esp32_firmware_design.md`
throughout (§2.5, §5, §6.4, §7.3). **That file does not exist in this
repository** — the only architecture document present is
`plans/hardware_interface_firmware_architecture.md`, which describes the
Pico/Rust firmware. Until it turns up or is written, §8's amendments have no
target and measurements should be written back into *this* plan and the README.

---

## 0. Constraints that shape the phase order

The design document is accurate on the display (§3) and on the Seesaw driver
strategy (§4.1). Three platform constraints, however, drive the ordering and the
pin plan below.

### 0.1 In USB host mode you lose the USB serial console

The ESP32-S3's USB-Serial/JTAG controller and the USB-OTG controller share the
same D+/D− pins. Once OTG is in **host** mode, the USB-C port is no longer a CDC
console — `Serial.printf()` goes nowhere, and flashing needs manual bootloader
entry (BOOT + RST).

**This is the core justification for doing the UI phase first.** The display has
to be the instrument in Phase 2, because the obvious instrument is gone. A
secondary out-of-band UART log is cheap insurance and is built in Phase 1 (§1.1)
while USB CDC still works.

**Amended after Phase 1.** The ordering argument was right, but the premise is
weaker than stated: the §1.1 UART log is verified and working, so Phase 2 has a
real console throughout host-mode bring-up. The display is not the only
instrument. This does not change the phase order — it **decouples** the work
within Phase 2: §2.2 and §2.3 can be debugged entirely over UART before any
monitor-page code exists, so §2.4 can be built last and built once, against
known-good parse output.

### 0.2 Pin budget — RESOLVED

**As built.** The board exposes 20 header pins, 13 of them usable GPIO.

| Function | Pins | Status |
|---|---|---|
| Encoder I²C (SDA/SCL) | **43 / 44** | ✅ Committed — the Qwiic connector |
| Seesaw INT | **16** | ✅ Committed, level-triggered |
| Log UART TX / RX | **17 / 18** | ✅ Committed |
| TRS MIDI TX | 1 | Phase 3 — pick from the free list |
| `DAISY_READY` / `DUMP_ACK` | 2 | Phase 4 — closes design doc §2.5 |
| **Used / free** | **5 used, 8 free** | 1, 2, 3, 10, 11, 12, 13, 21 |

Eight free GPIOs against three remaining committed needs, with room left for the
deferred `RESYNC_REQUEST`. **The budget risk is retired.**

#### The I²C pin decision, and how the original recommendation was wrong

This plan originally said: the Qwiic connector might be on 17/18 or on 43/44;
if it is on 43/44, hand-wire I²C to 17/18 and leave the connector unused, so
that 43/44 stay available as the UART0 log console.

**Measured:** the connector on this unit is on **43/44**. A scan of 17/18 found
nothing.

**But the recommended workaround was based on a false premise.** It assumed the
log console had to be UART0. It does not — ESP32 UARTs route through the GPIO
matrix, and `Serial1.begin(baud, SERIAL_8N1, rx, tx)` puts the console on any
free pins. So the correct resolution is the *opposite* of the original advice:
**take the connector for I²C and move the log UART to 17/18.** Nothing is lost.

Two consequences of I²C living on the UART0 pins, both benign, both important
to know:

1. **The ROM bootloader prints on U0TXD (GPIO 43) at reset and in download
   mode**, and GPIO 43 is now SDA. With SCL idle high, that appears on the bus
   as a burst of spurious START/STOP conditions with no clock edges, so no
   device latches anything. It is over before `setup()` runs. Flashing is
   likewise harmless.
2. **Core Debug Level must stay `None`.** Routing ESP_LOG to UART0 would drive
   SDA during normal operation and corrupt the bus. Nothing else does: the
   Arduino core calls `Serial0.setPins()` before `setup()`, but `uartSetPins()`
   only *stores* the pin numbers when the driver is not installed — GPIO 43/44
   are attached to the UART peripheral only by a `Serial0.begin()`, which this
   firmware never calls. Verified by reading the 3.3.11 core.

### 0.3 Confirm during bring-up

- ✅ **Whether the encoder I²C bus really has one consumer**, as §4/§7.3
  assert — confirmed by the §1.4 scan. The no-mutex conclusion holds *for the
  bus*. See §2.0's warning about `ParameterValues`, which is a different
  question and does not survive Phase 2 unchanged.
- ✅ **Free GPIOs for the handshake lines** — closed, see the table above.
- ❌ **VBUS path for the EWI Solo** — **CLOSED, and the answer is "the board
  cannot do it."** Measured 0 V at the USB-C connector with the board powered
  and running. The schematic shows `VBUS` and `+5V` as separate nets with **D3
  (IN5819 Schottky)** between them, so the 5V header pin is an output fed from
  USB and cannot back-feed VBUS; the battery input is worse still, since a LiPo
  gives no 5 V rail at all. `DRVVBUS` (GPIO-matrix signal 63) and `VBUSVALID`
  (61) exist on the S3 but LilyGO routed neither, and IDF already asserts
  `PRTPWR` — so there is no software workaround, and firmware cannot even sense
  VBUS. **5 V must be injected onto the connector's VBUS externally**, which
  then forward-biases D3 and powers the whole board from one supply. See the
  README's power section.

---

## 1. Phase 1 — UI subsystem: display + encoders ✅ COMPLETE

**Goal:** a self-contained UI firmware that renders a parameter page and responds
to all four encoders and their switches. No MIDI, no USB host, no UART MIDI.

**Why first:** it produces the display instrument that Phase 2 is verified with,
before USB host mode takes the console away (§0.1).

**All exit criteria met.** Encoders drive on-screen values immediately, switches
page correctly, redraw figures are within target, the four-encoder stress passes,
UI task and synthetic `loop()` load coexist, and the UART log sink is verified.

### 1.0 Project skeleton and toolchain ✅

Built as planned, with the sketch folder named for the Arduino IDE's
folder-equals-sketch rule:

```
spirant-hw-interface-esp32/
  spirant_hw_interface/
    spirant_hw_interface.ino     # setup() / loop() — wiring and synthetic load
    src/
      config.h                   # pins, addresses, tunables, feature flags
      log.h / log.cpp            # log sink abstraction (§1.1)
      display/
        lgfx_t_display_s3.h      # LGFX_Device subclass: ST7789 + I8080 bus
        display_manager.h/.cpp   # dirty-region renderer + profiler
      diag/
        i2c_scan.h/.cpp          # bus survey (§1.4)
      encoders/
        encoder_input.h/.cpp     # Seesaw driver, level-triggered ISR, notify
      params/
        param_specs.h            # the 6×4 parameter table
        parameter_values.h/.cpp  # runtime state, per-consumer change flags
      tasks/
        ui_task.h/.cpp
  tests/
    Makefile                     # host tests, no Arduino toolchain
    parameter_values_test.cpp
  README.md
```

The Arduino IDE compiles `src/**` recursively, so this works without a library
manifest — same as `arduino-cli` would.

**As-built deliverables:**

- **No `sketch.yaml` / `Makefile` for the firmware.** The build is driven from
  the Arduino IDE. Board settings and library versions are pinned in the
  README's tables instead. **Every board-menu default for
  `esp32:esp32:lilygo_t_display_s3` is already correct for Phase 1** —
  `USBMode=hwcdc`, `CDCOnBoot=cdc`, `LoopCore=1`, 16M/3MB-app partition.
- A headless compile check is still available without installing anything:
  Arduino IDE bundles an `arduino-cli` at
  `/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli`.
  Passing explicit menu options mostly fails (`FlashSize` is not a valid option
  for this board; PSRAM is fixed by the board definition) — just use the bare
  FQBN.
- Libraries pinned: **LovyanGFX 1.2.26**, **Adafruit seesaw Library 1.7.9**,
  **Adafruit BusIO 1.17.4**. `ESP32_Host_MIDI` is a Phase 2 dependency and is
  not yet installed.
- **No blink** — the T-Display-S3 has no user LED. The end-to-end check is the
  boot log line plus the splash screen.

**Build size as built:** 386,743 bytes flash (12%), 26,740 bytes global RAM
(8%). Free heap at boot 347,756 bytes, before ~37.4 KB of sprite allocation.

### 1.1 Log sink abstraction ✅

Built as specified, with one forced rename:

```cpp
void log_init();
void slog_printf(const char *format, ...);   // NOT log_printf
```

**`log_printf` collides with the ESP32 core** (`esp32-hal-log.h` declares
`int log_printf(const char*, ...)`), and the two declarations are ambiguous.
Hence the prefix. `log_init()` is unchanged.

Both sinks can be enabled simultaneously — `LOG_SINK_USB` and `LOG_SINK_UART`
are independent flags, which is how "identical output over both sinks" was
verified. Phase 2 sets `LOG_SINK_USB 0` and changes nothing else.

UART sink: `Serial1`, **GPIO 17 (TX) / 18 (RX)**, 115200 8N1. Physically
verified with a USB-UART adapter. Output is mutex-guarded around a single 256-byte
buffer; safe from any task, **not** from an ISR.

### 1.2 Display bring-up ✅

`LGFX_TDisplayS3` as an `LGFX_Device` subclass: `Bus_Parallel8` +
`Panel_ST7789`. Pin map, offsets and flags taken from LilyGO's board definition
rather than derived, as the plan insisted. The values that matter:

- `memory_width 240`, `panel_width 170`, **`offset_x 35`** — the classic trap,
  avoided
- `invert = true`, `rgb_order = false`, `readable = false`, `bus_shared = false`
- `PIN_POWER_ON` (GPIO 15) driven HIGH in `begin()` before anything renders. On
  this board that pin also gates the header 3V3 rail, so the Seesaw needs it too.
- `freq_write = 20 MHz` (LilyGO's value). Drop to 6 MHz if pixels ever tear.

Rendering runs in **landscape, rotation 1 → 320×170**. The four-column parameter
page needs the width.

### 1.3 Display manager and redraw profiling ✅

Dirty-region only, as specified. Layout:

```
y 0   +-------------------------------------------------+
      |  Filter                                    2/6  |  header (28)
y 29  +--------+----------+----------+----------+
      | Res    | Brite    | Floor    | Pass     |
      |   35   |    66    |   150    |    0     |          columns (122)
      |   %    |    %     |    Hz    |    %     |
y 152 +-------------------------------------------------+
      | status line                                     |  footer (16)
y 170 +-------------------------------------------------+
```

Two sprites, 16bpp, internal RAM (not PSRAM): header 320×28 and **one reusable**
80×122 column, pushed to whichever column needs updating. 37,440 bytes total.
One reusable column sprite rather than four costs nothing — all rendering is
serialised on the UI task anyway — and `waitDMA()` is called after each push
because the buffer is immediately reused.

**Measured (worst case over 200 value renders / 20 full pages):**

| Measurement | min | avg | max | Target | Result |
|---|---|---|---|---|---|
| Single value region | 1332 µs | 1422 µs | **1539 µs** | < 2000 µs | ✅ |
| Full page redraw | 11661 µs | 11668 µs | **11688 µs** | < 20000 µs | ✅ |

DMA async check (`push` vs. subsequent `waitDMA`): ______________________

**Both targets met, but note the plan's expectation of "single regions well
under 1 ms" was optimistic.** The 80×122 column is 19,520 bytes; at 20 MHz on an
8-bit bus the transfer alone is ~980 µs, so the measurement is dominated by
pixels moving, not by drawing. **If Phase 2 needs more headroom**, the lever is
shrinking the pushed region to just the value text — the label and unit do not
change on a value update — for roughly a 3× reduction. Not needed to pass §1.3.

### 1.4 I²C bus survey and pin decision ✅

`i2c_survey()` runs at boot, scans both candidate pin pairs, and logs every
address found. It skips the alternate pair at runtime if it would collide with
the log UART, saying so rather than silently doing nothing.

**Result: exactly one device, the Seesaw at 0x49.** Design doc §4/§7.3's
"exactly one consumer, no mutex needed" conclusion **holds for the I²C bus**.

Pins settled: **SDA 43, SCL 44** (that order — the swapped-order scan is what
confirmed it). `Wire.setTimeOut(50)` per design doc §4.

### 1.5 Seesaw encoders — polled ✅

`Adafruit_Seesaw` directly, per design doc §4.1. Retained as a permanent
`config.h` flag rather than a throwaway step: **`ENCODER_USE_INTERRUPT 0`** puts
the firmware back into pure 20 ms polling, which is the right first move for any
future encoder problem. Switch pins on the Seesaw are 12, 14, 17, 9 — the same
numbers the Pico driver uses, read via `digitalReadBulk` with pull-ups.

### 1.6 Level-triggered interrupt + task notification ✅

Built as specified: `enableEncoderInterrupt()` per encoder, host GPIO configured
`GPIO_INTR_LOW_LEVEL` through `gpio_config()`, `IRAM_ATTR` ISR that **disables
the interrupt as its first statement**, then `vTaskNotifyGiveFromISR`. The
service routine reads all four encoders and all four switches on every wake and
diffs against cache, which is what makes simultaneous movement impossible to
drop. Two-encoder concurrency and the 60 s four-encoder spin both pass.

**Three implementation details this plan did not anticipate:**

1. **`Adafruit_seesaw` cannot clear the interrupt flag.** Its `read8`/`read32`
   register accessors are `protected`, and it exposes no flag read at all. So
   the read-to-clear is a **direct `Wire` transaction** to `[SEESAW_GPIO_BASE,
   SEESAW_GPIO_INTFLAG]` = `[0x01, 0x0A]` — the same two-byte register address
   the Pico driver uses — with a 250 µs gap between the address write and the
   4-byte read. Without this the INT line never deasserts.
2. **Seesaw GPIO interrupts for the switches default to OFF**
   (`SEESAW_GPIO_INTERRUPTS 0`), contradicting this plan's §1.6. The shipped
   Pico firmware deliberately keeps the buttons off the shared INT line so a
   press cannot perturb the rotation path; the buttons are polled on the UI tick
   instead. The flag enables the plan's version without editing any module, and
   the UI behaves identically either way.
3. **`present_` is set at the very end of `begin()`.** `begin()` runs on the
   loopTask while the UI task is already ticking; `service()` and `rearm()`
   no-op until that flag is set, which is what stops the two from interleaving
   I²C transactions during bring-up.

**Two safeguards added beyond the plan**, both counted and reported in the stats
line:

- **Glitch rejection.** A position jump larger than 64 counts in one service
  window is treated as a corrupted read: the cache resyncs and the delta is
  dropped. Applying such a jump would slam a parameter to its rail, which is far
  more visible than dropping one bogus event.
- **Bounded re-fires.** If the INT line will not deassert after 64 consecutive
  services, `rearm()` stops re-arming and lets the 20 ms tick drive polling.
  This is the difference between the plan's warned-about lockup and a degraded
  but working instrument.

### 1.7 ParameterValues model ✅ — **model changed, read this**

**This plan's §1.7 was stale and was not followed.** It described "16 parameters
across 4 pages of 4" with "plain 7-bit `uint8_t` 0–127".

The actual shipped model is `spirant-parameter-values-rs`, which the Daisy v1
firmware already speaks to:

- **6 pages** of 4 slots mirroring the Daisy v0 signal chain — Oscillator,
  Filter, Overdrive, Chorus, Delay, Reverb
- **24 slots, 19 active, 5 null** (indices 2, 3, 10, 11, 23)
- **`int32_t` values in real engineering units, not 0–127** — Cutoff Floor
  20–500 Hz step 5, Delay Time 40–750 ms step 5, Damp LP 1000–18000 Hz step 250,
  LFO Rate in centi-Hz, Damping as a per-mille coefficient
- per-slot `min`/`max`/`default`/`step` from a static `ParamSpec` table

`src/params/param_specs.h` is a C++ port of that table. **It is a cross-language
contract in three places** — the Rust `PARAM_SPECS`, this header, and
`spirant-daisy/v1/comm/param_table.h`. Changing a range is a three-firmware
change; the sync contract is stated at the top of each file.

Change flags renamed as planned: `changed_display` / `changed_midi`. Encoder
changes set both; `updateFromMidi()` sets only `changed_display`, which is the
echo guard. Clamping at the ends, no wrapping.

**Page navigation differs from this plan.** §1.7 said "encoder switch → page
change". As built, matching the shipped Pico firmware: **encoder 0's switch pages
backward, encoder 3's pages forward**, both wrapping, 40 ms debounce. Switches 1
and 2 are unassigned. Six pages make backward navigation worth having.
Configurable via `PAGE_BACK_SWITCH` / `PAGE_FWD_SWITCH`.

**Consequence for Phase 3:** because values are real units, the parameter → CC
102–119 mapping must **scale into 0–127 on send and back on receive**. It is not
a pass-through. This is new work that this plan's original 0–127 model would
have avoided.

**Exit met:** 11 host test groups covering clamping, page mapping, flag
semantics, null slots, wrapping and buffer-capacity behaviour. Run with
`make -C spirant-hw-interface-esp32/tests run` — system compiler, no Arduino
toolchain, because `src/params/` is deliberately free of `Arduino.h`.

### 1.8 Split the UI task ✅

`ui_task` on **core 1, priority 3**, stack **8192 bytes**. The Arduino loopTask
is core 1 priority 1, so the priority relationship under test is real. The task
blocks on `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20))`, so it wakes on a Seesaw
interrupt *or* on the 20 ms tick — the tick is what polls the push-buttons.

**Display writes happen only on this task.** The one place that violated it —
an encoder-failure message drawn from `setup()` — was moved into the task's
stats tick.

**Gotcha:** `xTaskCreatePinnedToCore`'s stack argument is in **bytes** under
ESP-IDF's FreeRTOS, not words as in vanilla FreeRTOS.

Synthetic load: `loop()` blocks on `vTaskDelayUntil` for 2 ms, then burns 500 µs,
measuring its own scheduling lateness. It blocks rather than spins because a
spinning priority-1 task starves core 1's idle task and trips the watchdog,
which would have told us nothing about priorities. Both directions confirmed: the
UI task services encoders promptly under load, and the load is not starved by
redraws.

Live figures on the footer and in the stats heartbeat:

```
stats: wakes=.. svc=.. isr=.. detents=.. glitch=.. i2cerr=.. lat=..us val=..us page=..us load_late=..us/..
```

`lat` is ISR-to-service latency; `load_late` is the synthetic load's worst
lateness.

Recorded from the 60 s stress: detents ______ glitches ______ i²c errors ______
worst latency ______ µs, worst load lateness ______ µs.

### 1.9 Deviations from this plan — summary

| # | Plan said | Built | Why |
|---|---|---|---|
| 1 | I²C on 17/18, connector unused | **I²C on 43/44 (connector), log UART on 17/18** | The trade-off was false — `Serial1` routes through the GPIO matrix, so the console needs no specific pins (§0.2) |
| 2 | 16 params, 4 pages, `uint8_t` 0–127 | **19 active params, 6 pages, `int32_t` real units** | Matches the shipped Rust model and the Daisy's table; one source of truth beats three (§1.7) |
| 3 | Any encoder switch → page change | **Enc 0 back, enc 3 forward, wrapping** | Matches shipped Pico behaviour; six pages need backward nav |
| 4 | `setGPIOInterrupts()` for switches | **Off by default, flag to enable** | Pico firmware deliberately keeps buttons off the shared INT line |
| 5 | `log_printf()` | **`slog_printf()`** | Collides with the ESP32 core's own `log_printf` |
| 6 | `Makefile` / `sketch.yaml`, `arduino-cli` | **Arduino IDE; settings pinned in README** | Chosen toolchain. IDE bundles an `arduino-cli` for headless compile checks |
| 7 | Blink + serial hello | **Boot log + splash screen** | No user LED on this board |
| 8 | — | **Glitch rejection, bounded INT re-fires** | Added: turn a corrupt read into a dropped event and a stuck line into degraded polling, rather than a slammed parameter or a lockup |

---

## 2. Phase 2 — USB MIDI host in, displayed on screen

**Goal:** the EWI Solo enumerates over USB and its live performance data is
visible on the display. **MIDI output is explicitly out of scope** — nothing is
sent to the Daisy, no TRS verification attempted.

**Depends on:** Phase 1 ✅ — the display as instrument, and the UART log.

### 2.0 What Phase 1 hands you, and what will bite

Read before writing any Phase 2 code.

#### 🛑 The one that will break your hardware: `Serial` remaps to the I²C bus

**Read this before changing any board setting.**

Host mode requires **USB CDC On Boot → `Disabled`**. In the ESP32 core
(`HardwareSerial.h:449`):

```c
#else   // !ARDUINO_USB_CDC_ON_BOOT -- Serial is used from UART0
#define Serial Serial0
```

So disabling CDC-on-boot silently remaps `Serial` to **UART0 — GPIO 43/44,
which is now the I²C bus** (§0.2). If `LOG_SINK_USB` is still `1` when you flip
that setting, `log_init()` calls `Serial.begin(115200)`, which installs the UART
driver and *attaches the peripheral to those pins*. Every subsequent log line
then drives SDA, and the encoders stop working.

The symptom — "encoders died when I enabled host mode" — points nowhere near the
cause. **The board setting and the `config.h` flag must change together.**

Make it impossible rather than remembering it. Add to `log.cpp`:

```cpp
#if LOG_SINK_USB && !ARDUINO_USB_CDC_ON_BOOT
#error "LOG_SINK_USB=1 with CDC-on-boot disabled maps Serial to UART0 on \
GPIO 43/44 -- the I2C bus. Set LOG_SINK_USB 0."
#endif
```

This failure mode did not exist when the plan assumed I²C on 17/18. It is the
direct cost of the §0.2 resolution, and Phase 2 is where it detonates.

#### Flipping the debug path

Otherwise trivial: **`LOG_SINK_USB 0`** in `config.h`. The UART sink is already
on GPIO 17 at 115200 and already physically verified, so there is nothing to
discover. Call `slog_printf()`, not `log_printf()`.

Board settings for host mode: **USB Mode → `USB-OTG (TinyUSB)`**, **USB CDC On
Boot → `Disabled`**. Confirm manual bootloader entry (BOOT + RST) and a flash
**while still in device mode**, before you need it.

#### ⚠️ Core Debug Level must stay `None`

Same root cause. GPIO 43 is both U0TXD and I²C SDA, so routing ESP_LOG output to
UART0 would drive SDA during operation and corrupt the encoder bus.

#### Shared state: what Phase 2 actually introduces

Design doc §7.3's "exactly one consumer, no mutex needed" was confirmed in §1.4
**for the I²C bus**. It says nothing about in-memory state.

`ParameterValues` is **not** the concern here. Phase 2 handles *performance*
data — notes, breath, bend — which never touches it. It stays single-writer
(the UI task) until parameter CCs arrive in Phase 3/5; the queue-versus-mutex
question belongs there, not now.

What Phase 2 does introduce is a **new** shared struct with the same shape:
monitor state written by the drain in `loop()`, read by the UI task to render.
It is an easier instance — the semantics are "latest value wins", so a plain
snapshot suffices and no queue is needed. Do not build the queue machinery here.

#### Rendering: what the §1.3 numbers already settle

`DisplayManager` currently renders **parameter pages only**: `update()` consumes
display change flags and repaints either the whole page or individual columns.
The §2.4 monitor page is a different screen — see §2.4 for the screen-concept
refactor, which is small if done first and annoying if bolted on.

Useful pieces already there: `drawStatus()` (printf-style footer),
`invalidate()` (force full repaint), `worstValueUs()` / `worstPageUs()` (live
worst-case counters), and the reusable 80×122 column sprite.

**Measured effective transfer rate: 19,520 bytes in ~1,420 µs ≈ 13.9 MB/s.** Use
that to size regions — cost is driven by pixel area, not by how much content
changed. §2.4 carries the resulting budget.

#### Turn off the synthetic load

`SYNTHETIC_LOAD_ENABLED 0` once the real MIDI drain occupies `loop()`. It has
served its purpose; leaving it on would compete with the thing it was
simulating. Keep the `ui_task::reportLoad()` hook — pointing it at the real
drain loop's lateness gives you the same instrumentation for free.

#### Pins still free

GPIO 1, 2, 3, 10, 11, 12, 13, 21.

### 2.1 Host port and power characterisation

You've confirmed the board hosts; capture the specifics, since they constrain
everything downstream:

- how VBUS reaches the device, and whether the EWI is bus-powered or needs
  external supply
- **measured EWI current draw** against whatever the supply path can deliver
- whether hosting and the programming port can coexist, or whether flashing means
  unplugging the instrument (affects iteration speed for the rest of the phase)

**Phase 1 gives you a free instrument here.** The Seesaw is powered from the
header 3V3 rail, gated by `PIN_POWER_ON` (GPIO 15). If VBUS draw sags that rail,
the **`i2cerr` counter in the stats heartbeat will show it** before anything
else does. Watch that counter, and the display for artifacts, *while the EWI is
plugged in and drawing current* — not just at idle.

**Exit:** documented, repeatable physical setup for host-mode testing, with the
encoder bus confirmed clean under EWI load.

### 2.2 USB host enumeration — known-good device first

Bring up `ESP32_Host_MIDI` (`USBConnection` / `USBMIDI2Connection`) with a
**generic class-compliant USB MIDI keyboard**, not the EWI. This separates "does
the library work here" from "does the EWI behave as expected" — two very
different debugging problems.

Log enumeration: VID/PID, MIDI 2.0 UMP vs. MIDI 1.0 fallback, endpoint
descriptors, polling interval.

Pin the library version in the README's table alongside the other three.

**Repeat a Phase 1 lesson: do not publish "connected" until the endpoints are
actually usable.** §1.6 shipped a bug of exactly this shape — the encoder
driver set its `present_` flag mid-`begin()`, which let the UI task issue I²C
transactions into a half-initialised driver. USB enumeration state shared with
the UI task has the same structure and the same trap.

**Exit:** device enumerates, holds for 10 minutes, raw 4-byte USB-MIDI packets
arriving in the ring buffer.

### 2.3 MIDI parse and dispatch

Drain the ring buffer in `loop()` on core 1 via `.task()` / `processQueue()`;
parse into note on/off, CC, and pitch bend. Keep the ring buffer as the **sole**
cross-core boundary per design doc §5 — do not add a second one. (The
parsed-message queue recommended in §2.0 is a same-core hand-off to the UI task,
not a second cross-core boundary.)

**⚠️ The drain loop must block, not spin.** Phase 1 hit this: a `loop()` that
polls without yielding starves core 1's idle task and trips the task watchdog.
That is why §1.8's synthetic load uses `vTaskDelayUntil` rather than a busy
poll. If `.task()` / `processQueue()` is a tight poll, wrap it the same way. The
failure presents as a random reboot, not as a scheduling problem, so it costs
more to diagnose than to prevent.

Keep `ui_task::reportLoad()` and point it at the real drain's lateness — same
instrumentation as §1.8, no new code.

Instrument for **ring buffer overruns**: the stock 64-entry buffer against a
sustained breath stream is the most likely place to silently drop data.

**The margin can be computed now, before writing any code.** The UI task at
priority 3 preempts the drain at priority 1, and its longest critical section is
the §1.3 full-page redraw:

| Quantity | Value |
|---|---|
| Worst-case UI-task stall of the drain | **11.7 ms** (full page redraw) |
| 64-entry ring buffer at §6.4's assumed 2 ms CC2 rate | **128 ms** |
| Margin | **≈ 11×** |

Comfortable, and it substantially de-risks the overrun entry in §7 before the
first line of Phase 2. §2.5 re-checks it against the measured rate: at a 500 µs
CC2 rate the buffer holds only 32 ms against the same 11.7 ms stall — a 2.7×
margin, which is where resizing starts to matter.

#### ⚠️ Amended after building it — the 11× is the wrong headline

The 64-entry figure is right: `USBConnection::QUEUE_SIZE` really is 64. But
**2.7× is the number to plan against, not 11×**, and three findings tighten it
further:

1. **The ring is not resizable.** `QUEUE_SIZE` is a `protected static const int`
   in the library. "Resize it if the measurement says otherwise" is not
   available without patching `ESP32_Host_MIDI` itself.
2. **Overruns are not countable.** `enqueueMidiMessage()` discards silently when
   full — it returns `false` and its only caller ignores the return. The
   "overrun counter on display" this plan promises **cannot be built** from
   outside the library. As-built substitutes the ring's high-water mark and a
   count of times it came within one slot of full, which warn *before* loss
   rather than after. The §2.5 exit criterion "zero ring-buffer overruns" has to
   be read as "high-water never approached capacity".
3. **A second queue was nearly introduced.** The library's documented
   `MIDIHandler` path (§2.2/§2.3) is a second buffer downstream of the ring,
   defaulting to **20 events** — below the ring's 64, so it would have silently
   become the binding constraint. As-built bypasses it; see §2.6.

Levers if `nearfull` is ever non-zero, cheapest first: shrink the worst-case UI
stall (the full-page repaint dominates it), move the drain to core 0, patch
`QUEUE_SIZE`.

**Exit:** parsed messages logged with correct channel, number, and value.

### 2.4 MIDI monitor page — the Phase 2 deliverable

A display page showing:

- last note on/off and current active note
- **breath CC2 as a live bar** — the most informative element, CC2 being the
  design's central signal
- pitch bend as a centred bar
- any other CC seen, with its number
- message rate (msg/s), ring-buffer high-water mark, overrun count
- USB connection state

All rendering goes through the Phase 1 dirty-region path, on the UI task. The
320×170 landscape geometry, the 28 px header and the 16 px footer are already
established; reuse them so the two screens feel like one instrument.

#### No separate rate limiter is needed

The plan originally called for rate-limiting the redraw to 20–30 Hz
independently of MIDI arrival rate. **The UI task's existing 20 ms tick already
is that limiter**, at 50 Hz, and the §1.3 measurements show the whole page fits
inside it. Budgeting at the measured 13.9 MB/s:

| Element | Region | Bytes | Cost |
|---|---|---|---|
| Note / active note | 100×30 | 6,000 | ~430 µs |
| CC2 breath bar | 280×24 | 13,440 | ~970 µs |
| Pitch bend bar | 280×16 | 8,960 | ~645 µs |
| Stats line | existing `drawStatus()` | — | small |
| **Full monitor refresh** | | | **~2.0 ms** |

That is **10% duty at the 20 ms tick**, 6% at 30 Hz. Render on the tick and
delete the rate-limiter requirement — one less mechanism, and the display
cannot become a bottleneck on the parse path because it is not on that path at
all.

Plain full-region sprite pushes are therefore affordable; no incremental
rendering is required. (Redrawing only the changed segment of a bar is roughly
40× cheaper and is the lever if you ever want a bar to track the full ~500 Hz
breath rate rather than the tick. Not needed here.)

#### Screen selection — RESOLVED

Phase 1 uses encoder 0 / encoder 3 switches for the six-page parameter rotation.
**Switches 1 and 2 were unassigned**, deliberately, and this is what they were
for.

**As built: switch 2 toggles parameter page ↔ monitor** (`SCREEN_TOGGLE_SWITCH`).
Switches 0 and 3 keep paging parameters and do nothing while the monitor is up —
paging a hidden screen would surprise the player on the way back. Switch 1 stays
unassigned for Phase 3 or 4. The monitor is not a seventh page: six is already a
long rotation and it is not a parameter page.

This forces a small refactor — `DisplayManager::update()` currently takes
`ParameterValues&` directly, so it needs an explicit screen concept with each
screen pulling the state it needs. **Do this first, before writing the monitor
page**; it is a small change up front and an annoying one afterwards. Note that
a screen switch uses the full-page path: ~11.7 ms, so switches should be
deliberate, not incidental.

**Exit:** playing the keyboard produces correct, responsive on-screen feedback.

### 2.5 EWI Solo substitution and characterisation

Swap in the real instrument and **measure what it actually sends** — data the rest
of the project currently assumes:

- confirms MIDI 1.0 enumeration (design doc §5 expects this)
- **measured CC2 update rate** — §6.4 budgets on an assumed ~2 ms; verify
- whether it uses running status on the wire
- exact CC set and channel(s)
- note-transition behaviour and pitch-bend range

**Exit criteria for Phase 2 overall:**

- EWI Solo enumerates and holds through a 10-minute continuous play session
- Display shows live notes, breath, and bend with no perceptible lag
- Zero ring-buffer overruns under sustained playing
- Hot-unplug and replug recovers cleanly without a reset
- Measured EWI rates written back into the design doc, and §6.4's bandwidth
  budget re-checked against real numbers
- **§2.3's stall margin recomputed against the measured CC2 rate**, not just the
  bandwidth budget. The ring buffer must still cover the 11.7 ms worst-case UI
  stall with margin; resize it if the measurement says otherwise — but see
  §2.3's amendment: the ring is not resizable without patching the library, so
  the levers are elsewhere.

### 2.6 Deviations from this plan — Phase 2 summary

| # | Plan said | Built | Why |
|---|---|---|---|
| 1 | `MIDIHandler` + `addTransport()` + `getQueue()` (§2.2, §2.3) | **Only `USBConnection`; our own callback, parser and state** | `MIDIHandler` is a second queue on the hot path, capped at 20 events — below the ring's 64, so it becomes the binding constraint. Its `MIDIEventData` also carries four `std::string`s in a `std::deque`, i.e. thousands of heap allocations/second at breath rates. And our parser is host-testable; theirs is not. Argument in full at the top of `src/midi/usb_midi_host.h` |
| 2 | "instrument for ring buffer overruns" (§2.3) | **High-water mark + near-full count** | The library discards silently and reports nothing; an exact count is not observable from outside it (§2.3 amendment) |
| 3 | ≈11× ring margin (§2.3) | **≈2.7× against §2.5's own pessimistic rate**, and the ring is not resizable | The 11× used the optimistic CC2 rate. Corrected in §2.3 |
| 4 | "latest-value-wins snapshot, no queue needed" (§2.0) | **Snapshot behind a FreeRTOS mutex, taken once per drain batch** | Correct, but the obvious lock-free implementation is a trap: a seqlock's reader spins until the writer finishes, and here the reader is the *higher-priority task on the same core*. Preempting the writer mid-update hangs it permanently. A mutex is right because it does priority inheritance |
| 5 | One `#error` guard, `LOG_SINK_USB` vs CDC-on-boot (§2.0) | **Five**, four of them in `config.h`: `ARDUINO_USB_MODE`, and CDC / MSC / DFU **On Boot** | §2.0 saw only half the hazard. The core gates all USB *device* start-up on `!ARDUINO_USB_MODE`, so selecting OTG — which hosting requires — is exactly what arms CDC/MSC/DFU-on-boot; each ends in `USB.begin()` and claims the sole OTG controller. CDC On Boot is the **menu default**. All five verified to fire. Placed in `config.h` so they precede any library include, after one was masked by a library header failure |
| 6 | `DisplayManager::update()` gains a screen concept (§2.4) | **Done first, as advised.** `DisplayManager` owns panel + 3-sprite bank + timing; `ParamScreen` / `MonitorScreen` pull their own state | As §2.4 predicted, small up front. `worstValueUs()` now ignores pushes made during a full repaint so its §1.3 meaning survives |
| 7 | `SYNTHETIC_LOAD_ENABLED 0` (§2.0) | **Removed entirely** | A dead flag competing with the thing it simulated. `ui_task::reportLoad()` survives and carries the real drain's lateness |
| 8 | — | **Note-on with velocity 0 normalised to note-off in the parser** | Conformant sources release that way; without it the active-note count only ever rises. Tested end-to-end |
| 9 | — | **Held notes released on USB disconnect** | Otherwise an unplug mid-phrase latches a note forever, which is exactly what the hot-replug exit criterion would catch late |
| 10 | "measured CC2 update rate" (§2.5) | **Reported as a count per second, not as a minimum inter-message interval** | The drain folds a whole batch in with one timestamp, so an interval measurement is blind inside a burst and would read long. Both are reported; the per-second count is the one §6.4 should be updated from |
| 11 | USB Mode → "USB-OTG (TinyUSB)", CDC On Boot → "Disabled", `LOG_SINK_USB 0` (§2.0) | **USB Mode → "Hardware CDC and JTAG", CDC On Boot → "Enabled", `LOG_SINK_USB 1`** — every board-menu default | §2.0's premise was wrong: USB Mode picks which controller drives Arduino's *device-side* `Serial`, not whether the OTG controller can host. Hardware CDC compiles out `USB.begin()` entirely, so no device stack can claim the controller, and `Serial` becomes `HWCDCSerial` instead of UART0 — which removes the I²C hazard **and** hands back a USB console from boot until the PHY handover. Matches `ESP32_Host_MIDI`'s own tested example |
| 12 | — | **`Serial.setTxTimeoutMs(0)` in `log_init()`** | The CDC console dies mid-boot when the host driver takes the PHY. HWCDC's disconnected path drops rather than blocks, but the transition window can wait ~2 s — unacceptable inside the core-0 USB task during enumeration |
| 13 | — | **Board power latch moved to the first statement of `setup()`** | GPIO 15 latches the 3V3 regulator; on battery the board browns out without it. It was being set inside `DisplayManager::begin()`, *after* `log_init()`'s up-to-1.5 s USB wait. Latent while `LOG_SINK_USB` was 0; a live bug the moment it went back to 1 |

**What §2.6 does *not* claim.** Nothing here has run on hardware. Verified:
host tests (250 checks across the parser and the monitor state), a clean compile
at the phase 2 board settings, and both `#error` guards firing. Unverified:
every measurement in §2.1, §2.2 and §2.5, and every rendering claim in §2.4.

---

## 3. Phase 3 — TRS MIDI output and routing

First phase with no display-only verification path, hence deferred. Build it,
review it, verify in Phase 5 (or earlier if a MIDI interface appears).

- `UARTConnection` TX-only at 31250 baud, `rxPin = -1`. Pick TX from the free
  list in §0.2 — **not** 43/44, which are the I²C bus.
- MIDI-thru path: USB ring buffer → parse → output queue
- **Priority queue policy** (§6.1): performance data ahead of pending parameter CCs
- **Running-status compression** on send (§6.4)
- **Encoder CC throttling** to ~15–20 Hz per encoder, on value change only (§6.2)
- Parameter → CC 102–119 mapping table, single source of truth shared with the
  display labels. **This must scale between real engineering units and 0–127 in
  both directions** — see §1.7. `param_specs.h` already carries the `min`/`max`
  each slot needs for that conversion, so the mapping table should derive the
  scaling from it rather than hard-coding a second copy of the ranges.

Build in a **loopback self-test** (TX to a free RX pin) plus a byte-level send log
rendered to the display. Not electrically conclusive, but it catches
message-construction bugs with no external gear — worth the hour, given
verification is otherwise deferred to Phase 5.

**Exit:** loopback echoes byte-identical to intent; running-status compression
measurably reduces byte count on a sustained CC2 run; round-tripping a value
through the unit↔CC scaling is stable (no drift on repeated conversion).

## 4. Phase 4 — Boot handshake

ESP32 side only; Daisy stubbed.

- Pin assignment from §0.2's free list — eight pins available, so this is no
  longer constrained. Closes design doc §2.5.
- Open-drain with pull-ups, few-ms debounce (§2)
- State machine: `BOOT_INIT` → `WAIT_DAISY_READY` → `WAIT_DUMP_ACK` → `RUNNING`
- 250 ms ACK timeout, **bounded** retry, and a defined terminal behaviour once
  retries are exhausted — §2 says "bounded" but not what follows. Recommend
  proceeding to `RUNNING` degraded with a persistent on-screen warning, so the
  instrument is never bricked by a Daisy that didn't boot.
- Compiled-in default patch as the ESP32's boot-time source of truth (§2.3).
  `ParameterValues` already boots every slot to its `ParamSpec` default and has
  `markAllChangedMidi()` for exactly this full-dump case.
- Handshake state surfaced on the display

Test with **jumper wires simulating the Daisy** (manually assert/pulse the lines),
plus a spare MCU for automated timing tests. Exercise both timeout and success
paths.

**Not in scope:** `RESYNC_REQUEST` (§2.4, explicitly deferred), SD-card patch
storage (v2).

## 5. Phase 5 — Integration

Real Daisy on the other end. Full boot handshake, encoder → CC → audible parameter
change, EWI → TRS → audio. Latency and jitter tuning against §7's priority model.
Update the design doc to as-built, per your usual practice.

---

## 6. Dependency graph

```
1.0 skeleton                                          ✅ PHASE 1 COMPLETE
 ├─ 1.1 log sink ─────────────────────────────────────┐
 ├─ 1.2 display ── 1.3 redraw profiling ──────────┐   │
 └─ 1.4 i2c survey ── 1.5 seesaw poll ── 1.6 ISR ─┤   │
                                                  └─ 1.7 params ── 1.8 UI task
                                                                     │
              ┌──────────────────────────────────────────────────────┘
              │
        2.0 log flip ── 2.1 host power ── 2.2 enumerate ── 2.3 parse
                                                            └─ 2.4 monitor page
                                                                 └─ 2.5 EWI
                                                                      │
                                                3. TRS out ───────────┤
                                                4. handshake ─────────┤
                                                                      └─ 5. integration
```

Phases 3 and 4 are independent and can be built in either order.

---

## 7. Risk register

### Retired in Phase 1

| Risk | Outcome |
|---|---|
| I²C pins collide with UART0, forcing a repin mid-project | **Resolved, but not as recommended.** I²C took the Qwiic connector at 43/44; the log UART moved to 17/18. The recommended trade-off turned out to be false (§0.2). New standing constraint: Core Debug Level must stay `None`. |
| Level-triggered ISR re-fires and locks the device | **Not observed.** Disable-on-entry works; a bounded-re-fire fallback to polling was added as a backstop. Note the flag clear needs a direct `Wire` transaction — `Adafruit_seesaw` cannot do it (§1.6). |
| ST7789 panel offset / inversion eats a day | **Did not materialise.** Taking LilyGO's numbers rather than deriving them was the right call. |
| GPIO budget (13 usable) over-subscribed | **Retired.** 5 used, 8 free, 3 committed needs remaining (§0.2). |
| Losing USB serial in host mode stalls debugging | **Mitigated as planned.** UART sink built and physically verified on GPIO 17 before any USB work. |

### Active

### Retired in Phase 2 (code)

| Risk | Outcome |
|---|---|
| **Disabling CDC-on-boot remaps `Serial` to UART0 — the I²C bus — and kills the encoders** | **Retired.** `#error` in `log.cpp` binds `LOG_SINK_USB` to `ARDUINO_USB_CDC_ON_BOOT`. Verified by compiling the bad combination |
| **A wrong USB board setting silently prevents hosting** | **Retired.** Three `#error` guards in `config.h` reject CDC / MSC / DFU **On Boot** *when USB Mode is OTG*, plus `log.cpp`'s existing `Serial`-remap guard. Not in the original register — §2.0 named only the `Serial`-remap half. All four verified to fire, and all four USB configurations checked |
| **USB Mode was over-constrained, blocking the recommended configuration** | **Corrected.** The first version of these guards required `ARDUINO_USB_MODE == 0` and rejected "Hardware CDC and JTAG" outright, on the false premise that OTG mode was needed to host. USB Mode selects only which controller Arduino uses for its *device-side* `Serial`; `usb_host_install()` takes the OTG controller either way. Hardware CDC is now recommended — it makes the device-stack conflict structurally impossible **and** keeps `Serial` off the I²C pins, restoring USB logging (§2.6 #11) |
| Non-yielding MIDI drain loop trips the task watchdog | **Avoided by construction.** `loop()` blocks on `vTaskDelayUntil` at `MIDI_DRAIN_PERIOD_MS`; `USBConnection::task()` is a non-blocking ring drain, so there is nothing to busy-poll |
| USB state published before endpoints are usable | **Handled, and the trap was real.** The library's own `dispatchConnected()` fires after `_processConfig()` whether or not it found a usable endpoint. As-built distinguishes `usb_device_present` from `usb_endpoint_ready` and the header shows which you have |

### Active

| Risk | Phase | Mitigation |
|---|---|---|
| ESP_LOG on UART0 corrupts the I²C bus | standing | Core Debug Level stays `None`; nothing calls `Serial0.begin()` (§0.2) |
| Monitor state shared between the drain and the UI task | 2.4 | Latest-value-wins snapshot behind a priority-inheriting mutex, taken once per drain batch. **A seqlock would deadlock here** — see §2.6 #4. `ParameterValues` is untouched in Phase 2 and stays single-writer (§2.0) |
| **64-entry ring buffer overruns on sustained breath data** | **2.3 / 2.5** | **Still active, and tighter than first computed: ≈2.7× against §2.5's pessimistic 500 µs rate, the ring is not resizable, and exact overruns are not countable.** High-water mark and near-full count reported instead; recompute against the measured rate in §2.5 (§2.3 amendment) |
| Flashing requires unplugging the instrument, slowing iteration | 2.1 | Characterise early; consider OTA if it proves painful |
| `ESP32_Host_MIDI` unreliable even though the board hosts | 2.2 | Known-good keyboard first isolates library from instrument; TinyUSB host as fallback. **Note the fallback is now more expensive than planned** — the drain calls `USBConnection` directly rather than through an abstraction, so a backend swap touches `usb_midi_host.cpp` rather than one implementation file |
| Enumeration logging blocks the library's core-0 USB task | 2.2 | ~200 ms of blocking UART writes at enumeration only, with no transfer in flight. `USB_MIDI_LOG_ENUMERATION 0` if it ever matters |
| Library's own SysEx reassembly allocates (`std::vector`) on the USB task | 2.3 | Not on our path — SysEx goes to a callback we do not register. Watch for it only if a device sends SysEx continuously |
| Real-unit ↔ CC 0–127 scaling introduces rounding drift | 3 | Derive scaling from `param_specs.h` ranges, single source; round-trip test in the §3 exit criteria (§1.7) |
| Three-way parameter table drift (Rust / ESP32 / Daisy) | standing | Sync contract documented in all three files; host tests assert the table's shape (§1.7) |
| Deferred MIDI-out verification hides bugs until Phase 5 | 3 | Loopback self-test + byte-level send log to display |

---

## 8. Design doc amendments needed

**Ready to apply now, from Phase 1:**

1. ✅ **§4 — I²C pins.** The Qwiic connector is on **GPIO 43/44** (SDA 43,
   SCL 44), and I²C uses it. The log UART moved to 17/18. Record that the
   "keep 43/44 for UART0" trade-off was a false premise, and record the two
   standing consequences: ROM-bootloader chatter on SDA at reset (harmless) and
   Core Debug Level must stay `None` (not harmless).
2. ✅ **§4 / §7.3 — "exactly one consumer, no mutex needed".** **Confirmed for
   the I²C bus**: the scan found only the Seesaw at 0x49. Add the qualifier that
   this is a statement about the bus, not about `ParameterValues`, which gains a
   second writer in Phase 2 (§2.0).
3. ✅ **§2.5 — GPIO open item.** Closed. 5 pins committed, 8 free (1, 2, 3, 10,
   11, 12, 13, 21).
4. ✅ **§7.4 — redraw times.** Replace the risk with: value region max
   **1539 µs** (target 2000), full page max **11688 µs** (target 20000). Note
   the transfer-bound nature of the figure and the region-shrinking lever.
5. ✅ **§6.3 — parameter representation.** The design doc's "plain 7-bit 0–127"
   does not match the shipped model. Values are `int32_t` in real engineering
   units across 6 pages / 19 active slots; the CC mapping must scale in both
   directions (§1.7).
6. ✅ **§4.1 — Seesaw driver.** Record that `Adafruit_seesaw` keeps its register
   accessors `protected` and exposes no interrupt-flag read, so clearing the
   INTFLAG requires a direct `Wire` transaction to `[0x01, 0x0A]`.

**Still open, pending later phases:**

7. ⏳ **§5** — add the USB-Serial/JTAG vs. USB-OTG pin-sharing consequence, and
   the working host-port configuration once §2.1 documents it.
8. ⏳ **§6.4** — replace the assumed ~2 ms breath rate with the §2.5
   measurement.
9. ⏳ **§2** — specify terminal behaviour after ACK retries are exhausted
   (recommendation in §4: degraded `RUNNING` with a persistent warning).

**Ready to apply from Phase 2's code, once there is a document to apply them
to:**

10. ⏳ **§5 — the ring buffer is the cross-core boundary, and it is fixed at 64
    entries.** Record that `USBConnection` runs its own polling task on core 0
    at priority 5, that its ring is a `protected static const` and therefore not
    tunable, and that a full ring discards silently with no notification. The
    "sole cross-core boundary" requirement is met, but only because
    `MIDIHandler` is bypassed — record that too, with the reasoning (§2.6 #1).
11. ⏳ **§7.3 — shared state.** The qualifier added in item 2 needs a second
    part: Phase 2's shared struct is the monitor snapshot, not
    `ParameterValues`, and its correct primitive is a priority-inheriting mutex
    rather than any lock-free scheme, because the reader is the higher-priority
    task on the same core (§2.6 #4).
12. ⏳ **§6.3 — no running status on USB.** USB-MIDI event packets always carry
    a status byte, so §2.5's running-status question is structurally N/A for the
    USB input. It remains live for Phase 3's TRS output, which is a real MIDI
    wire.

**First, though: decide what `esp32_firmware_design.md` is.** It does not exist
(see the note under the phase table). Items 1–12 have accumulated against a
document that may never have been written. Either write it from this plan's
as-built sections, or delete the citations and treat this plan plus the README as
the record.
