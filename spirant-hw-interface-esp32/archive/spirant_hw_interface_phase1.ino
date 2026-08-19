// Spirant hardware interface -- ESP32-S3 firmware, phase 1.
//
// LilyGO T-Display-S3. Display and encoders only: no MIDI, no USB host, no
// UART MIDI. See plans/esp32_implementation_plan.md section 1.
//
// This file is wiring only. All logic lives under src/, which the Arduino
// build compiles recursively.
//
// Board settings (Tools menu) are listed in the README -- USB Mode and USB CDC
// On Boot in particular decide whether phase 2 can host at all.

#include "src/config.h"
#include "src/log.h"
#include "src/tasks/ui_task.h"

void setup()
{
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

    slog_printf("boot: complete\n");
}

// loop() runs on the Arduino loopTask: core 1, priority 1. The UI task sits
// above it on the same core, which is the arrangement plan section 1.8 asks us
// to validate. The synthetic load below stands in for phase 2's MIDI drain --
// a periodic burst of work that must not be starved by display redraws, and
// must not delay encoder servicing either.
void loop()
{
#if SYNTHETIC_LOAD_ENABLED
    static TickType_t last_wake   = xTaskGetTickCount();
    static uint32_t   expected_us = 0;
    static uint32_t   worst_late  = 0;
    static uint32_t   iterations  = 0;

    if (expected_us == 0) expected_us = micros();

    // A real drain loop blocks between bursts; so does this one. Spinning
    // instead would starve core 1's idle task and trip the watchdog, which
    // would tell us nothing about priorities.
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SYNTHETIC_LOAD_PERIOD_US / 1000));
    expected_us += SYNTHETIC_LOAD_PERIOD_US;

    const uint32_t now  = micros();
    const int32_t  late = static_cast<int32_t>(now - expected_us);

    if (late > static_cast<int32_t>(10 * SYNTHETIC_LOAD_PERIOD_US) || late < 0)
    {
        // Far out of step (or the timer wrapped) -- resync rather than
        // reporting a meaningless number forever after.
        expected_us = now;
    }
    else if (static_cast<uint32_t>(late) > worst_late)
    {
        worst_late = static_cast<uint32_t>(late);
    }

    // The burst itself.
    const uint32_t burn_until = micros() + SYNTHETIC_LOAD_BURN_US;
    volatile uint32_t sink = 0;
    while (static_cast<int32_t>(micros() - burn_until) < 0)
    {
        sink += 1;
    }
    (void)sink;

    if ((++iterations % 256) == 0) ui_task::reportLoad(worst_late, iterations);
#else
    delay(50);
#endif
}
