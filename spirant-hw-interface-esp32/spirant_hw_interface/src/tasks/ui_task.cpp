#include "ui_task.h"

#include <Arduino.h>
#include <Wire.h>

#include "../config.h"
#include "../diag/i2c_scan.h"
#include "../display/display_manager.h"
#include "../display/monitor_screen.h"
#include "../display/param_screen.h"
#include "../encoders/encoder_input.h"
#include "../log.h"
#include "../midi/usb_midi_host.h"
#include "../params/parameter_values.h"

namespace ui_task
{
namespace
{

DisplayManager  g_display;
EncoderInput    g_encoders;
ParameterValues g_params;

ParamScreen   g_param_screen{g_params};
MonitorScreen g_monitor_screen;

TaskHandle_t g_task = nullptr;

bool g_display_ok  = false;
bool g_encoders_ok = false;

uint32_t g_wakes                    = 0;
uint32_t g_worst_service_latency_us = 0;

// Written by the drain in loop(), read by the UI task. Diagnostics only.
volatile uint32_t g_load_worst_late_us = 0;
volatile uint32_t g_load_iterations    = 0;

bool showingMonitor()
{
    return g_display.screen() == &g_monitor_screen;
}

void applyEvents(const EncoderInput::Events& ev)
{
    // Screen toggle first: while the monitor is up, the encoders and the page
    // switches belong to the parameter model but nothing they do is visible, so
    // acting on the toggle before the rest keeps a simultaneous press and turn
    // from paging a screen the player cannot see.
    if (ev.pressed[SCREEN_TOGGLE_SWITCH])
    {
        Screen* next = showingMonitor() ? static_cast<Screen*>(&g_param_screen)
                                        : static_cast<Screen*>(&g_monitor_screen);
        g_display.setScreen(next);
        slog_printf("screen -> %s\n", next->title());
        return;
    }

    for (uint8_t i = 0; i < kParamsPerPage; ++i)
    {
        if (ev.deltas[i] != 0) g_params.updateFromEncoder(i, ev.deltas[i]);
    }

    // Page navigation applies to the parameter screen only -- paging a hidden
    // screen would surprise the player on the way back. Switches 0 and 3 match
    // spirant-hw-interface-rs: encoder 0 backward, encoder 3 forward.
    if (showingMonitor()) return;

    if (ev.pressed[PAGE_BACK_SWITCH])
    {
        g_params.pageBackward();
        slog_printf("page -> %u (%s)\n", g_params.currentPage(), kPageNames[g_params.currentPage()]);
    }
    else if (ev.pressed[PAGE_FWD_SWITCH])
    {
        g_params.pageForward();
        slog_printf("page -> %u (%s)\n", g_params.currentPage(), kPageNames[g_params.currentPage()]);
    }
}

void logChanges()
{
    // Phase 2 still has no MIDI consumer for parameters, so drain the flags
    // here. This is exactly where the CC send will go in phase 3 -- keeping the
    // drain in place now means the flags never silently pile up, and the log
    // doubles as a preview of what will go on the wire.
    ParameterChange changes[kTotalSlots];
    const size_t    n = g_params.takeMidiChanges(changes, kTotalSlots);

    for (size_t i = 0; i < n; ++i)
    {
        slog_printf("param %2u %-16s = %ld\n",
                    changes[i].global_idx,
                    changes[i].name,
                    static_cast<long>(changes[i].value));
    }
}

void monitorStatsTick()
{
    const MonitorSnapshot& m = g_monitor_screen.snapshot();

    // cc2/s is the authoritative breath rate and the figure plan section 2.5
    // reports back into the bandwidth budget. minint is a bound on burst
    // spacing, not a rate -- a whole drain batch shares one timestamp, so it
    // cannot see inside a burst. Both are printed because they answer different
    // questions, and 0 for minint means "never resolved", not "instant".
    slog_printf("midi: msg=%lu %lu/s cc2=%lu %lu/s minint=%luus unparsed=%lu "
                "ring=%u/%u hwm=%u nearfull=%lu batch=%u stage_ovf=%lu ch=%04X\n",
                static_cast<unsigned long>(m.messages),
                static_cast<unsigned long>(m.messages_per_sec),
                static_cast<unsigned long>(m.breath_updates),
                static_cast<unsigned long>(m.breath_per_sec),
                static_cast<unsigned long>(m.breath_min_interval_us == 0xFFFFFFFFUL
                                               ? 0UL
                                               : m.breath_min_interval_us),
                static_cast<unsigned long>(m.messages_unparsed),
                usb_midi::queueDepth(),
                m.queue_capacity,
                m.queue_hwm,
                static_cast<unsigned long>(m.queue_near_full),
                m.drain_max_batch,
                static_cast<unsigned long>(usb_midi::stagingOverflows()),
                m.channels_seen);

    if (!g_display_ok) return;

    // The footer carries the numbers that decide whether phase 2 passed:
    // throughput, how close the ring came to overrunning, and drain lateness.
    g_display.drawStatus("%lu/s  ring %u/%u  near %lu  late %luus",
                         static_cast<unsigned long>(m.messages_per_sec),
                         m.queue_hwm,
                         m.queue_capacity,
                         static_cast<unsigned long>(m.queue_near_full),
                         static_cast<unsigned long>(g_load_worst_late_us));
}

void paramStatsTick(const Stats& s)
{
    slog_printf(
        "stats: wakes=%lu svc=%lu isr=%lu detents=%lu glitch=%lu i2cerr=%lu "
        "lat=%luus val=%luus page=%luus drain_late=%luus/%lu\n",
        static_cast<unsigned long>(s.wakes),
        static_cast<unsigned long>(s.services),
        static_cast<unsigned long>(s.isr_count),
        static_cast<unsigned long>(s.detents),
        static_cast<unsigned long>(s.glitches),
        static_cast<unsigned long>(s.i2c_errors),
        static_cast<unsigned long>(s.worst_service_latency_us),
        static_cast<unsigned long>(s.worst_value_us),
        static_cast<unsigned long>(s.worst_page_us),
        static_cast<unsigned long>(g_load_worst_late_us),
        static_cast<unsigned long>(g_load_iterations));

    if (!g_display_ok) return;

    g_display.drawStatus("lat %lu  val %lu  pg %lu  late %lu  err %lu",
                         static_cast<unsigned long>(s.worst_service_latency_us),
                         static_cast<unsigned long>(s.worst_value_us),
                         static_cast<unsigned long>(s.worst_page_us),
                         static_cast<unsigned long>(g_load_worst_late_us),
                         static_cast<unsigned long>(s.i2c_errors));
}

void statsTick()
{
#if STATS_INTERVAL_MS > 0
    static uint32_t last_ms = 0;

    const uint32_t now = millis();
    if (now - last_ms < STATS_INTERVAL_MS) return;
    last_ms = now;

    g_encoders.probe();

    Stats s;
    snapshot(s);

    if (!s.encoders_ok)
    {
        if (g_display_ok)
        {
            g_display.drawStatus("NO SEESAW at 0x%02X -- check I2C wiring", SEESAW_ADDR);
        }
        return;
    }

    // Each screen's footer answers the question that screen is being watched
    // for. Both lines still go to the log regardless of which is on the panel.
    if (showingMonitor())
    {
        monitorStatsTick();
    }
    else
    {
        paramStatsTick(s);
    }
#endif
}

void taskMain(void* arg)
{
    (void)arg;

    // First pass paints the whole screen: setScreen() marked it dirty.
    if (g_display_ok) g_display.update();

    for (;;)
    {
        const uint32_t notified =
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_TICK_MS));

        ++g_wakes;

        if (notified != 0)
        {
            const uint32_t latency = micros() - g_encoders.lastIsrUs();
            // Guard against the timer wrap producing a nonsense figure.
            if (latency < 1000000UL && latency > g_worst_service_latency_us)
            {
                g_worst_service_latency_us = latency;
            }
        }

        EncoderInput::Events ev;
        g_encoders.service(ev);
        g_encoders.rearm();

        if (ev.any)
        {
            applyEvents(ev);
            logChanges();
        }

        if (g_display_ok) g_display.update();

        statsTick();
    }
}

}  // namespace

bool start()
{
    g_display_ok = g_display.begin();

    if (g_display_ok)
    {
        g_display.showSplash("spirant", "hw interface / esp32");
        delay(800);
    }

    // Bus survey before anything talks to the Seesaw, so a wrong pin pair
    // shows up as "nothing responded" rather than as a mystery driver failure.
#if I2C_SCAN_ON_BOOT
    i2c_survey();
#else
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);
#endif

    if (g_display_ok && DISPLAY_PROFILE_ON_BOOT) g_param_screen.profile(g_display);

    // Boot on the parameter screen; switch 2 reaches the monitor.
    g_display.setScreen(&g_param_screen);

    // The task handle has to exist before begin() installs the ISR, so create
    // the task suspended-in-effect by having taskMain block immediately -- it
    // does, on ulTaskNotifyTake. Encoders come up after creation.
    const BaseType_t created = xTaskCreatePinnedToCore(taskMain,
                                                       "ui",
                                                       UI_TASK_STACK_BYTES,
                                                       nullptr,
                                                       UI_TASK_PRIORITY,
                                                       &g_task,
                                                       UI_TASK_CORE);

    if (created != pdPASS || g_task == nullptr)
    {
        slog_printf("ui: FAILED to create task\n");
        return false;
    }

    // After task creation: begin() needs the handle to install the ISR
    // against. A failure here is reported on screen by statsTick(), not from
    // this thread -- the panel has exactly one writer (plan section 1.8).
    g_encoders_ok = g_encoders.begin(g_task);

    slog_printf("ui: task on core %d prio %d (loopTask is prio 1 on core %d)\n",
                UI_TASK_CORE,
                UI_TASK_PRIORITY,
                xPortGetCoreID());
    slog_printf("ui: switch %d toggles param page <-> MIDI monitor\n", SCREEN_TOGGLE_SWITCH);

    return true;
}

void snapshot(Stats& out)
{
    out.wakes                    = g_wakes;
    out.services                 = g_encoders.serviceCount();
    out.isr_count                = g_encoders.isrCount();
    out.detents                  = g_encoders.detentCount();
    out.glitches                 = g_encoders.glitchCount();
    out.i2c_errors               = g_encoders.i2cErrorCount();
    out.worst_service_latency_us = g_worst_service_latency_us;
    out.worst_value_us           = g_display.worstValueUs();
    out.worst_page_us            = g_display.worstPageUs();
    out.page                     = g_params.currentPage();
    out.display_ok               = g_display_ok;
    out.encoders_ok              = g_encoders_ok;
    out.showing_monitor          = showingMonitor();
}

void reportLoad(uint32_t worst_late_us, uint32_t iterations)
{
    g_load_worst_late_us = worst_late_us;
    g_load_iterations    = iterations;
}

}  // namespace ui_task
