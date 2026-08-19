// Spirant hardware interface -- ESP32-S3 firmware, phase 2.
//
// LilyGO T-Display-S3. Display, encoders, and USB MIDI host input rendered on a
// monitor page. No MIDI output: nothing is sent to the Daisy and no TRS path
// exists yet. See plans/esp32_implementation_plan.md section 2.
//
// This file is wiring only. All logic lives under src/, which the Arduino build
// compiles recursively.
//
// BOARD SETTINGS (Tools menu) -- phase 2 needs three, and two of them changed:
//
//   USB Mode         -> "USB-OTG (TinyUSB)"     CHANGED from "Hardware CDC and JTAG"
//   USB CDC On Boot  -> "Disabled"              CHANGED from "Enabled"
//   Core Debug Level -> "None"                  unchanged, and load-bearing
//
// The USB-C port is no longer a serial console in host mode; the log comes out
// of GPIO 17 at 115200. Flashing needs manual bootloader entry: hold BOOT, tap
// RST, release BOOT. src/config.h and src/log.cpp carry #error guards that stop
// the firmware compiling if the board settings and the flags disagree -- see
// the comment in log.cpp for what that combination does to the encoder bus.

#include "src/config.h"
#include "src/log.h"
#include "src/midi/usb_midi_host.h"
#include "src/tasks/ui_task.h"

namespace
{

bool g_usb_ok = false;

}  // namespace

void setup()
{
    // FIRST, before anything that can take time. On this board GPIO 15 latches
    // the 3V3 regulator on; when the board is powered from the battery
    // connector rather than from USB, the regulator drops out shortly after
    // boot unless firmware holds this high. DisplayManager::begin() also sets
    // it, but that runs after log_init(), which can spend up to
    // LOG_USB_WAIT_MS waiting for a USB console -- long enough to brown out on
    // battery. A power latch should not be a side effect of bringing up a
    // peripheral.
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);

    log_init();

    // The T-Display-S3 has no user LED, so the section 1.0 "blink + hello"
    // end-to-end check is the log line above plus the splash screen that
    // ui_task::start() paints.
    slog_printf("boot: chip %s rev %d, %lu MHz, free heap %u\n",
                ESP.getChipModel(),
                ESP.getChipRevision(),
                static_cast<unsigned long>(getCpuFrequencyMhz()),
                static_cast<unsigned>(ESP.getFreeHeap()));

    if (!ui_task::start())
    {
        slog_printf("boot: UI task failed to start -- halting\n");
        for (;;) delay(1000);
    }

    // USB host comes up after the UI, so a host stack that will not start still
    // leaves a working instrument with the failure on screen and in the log,
    // rather than a dark panel.
    g_usb_ok = usb_midi::begin();
    if (!g_usb_ok)
    {
        slog_printf("boot: USB MIDI host unavailable -- UI still running\n");
    }

    slog_printf("boot: complete, free heap %u\n", static_cast<unsigned>(ESP.getFreeHeap()));
}

// loop() runs on the Arduino loopTask: core 1, priority 1. The UI task sits
// above it on the same core, which is the arrangement plan section 1.8
// validated with a synthetic load and section 2.3 now subjects to the real
// thing.
//
// **It blocks rather than spins.** A loop() that polls without yielding starves
// core 1's idle task and trips the task watchdog, and that presents as a random
// reboot rather than as a scheduling problem -- more expensive to diagnose than
// to prevent (plan section 2.3). vTaskDelayUntil also gives us the lateness
// measurement for free, which is what reportLoad() carries to the display.
void loop()
{
    static TickType_t last_wake   = xTaskGetTickCount();
    static uint32_t   expected_us = 0;
    static uint32_t   worst_late  = 0;
    static uint32_t   iterations  = 0;

    if (expected_us == 0) expected_us = micros();

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MIDI_DRAIN_PERIOD_MS));
    expected_us += MIDI_DRAIN_PERIOD_MS * 1000UL;

    const uint32_t now  = micros();
    const int32_t  late = static_cast<int32_t>(now - expected_us);

    if (late > static_cast<int32_t>(10000 * MIDI_DRAIN_PERIOD_MS) || late < 0)
    {
        // Far out of step (or the timer wrapped) -- resync rather than
        // reporting a meaningless number forever after.
        expected_us = now;
    }
    else if (static_cast<uint32_t>(late) > worst_late)
    {
        worst_late = static_cast<uint32_t>(late);
    }

    if (g_usb_ok) usb_midi::drain(millis(), now);

    if ((++iterations % 256) == 0) ui_task::reportLoad(worst_late, iterations);
}
