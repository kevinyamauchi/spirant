#pragma once

// Single source of truth for pins, addresses, tunables and feature flags.
// Implementation plan section 1.0. Anything you might want to change on the
// bench belongs here, not scattered through the modules.

#include <stdint.h>

// ---------------------------------------------------------------------------
// Board: LilyGO T-Display-S3 (ESP32-S3, 1.9" ST7789 170x320, 8-bit I8080 bus)
// ---------------------------------------------------------------------------
// These are fixed by the PCB -- do not change them unless you have a different
// board revision in hand. Taken from LilyGO's own T-Display-S3 pin definitions
// (plan section 1.2: take the pin map from the vendor, do not derive it).

#define PIN_LCD_D0 39
#define PIN_LCD_D1 40
#define PIN_LCD_D2 41
#define PIN_LCD_D3 42
#define PIN_LCD_D4 45
#define PIN_LCD_D5 46
#define PIN_LCD_D6 47
#define PIN_LCD_D7 48

#define PIN_LCD_WR 8
#define PIN_LCD_RD 9
#define PIN_LCD_DC 7
#define PIN_LCD_CS 6
#define PIN_LCD_RST 5
#define PIN_LCD_BL 38

// Board power enable. Must be driven HIGH before the panel will show anything
// (plan section 1.2). Also gates the header 3V3 rail on this board, so the
// Seesaw needs it too.
#define PIN_POWER_ON 15

// The 170px-wide panel sits on a 240px-wide ST7789 controller. This offset is
// the classic trap called out in the plan's risk register.
#define LCD_MEMORY_WIDTH 240
#define LCD_MEMORY_HEIGHT 320
#define LCD_PANEL_WIDTH 170
#define LCD_PANEL_HEIGHT 320
#define LCD_OFFSET_X 35
#define LCD_OFFSET_Y 0

// Parallel bus write clock. LilyGO's examples run 20 MHz. If you see torn or
// speckled pixels, drop to 6000000 before suspecting anything else.
#define LCD_BUS_FREQ_HZ 20000000

// 1 or 3 -> landscape 320x170 (the four-column parameter page needs the width).
// 1 and 3 differ only in which physical edge is "up"; flip if it reads upside
// down on your bench.
#define DISPLAY_ROTATION 1

// 0-255.
#define DISPLAY_BRIGHTNESS 200

// ---------------------------------------------------------------------------
// Encoder I2C bus -- THE PIN DECISION, RESOLVED (plan sections 0.2 and 1.4)
// ---------------------------------------------------------------------------
// SETTLED on hardware: this unit's STEMMA QT / Qwiic connector is on GPIO
// 43/44, and a scan of 17/18 found nothing.
//
// Plan section 0.2 recommends hand-wiring I2C to 17/18 in this case, to keep
// 43/44 (the default UART0 pins) free for the log UART. That trade-off does
// not actually exist: ESP32 UARTs route through the GPIO matrix, and log.cpp
// drives Serial1 with explicit pins, so the log console can sit on any free
// GPIO. Taking the connector and moving the log UART to 17/18 costs nothing
// and keeps the bus plug-and-play.
//
// The one real consequence: the ROM bootloader prints on U0TXD (GPIO43) at
// reset, which is now SDA. With SCL idle high, that chatter looks like a burst
// of spurious START/STOP conditions and no clock edges, so no device latches
// anything -- harmless, and over before setup() runs. It does mean **Core
// Debug Level must stay "None"**: routing ESP_LOG to UART0 would drive SDA
// during normal operation and corrupt the bus.

#define PIN_I2C_SDA 43
#define PIN_I2C_SCL 44
// Drop to 100000 if the bus is flaky on a long Qwiic cable.
#define I2C_FREQ_HZ 400000

// Generous, per design doc section 4: a wedged Seesaw must not hang the UI
// task. Milliseconds.
#define I2C_TIMEOUT_MS 50

// Adafruit Quad Rotary Encoder breakout (product 5752 / Seesaw). Confirmed at
// 0x49 on this project's hardware by the Pico firmware.
#define SEESAW_ADDR 0x49

// Host-side interrupt input from the Seesaw INT pin. Active low, open drain --
// the breakout does not pull it up, so we use the ESP32 internal pull-up.
#define PIN_SEESAW_INT 16

// ---------------------------------------------------------------------------
// Log sink (plan section 1.1)
// ---------------------------------------------------------------------------
// Both sinks can be enabled at once, which is how phase 1 verified the UART
// sink produces identical output.
//
// **USB logging is available in phase 2 after all**, on the recommended board
// settings below (USB Mode "Hardware CDC and JTAG" + CDC On Boot "Enabled").
// In that combination `Serial` resolves to HWCDCSerial -- the USB-Serial/JTAG
// peripheral -- and never touches GPIO 43/44. The dangerous mapping is
// specifically CDC On Boot = "Disabled", which makes `Serial` mean Serial0 =
// UART0 = the I2C bus. log.cpp carries an #error binding this flag to
// ARDUINO_USB_CDC_ON_BOOT so the two cannot disagree; do not defeat it.
//
// **What the USB console does and does not cover.** It works from boot until
// usb_host_install() takes the OTG PHY, then goes silent for the rest of the
// session -- so it carries the I2C survey, the redraw profile and the boot
// banner, but NOT the enumeration descriptor dump, which happens after the
// handover. The UART sink (or the display, later) is what covers that.
//
// Writes are made non-blocking in log_init(); see the note there.

#define LOG_SINK_USB 1
#define LOG_SINK_UART 1

// Moved off 43/44, which the Qwiic connector claims for I2C. Serial1 goes
// through the GPIO matrix, so these are free choices -- any unused header pin
// works. Re-verify the sink physically on the new TX pin.
#define PIN_LOG_UART_TX 17
#define PIN_LOG_UART_RX 18
#define LOG_UART_BAUD 115200

// How long log_init() waits for a USB CDC console to be opened before giving up
// and continuing. It never blocks boot beyond this -- the board must come up
// standalone. Costs this much on every boot with no USB host attached, so drop
// it (or to 0) once you are not reading the boot log over USB.
#define LOG_USB_WAIT_MS 1500

// ---------------------------------------------------------------------------
// Bring-up diagnostics
// ---------------------------------------------------------------------------

// Scan the I2C bus at boot and log every address found (plan section 1.4).
#define I2C_SCAN_ON_BOOT 1

// Also scan an alternate pin pair. Skipped automatically at runtime if it
// would collide with the log UART pins.
//
// Answered in phase 1: SDA 43, SCL 44, one device at 0x49. Off since.
#define I2C_SCAN_ALT_PINS 0
#define PIN_I2C_SDA_ALT 44
#define PIN_I2C_SCL_ALT 43

// Run the redraw profile sweep at boot and log the numbers (plan section 1.3).
#define DISPLAY_PROFILE_ON_BOOT 1

// The phase 1 synthetic load (SYNTHETIC_LOAD_*) is gone rather than switched
// off. It stood in for phase 2's MIDI drain, and loop() now runs the real
// thing, so keeping it would have meant a dead flag that competed with what it
// was simulating. Its ui_task::reportLoad() hook survives and carries the real
// drain's lateness to the footer -- same instrumentation, no extra code
// (plan section 2.0).

// Log a one-line stats heartbeat this often. 0 disables.
#define STATS_INTERVAL_MS 5000

// ---------------------------------------------------------------------------
// UI task (plan section 1.8)
// ---------------------------------------------------------------------------
// Pinned to core 1 alongside the Arduino loopTask, at a higher priority --
// which is precisely the relationship section 1.8 asks us to validate.
// loopTask runs at priority 1.

#define UI_TASK_CORE 1
#define UI_TASK_PRIORITY 3
// ESP-IDF's FreeRTOS takes this in BYTES, not words. The task runs LovyanGFX
// text rendering and vsnprintf, so do not trim this without checking
// uxTaskGetStackHighWaterMark.
#define UI_TASK_STACK_BYTES 8192

// The UI task blocks on a task notification with this timeout, so it wakes
// either on a Seesaw interrupt or on this tick -- whichever comes first. The
// tick is what polls the push-buttons (see SEESAW_GPIO_INTERRUPTS).
#define UI_TICK_MS 20

// Use the level-triggered Seesaw interrupt (plan section 1.6).
//
// Set to 0 for the section 1.5 bring-up step: encoders are then read purely on
// the UI_TICK_MS tick, which isolates Seesaw driver problems from ISR
// problems. The UI behaves identically, just with up to one tick of latency.
#define ENCODER_USE_INTERRUPT 1

// Enable Seesaw GPIO interrupts for the push-buttons, putting them on the
// shared INT line.
//
// Default OFF, matching the shipped Pico firmware
// (spirant-encoder-board-rs/src/registers.rs), which deliberately polls the
// buttons "so button presses stay off the shared INT line and the rotation
// interrupt path is unaffected". The plan's section 1.6 suggests enabling
// them; this flag lets you try that without editing any module. Buttons are
// polled on the UI_TICK_MS tick either way, so the UI is identical.
#define SEESAW_GPIO_INTERRUPTS 0

// Push-button debounce: a press must be a fresh low reading on a tick, and
// this many milliseconds must have passed since the last accepted press.
#define BUTTON_DEBOUNCE_MS 40

// ---------------------------------------------------------------------------
// Page navigation and screen selection
// ---------------------------------------------------------------------------
// Encoder 0's switch pages backward, encoder 3's pages forward, both wrapping
// -- same as spirant-hw-interface-rs. Both act on the parameter screen only.
//
// Switch 2 toggles between the parameter screen and the MIDI monitor (plan
// section 2.4). The monitor is deliberately NOT a seventh page: six is already
// a long rotation and the monitor is not a parameter page. Switch 1 stays
// unassigned, for phase 3 or 4.
//
// A screen switch takes the full-repaint path, ~11.7 ms, so it should feel
// deliberate rather than incidental.

#define PAGE_BACK_SWITCH 0
#define PAGE_FWD_SWITCH 3
#define SCREEN_TOGGLE_SWITCH 2

// ---------------------------------------------------------------------------
// Phase 2 -- USB MIDI host (plan section 2)
// ---------------------------------------------------------------------------
// RECOMMENDED BOARD SETTINGS -- matching ESP32_Host_MIDI's own USB-Host
// example, which is the configuration the library is tested against:
//
//   Tools > USB Mode                   -> "Hardware CDC and JTAG"   (the menu default)
//   Tools > USB CDC On Boot            -> "Enabled"                 (the menu default)
//   Tools > USB Firmware MSC On Boot   -> "Disabled"                (the menu default)
//   Tools > USB DFU On Boot            -> "Disabled"                (the menu default)
//   Tools > Core Debug Level           -> "None"                    (load-bearing, see below)
//
// **USB Mode does not decide whether the board can host.** The ESP32-S3 has two
// USB controllers sharing one PHY and one pin pair: the fixed-function
// USB-Serial/JTAG, and the host-capable USB-OTG. ARDUINO_USB_MODE only selects
// which one Arduino uses for its *device-side* `Serial`. Either way,
// usb_host_install() claims the OTG controller and switches the PHY to it.
//
// "Hardware CDC and JTAG" is the better choice for two reasons:
//
//  1. Every TinyUSB device start-up in the core is gated on !ARDUINO_USB_MODE
//     (cores/esp32/main.cpp), so in this mode USB.begin() is *never called* and
//     CDC/MSC/DFU On Boot cannot claim the OTG controller at all. In OTG mode
//     they can, which is what the guards below are for.
//  2. `Serial` resolves to HWCDCSerial rather than to UART0, so the log sink is
//     usable without endangering the I2C bus -- and you get a console over the
//     USB-C port from boot until the PHY handover.
//
// **Core Debug Level must still be "None".** That constraint is unrelated to
// Arduino's `Serial`: the prebuilt IDF sets CONFIG_ESP_CONSOLE_UART_NUM=0, so
// ESP_LOG output goes to UART0 = GPIO 43/44 = SDA/SCL no matter which USB mode
// is selected. This half of the hazard does not go away.
//
// Once host mode engages the USB-C port stops being a console, so flashing
// needs manual bootloader entry: hold BOOT, tap RST, release BOOT.

#define USB_MIDI_ENABLED 1

// ---------------------------------------------------------------------------
// Board-setting guards (plan section 2.0)
// ---------------------------------------------------------------------------
// The core gives no runtime hint when the board settings make hosting
// impossible: the firmware builds, runs, and simply never enumerates anything.
// On a bench with no serial console that is close to undiagnosable, so it is a
// compile error instead.
//
// **These are conditional on USB Mode, not absolute.** An earlier version of
// this block required ARDUINO_USB_MODE == 0 and rejected "Hardware CDC and
// JTAG" outright. That was wrong, and it blocked the very configuration the
// library's own example recommends. USB Mode does not gate host capability
// (see the note above); what matters is only whether something started the
// TinyUSB *device* stack on the OTG controller first, and that can only happen
// in OTG mode:
//
//   ARDUINO_USB_MODE == 1  ("Hardware CDC and JTAG")
//       main.cpp's USB.begin() is compiled out entirely. CDC/MSC/DFU On Boot
//       are inert. Nothing to check.
//
//   ARDUINO_USB_MODE == 0  ("USB-OTG (TinyUSB)")
//       Any of CDC/MSC/DFU On Boot reaches USB.begin() before setup() and takes
//       the controller. All three must be Disabled.
//
// **Why they live in config.h** rather than beside the USB code: this header is
// included by every firmware translation unit, so the error fires on the first
// one compiled. A guard placed after a library #include is a guard that a
// failing library header can mask -- which has already happened once here, when
// a core with no USB-host component turned a board-setting mistake into a
// confusing "usb/usb_host.h: No such file or directory".
//
// Each test is written `defined(X) && X` so that a core which stops defining
// one of these fails open rather than silently skipping the check.

#if USB_MIDI_ENABLED && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE == 0

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#error "With Tools > USB Mode = 'USB-OTG (TinyUSB)', USB CDC On Boot must be 'Disabled' -- Enabled calls USB.begin() before setup() and claims the OTG controller as a device, so usb_host_install() has nothing left to take. Either disable it, or switch USB Mode to 'Hardware CDC and JTAG' (recommended), where it is harmless and Serial stays off the I2C pins."
#endif

#if defined(ARDUINO_USB_MSC_ON_BOOT) && ARDUINO_USB_MSC_ON_BOOT
#error "With Tools > USB Mode = 'USB-OTG (TinyUSB)', USB Firmware MSC On Boot must be 'Disabled' -- it starts the TinyUSB device stack as mass storage before setup() and claims the OTG controller."
#endif

#if defined(ARDUINO_USB_DFU_ON_BOOT) && ARDUINO_USB_DFU_ON_BOOT
#error "With Tools > USB Mode = 'USB-OTG (TinyUSB)', USB DFU On Boot must be 'Disabled' -- it starts the TinyUSB device stack as DFU before setup() and claims the OTG controller."
#endif

#endif  // USB_MIDI_ENABLED && OTG mode

// Dump the device and configuration descriptors at enumeration: VID/PID,
// interfaces, endpoints, bInterval, and the MS header's bcdMSC (which is what
// actually answers "MIDI 1.0 or MIDI 2.0"). Plan section 2.2 asks for all of
// it. Logged from the library's core-0 USB task and blocking at 115200, so it
// costs a couple of hundred milliseconds -- once, at enumeration, with nothing
// streaming.
#define USB_MIDI_LOG_ENUMERATION 1

// Log every parsed message. A firehose at breath rates -- 115200 cannot carry
// 2000 msg/s and the drain would block on the UART. For short bring-up
// sessions and single-message checks only; the monitor page is the instrument
// for sustained traffic.
#define USB_MIDI_LOG_MESSAGES 0

// Capacity of USBConnection's ring buffer, mirrored from the library
// (USBConnection.h, `static const int QUEUE_SIZE = 64`). It is protected and
// not configurable, so this is a copy, not a setting -- changing it here
// changes only the arithmetic below and what the display reports.
//
// The plan's section 2.3 margin was computed against an assumed 2 ms CC2 rate:
//
//   64 packets / 2 ms      = 128 ms of buffer   vs 11.7 ms worst UI stall  ~11x
//   64 packets / 500 us    =  32 ms of buffer   vs 11.7 ms worst UI stall  ~2.7x
//
// The 500 us figure is the one section 2.5 warns about, and 2.7x is thin. The
// firmware therefore reports the ring's high-water mark and counts how close it
// came to full; section 2.5 re-checks both against the measured EWI rate.
#define USB_RING_CAPACITY 64

// Drain period. The drain blocks on vTaskDelayUntil rather than spinning: a
// loop() that polls without yielding starves core 1's idle task and trips the
// task watchdog, which presents as a random reboot rather than as a scheduling
// problem (plan section 2.3).
#define MIDI_DRAIN_PERIOD_MS 1

// How long either side will wait for the monitor snapshot mutex. The reader
// (UI task) renders stale data rather than blocking; the writer (drain) drops
// an update rather than backing up. See midi/monitor_access.h.
#define MONITOR_LOCK_TIMEOUT_MS 5
