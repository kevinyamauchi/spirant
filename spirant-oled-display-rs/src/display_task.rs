//! Display update task and configuration.
//!
//! Contains the [`DisplayConfig`] struct (the single source of layout
//! geometry) and the [`display_update_task`] async function that
//! periodically reads [`ParameterValues`] and flushes changed frames to
//! the OLED hardware.
//!
//! [`ParameterValues`]: spirant::parameter_values::ParameterValues

use embedded_hal_async::i2c::I2c;

use spirant::parameter_values::{
    ParameterSlot, ParameterValues, PARAMS_PER_PAGE, PARAM_NAMES, PAGE_NAMES,
};

use crate::driver::OledDriver;
use crate::layout::{render_display, DisplayConfig, DisplayState};

// ── Display update task ──────────────────────────────────────────────────

/// Periodic display update loop.
///
/// This is a regular `async fn` — **not** an Embassy `#[task]`. Callers
/// should create a thin, concrete task wrapper that calls this function,
/// since Embassy tasks cannot be generic:
///
/// ```ignore
/// #[embassy_executor::task]
/// async fn oled_task(
///     driver: OledDriver<MyConcreteI2cType>,
///     params: &'static Mutex<CriticalSectionRawMutex, ParameterValues>,
///     config: DisplayConfig,
/// ) {
///     display_update_task(driver, params, config).await;
/// }
/// ```
///
/// # Control flow
///
/// 1. Initialise the display hardware.
/// 2. Loop at `config.update_frequency_hz`:
///    - **Step 1** — Lock `param_values`, read current page/param data and
///      snapshot the `changed_oled` flags. Release the mutex.
///    - **Step 2** — Build a [`DisplayState`] from the snapshot.
///    - **Step 3** — Skip if state matches the previous frame.
///    - **Step 4** — Clear buffer and render (no I2C, no mutex).
///    - **Step 5** — Flush frame buffer to hardware (~20 ms I2C).
///    - **Step 6** — Lock `param_values`, selectively clear
///      `changed_oled` for parameters that had the flag set in Step 1.
///      Parameters that changed *during* the flush keep their flag.
///
/// # Errors
///
/// * Initialisation failure: logs the error and **returns** (task exits).
/// * Render / flush failure: logs the error and continues to the next cycle.
#[allow(clippy::needless_pass_by_value)] // config is small and consumed
pub async fn display_update_task<I2C>(
    mut driver: OledDriver<I2C>,
    param_values: &'static embassy_sync::mutex::Mutex<
        embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex,
        ParameterValues,
    >,
    config: DisplayConfig,
) where
    I2C: I2c,
{
    // ── Initialisation ───────────────────────────────────────────────
    if let Err(_e) = driver.init().await {
        #[cfg(feature = "defmt")]
        defmt::error!("OLED init failed: {}", _e);
        return;
    }

    #[cfg(feature = "defmt")]
    defmt::info!("OLED initialised");

    let period = embassy_time::Duration::from_millis(config.update_period_ms());
    let mut last_state = DisplayState::default();

    // ── Main loop ────────────────────────────────────────────────────
    loop {
        embassy_time::Timer::after(period).await;

        // ── Step 1: read state (mutex held briefly) ──────────────────
        let (page_name, param_names, param_values_snap, changed_flags) = {
            let params = param_values.lock().await;
            let page_idx = params.current_page();
            let page_name: &str = PAGE_NAMES[page_idx];

            let mut names: [Option<&str>; 4] = [None; 4];
            let mut values: [Option<i32>; 4] = [None; 4];
            let mut flags: [bool; 4] = [false; 4];

            let page = &params.pages[page_idx];
            for i in 0..PARAMS_PER_PAGE {
                match &page.params[i] {
                    ParameterSlot::Active(param) => {
                        names[i] = PARAM_NAMES[page_idx][i];
                        values[i] = Some(param.value);
                        flags[i] = param.changed_oled;
                    }
                    ParameterSlot::Null => {
                        // Leave as None / false — blank column.
                    }
                }
            }

            (page_name, names, values, flags)
        }; // ← mutex released here, before any I2C work

        // ── Step 2: build new display state ──────────────────────────
        let new_state = DisplayState::from_params(page_name, param_names, param_values_snap);

        // ── Step 3: skip if nothing changed ──────────────────────────
        if new_state == last_state {
            continue;
        }

        // ── Step 4: render to frame buffer (no I2C, no mutex) ────────
        driver.clear_buffer();
        if let Some(display) = driver.display_mut() {
            if let Err(_e) = render_display(display, &new_state, &config) {
                #[cfg(feature = "defmt")]
                defmt::error!("Render failed");
                continue;
            }
        } else {
            // Should not happen since init() succeeded, but guard anyway.
            continue;
        }

        // ── Step 5: flush to hardware (~20 ms I2C, no mutex held) ────
        if let Err(_e) = driver.flush().await {
            #[cfg(feature = "defmt")]
            defmt::error!("Flush failed: {}", _e);
            continue;
        }

        // ── Step 6: selectively clear changed_oled flags ─────────────
        //
        // Only clear flags for parameters that had changed_oled set when
        // we read them in Step 1. Parameters that changed *during* the
        // flush (Steps 4–5) keep their flag and are picked up next cycle.
        {
            let mut params = param_values.lock().await;
            let page_idx = params.current_page();
            let page = &mut params.pages[page_idx];
            for (i, &was_changed) in changed_flags.iter().enumerate() {
                if was_changed {
                    if let ParameterSlot::Active(ref mut param) = page.params[i] {
                        param.changed_oled = false;
                    }
                }
            }
        } // ← mutex released

        last_state = new_state;
    }
}

// Tests for DisplayConfig are in layout.rs where the type is defined.
