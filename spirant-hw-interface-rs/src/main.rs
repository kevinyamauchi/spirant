//! spirant-hw-interface
//!
//! Encoder → ParameterValues → OLED display integration firmware for the
//! Raspberry Pi Pico 2. Wires the three existing library crates into a live
//! interactive loop:
//!
//! 1. A rotary encoder is turned.
//! 2. The encoder board fires an interrupt on the INT pin.
//! 3. The encoder monitor task reads the new position, calculates the delta,
//!    and calls `update_from_encoder()` on the shared `ParameterValues` mutex.
//! 4. The OLED display task wakes on its 30 Hz timer, detects the
//!    `changed_oled` flag, builds a new `DisplayState`, and flushes the
//!    updated frame to the screen.
//!
//! The encoder task additionally polls the four encoder push-buttons every
//! 20 ms (interleaved with the rotation interrupt via `select`): a press of
//! encoder 0 pages backward and encoder 3 pages forward, both wrapping.
//!
//! A third task serves the **System Bus**: the Pico is an I2C target at
//! address `0x20` on I2C1, and the Daisy Seed is the controller that pulls
//! parameter values from it. The MSG line (GP22) tells the Daisy when
//! something is worth pulling. See `plans/inter_mcu_communication_protocol.md`
//! and [`system_bus_task`].

#![no_std]
#![no_main]

use defmt::*;
use embassy_executor::Spawner;
use embassy_rp::block::ImageDef;
use embassy_rp::bind_interrupts;
use embassy_rp::gpio::{Input, Level, Output, Pull};
use embassy_rp::i2c::{self, I2c};
use embassy_rp::i2c_slave::{self, Command, I2cSlave, ReadStatus};
use embassy_rp::peripherals::{I2C0, I2C1};
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::mutex::Mutex;
use embassy_sync::signal::Signal;
use embassy_embedded_hal::shared_bus::asynch::i2c::I2cDevice;
use embassy_futures::select::{select, Either};
use embassy_time::{Duration, Timer};
use static_cell::StaticCell;
use {defmt_rtt as _, panic_probe as _};

use encoder_driver::{QuadEncoderBoard, DEFAULT_ADDRESS};
use spirant::parameter_values::{ParameterValues, N_PAGES};
use spirant::wire::{serialize_frame, FRAME_LEN};
use spirant_oled_display_rs::{display_update_task, DisplayConfig, OledDriver};

// ---------------------------------------------------------------------------
// Boot block and interrupt binding
// ---------------------------------------------------------------------------

/// Tell the RP2350 Boot ROM about our application.
#[link_section = ".start_block"]
#[used]
pub static IMAGE_DEF: ImageDef = embassy_rp::block::ImageDef::secure_exe();

// Wire the I2C peripheral interrupts to Embassy's async handlers.
//
// I2C0 is the Interface Bus (controller: encoder board + OLED).
// I2C1 is the System Bus (target: the Daisy Seed is the controller).
// `i2c_slave` shares `i2c`'s interrupt handler type.
bind_interrupts!(struct Irqs {
    I2C0_IRQ => i2c::InterruptHandler<I2C0>;
    I2C1_IRQ => i2c::InterruptHandler<I2C1>;
});

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------

/// Shared I2C0 bus — both the encoder board and the OLED display access it
/// through I2cDevice wrappers that serialise transactions.
static I2C_BUS: StaticCell<
    Mutex<CriticalSectionRawMutex, I2c<'static, I2C0, i2c::Async>>,
> = StaticCell::new();

/// Shared synthesizer parameter state — written by the encoder task,
/// read by the OLED display task and the System Bus task.
static PARAM_VALUES: StaticCell<
    Mutex<CriticalSectionRawMutex, ParameterValues>,
> = StaticCell::new();

/// Raised by the encoder task whenever it sets `changed_i2c` flags, so the
/// System Bus task can drive the MSG line LOW.
///
/// A `Signal` rather than a channel: MSG is a level, not a queue, so
/// coalescing several encoder detents into one wake-up is exactly right. The
/// encoder task never touches GP22 itself — the pin has a single owner
/// (`system_bus_task`), which is what keeps the MSG invariant checkable.
static MSG_SIGNAL: Signal<CriticalSectionRawMutex, ()> = Signal::new();

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

/// Concrete I2C type for the OLED display, sharing I2C_BUS.
type OledI2c = I2cDevice<
    'static,
    CriticalSectionRawMutex,
    I2c<'static, I2C0, i2c::Async>,
>;

/// Concrete I2C type for the encoder board, sharing I2C_BUS.
type EncoderI2c = I2cDevice<
    'static,
    CriticalSectionRawMutex,
    I2c<'static, I2C0, i2c::Async>,
>;

// ---------------------------------------------------------------------------
// System Bus protocol constants
// ---------------------------------------------------------------------------

/// Our I2C target address on the System Bus.
const SYSTEM_BUS_ADDR: u16 = 0x20;

/// Return every active parameter (protocol §5.1).
const CMD_GET_ALL: u8 = 0x00;
/// Return only parameters with `changed_i2c` set (protocol §5.1).
const CMD_GET_CHANGED: u8 = 0x01;
/// Write one parameter into the Pico. Specced but currently unused by the
/// Daisy; implemented here because the semantics already exist and a future
/// preset-recall path is the natural caller (protocol §5.3).
const CMD_SET_PARAMETER: u8 = 0x02;

/// Length of a `SetParameter` write: command + index + i32 big-endian.
const SET_PARAMETER_LEN: usize = 6;

/// Buffer the target listens into.
///
/// Must be **strictly larger** than the longest write we expect: embassy's
/// `listen()` reports `Error::PartialWrite` when the caller's buffer fills
/// exactly, so a 6-byte buffer would turn every well-formed `SetParameter`
/// into an error.
const LISTEN_BUF_LEN: usize = 16;

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

/// Thin wrapper that monomorphises the generic `display_update_task` so it can
/// be spawned as a concrete Embassy task.
#[embassy_executor::task]
async fn oled_task(
    driver: OledDriver<OledI2c>,
    params: &'static Mutex<CriticalSectionRawMutex, ParameterValues>,
    config: DisplayConfig,
) {
    display_update_task(driver, params, config).await;
}

/// Encoder monitoring task — rotation (interrupt-driven) plus button polling.
///
/// Each loop iteration waits on whichever comes first:
/// - the INT pin going LOW (a rotation): read all 4 positions, compute deltas
///   against the previous baseline, and apply non-zero deltas to the current
///   page's parameters; or
/// - a 20 ms timer tick: poll the four push-buttons and, on a fresh press of
///   encoder 0 / encoder 3, page backward / forward (wrapping).
///
/// The `ParameterValues` mutex is held only during in-memory updates — never
/// during I2C operations.
#[embassy_executor::task]
async fn encoder_task(
    mut int_pin: Input<'static>,
    mut encoder_board: QuadEncoderBoard<EncoderI2c>,
    param_values: &'static Mutex<CriticalSectionRawMutex, ParameterValues>,
) {
    info!("Encoder monitor task started");

    // Establish a baseline so the first delta calculation is correct.
    // If this read fails we start from [0; 4]; the first interrupt may
    // produce a spurious delta — harmless in practice.
    let mut previous_positions = [0i32; 4];
    match encoder_board.read_all_positions().await {
        Ok(positions) => previous_positions = positions,
        Err(_) => warn!("Could not read initial positions; starting from [0; 4]"),
    }

    // Button poll interval and previous state for rising-edge detection.
    const BUTTON_POLL: Duration = Duration::from_millis(20);
    let mut previous_buttons = [false; 4];

    loop {
        // wait_for_low() is used instead of wait_for_falling_edge() — confirmed
        // reliable with this encoder board during hardware testing. Race it
        // against the button-poll timer; whichever fires first is handled.
        match select(int_pin.wait_for_low(), Timer::after(BUTTON_POLL)).await {
            // ── Rotation ─────────────────────────────────────────────
            Either::First(_) => {
                let positions = match encoder_board.read_all_positions().await {
                    Ok(p) => p,
                    Err(_) => {
                        error!("Encoder read failed");
                        // Clear flags even on error so INT returns HIGH and the
                        // next movement produces a fresh interrupt rather than
                        // spinning the task in a tight error loop.
                        let _ = encoder_board.clear_interrupt_flags().await;
                        continue;
                    }
                };

                // Clear AFTER reading positions — drives INT back HIGH.
                // Clearing before reading would risk missing a rapid second
                // movement that arrives during the I2C read.
                if let Err(_) = encoder_board.clear_interrupt_flags().await {
                    warn!("Failed to clear interrupt flags");
                }

                let deltas: [i32; 4] =
                    core::array::from_fn(|i| positions[i] - previous_positions[i]);

                // Update baseline unconditionally — tracks hardware state even
                // when all deltas are zero (e.g. spurious power-on interrupt).
                previous_positions = positions;

                if deltas.iter().all(|&d| d == 0) {
                    continue;
                }

                // Mutex held only during in-memory updates — never during I2C.
                {
                    let mut params = param_values.lock().await;
                    for (encoder_idx, &delta) in deltas.iter().enumerate() {
                        if delta != 0 {
                            params.update_from_encoder(encoder_idx, delta);
                            debug!(
                                "Encoder {}: delta={}, position={}",
                                encoder_idx, delta, positions[encoder_idx]
                            );
                        }
                    }
                }

                // update_from_encoder is the only producer of changed_i2c, so
                // this is the one place MSG needs to be told. Signalled after
                // the lock is released so the System Bus task can act on it
                // immediately. Page switches do not signal: set_active_page
                // marks changed_oled only, and the Daisy has no concept of
                // pages.
                MSG_SIGNAL.signal(());
            }

            // ── Button poll ──────────────────────────────────────────
            Either::Second(_) => {
                let buttons = match encoder_board.read_buttons().await {
                    Ok(b) => b,
                    Err(_) => {
                        warn!("Button read failed");
                        continue;
                    }
                };

                // Rising edge = pressed now, released last poll.
                let prev_next = buttons[3] && !previous_buttons[3];
                let prev_back = buttons[0] && !previous_buttons[0];
                previous_buttons = buttons;

                if prev_next || prev_back {
                    let mut params = param_values.lock().await;
                    let current = params.current_page();
                    // Encoder 3 → forward, encoder 0 → backward (wrapping).
                    // Forward wins if both are pressed on the same poll.
                    let next = if prev_next {
                        (current + 1) % N_PAGES
                    } else {
                        (current + N_PAGES - 1) % N_PAGES
                    };
                    // set_active_page marks the new page's slots for redraw.
                    let _ = params.set_active_page(next);
                    debug!("Page switch: {} -> {}", current, next);
                }
            }
        }
    }
}

/// System Bus task — I2C target on I2C1 plus the MSG line.
///
/// The Daisy Seed is the controller; this task only ever answers. Two things
/// can wake it: a transaction from the Daisy, or [`MSG_SIGNAL`] telling it
/// that the encoder task has made something pending.
///
/// # The MSG invariant
///
/// MSG is LOW exactly when some parameter has `changed_i2c` set. It is driven
/// LOW when the encoder task signals, and re-evaluated from
/// `any_changed_i2c()` after a response frame has been fully clocked out.
/// A spurious signal (flags already cleared by a transaction that raced it)
/// costs one empty `GetChanged` and then self-corrects — the level is always
/// recomputed from the state, never toggled blind.
///
/// # Flag clearing
///
/// Flags are cleared only on `ReadStatus::Done`, which the RP2350 reports when
/// the controller NACKs our final byte — i.e. it read all 97 bytes and
/// stopped. Anything else (early stop, bus abort, timeout on the Daisy side)
/// leaves the flags and MSG alone, so the next poll retries. Clearing is
/// clear-if-unchanged (protocol §5.2), so a parameter that moved again during
/// the ~2.4 ms transmission stays pending rather than being lost.
///
/// # Locking
///
/// The parameter mutex is held only for in-memory snapshot and flag work,
/// never across an I2C phase — the Interface Bus tasks must never be blocked
/// waiting on the Daisy's clock.
///
/// # A note on cancelling `listen()`
///
/// Racing `listen()` against the signal means the listen future is dropped
/// whenever an encoder moves. If that happens to land inside a controller
/// write, the bytes drained so far are lost and the command is missed. The
/// exposure is small — the write phase is one byte out of a 33 ms poll
/// period — and the failure degrades along the path the protocol already
/// defines: the following read is served an 0xFF fill, the Daisy's CRC check
/// fails, and it escalates to `GetAll` (§4.4). Flags are untouched throughout,
/// so nothing is lost. `respond_to_read` is deliberately **not** cancellable
/// this way (embassy warns that cancelling it can stall the bus); it is only
/// ever awaited directly.
#[embassy_executor::task]
async fn system_bus_task(
    mut slave: I2cSlave<'static, I2C1>,
    mut msg: Output<'static>,
    param_values: &'static Mutex<CriticalSectionRawMutex, ParameterValues>,
) {
    info!("System Bus task started (I2C1 target 0x{:02X}, MSG LOW)", SYSTEM_BUS_ADDR);

    let mut listen_buf = [0u8; LISTEN_BUF_LEN];
    let mut frame = [0u8; FRAME_LEN];

    // Command byte from a preceding write, for controllers that issue the
    // write and the read as two separate transactions rather than using a
    // repeated start. Both shapes are legal (protocol §5.2).
    let mut pending_cmd: Option<u8> = None;

    loop {
        match select(slave.listen(&mut listen_buf), MSG_SIGNAL.wait()).await {
            // ── A change became pending ──────────────────────────────
            Either::Second(()) => {
                // Unconditionally LOW: the signal means a flag was set. If it
                // races a transaction that just cleared flags and raised MSG,
                // the worst case is one extra empty GetChanged, after which
                // the level is recomputed from state.
                msg.set_low();
                debug!("MSG -> LOW (encoder change pending)");
            }

            // ── A transaction from the Daisy ─────────────────────────
            Either::First(Ok(command)) => match command {
                // Repeated start: command byte and read in one transaction.
                Command::WriteRead(len) => {
                    let cmd = handle_write(&listen_buf[..len], param_values).await;
                    pending_cmd = None;
                    serve_read(&mut slave, &mut msg, param_values, cmd, &mut frame).await;
                }

                // Separate transactions: latch the command, answer on the
                // following read.
                Command::Write(len) => {
                    pending_cmd = handle_write(&listen_buf[..len], param_values).await;
                }

                Command::Read => {
                    // take() so a stray read cannot replay a stale command.
                    let cmd = pending_cmd.take();
                    serve_read(&mut slave, &mut msg, param_values, cmd, &mut frame).await;
                }

                Command::GeneralCall(len) => {
                    warn!("System Bus: unexpected general call, {} bytes", len);
                }
            },

            Either::First(Err(e)) => {
                warn!("System Bus listen error: {}", e);
            }
        }
    }
}

/// Decode a write phase. Returns the command byte if it is one a following
/// read should answer; `SetParameter` is applied here and returns `None`.
async fn handle_write(
    bytes: &[u8],
    param_values: &'static Mutex<CriticalSectionRawMutex, ParameterValues>,
) -> Option<u8> {
    let Some(&cmd) = bytes.first() else {
        warn!("System Bus: empty write");
        return None;
    };

    match cmd {
        CMD_GET_ALL | CMD_GET_CHANGED => Some(cmd),

        CMD_SET_PARAMETER => {
            if bytes.len() < SET_PARAMETER_LEN {
                warn!("System Bus: short SetParameter ({} bytes)", bytes.len());
                return None;
            }
            let idx = bytes[1];
            let value = i32::from_be_bytes([bytes[2], bytes[3], bytes[4], bytes[5]]);

            // update_from_i2c sets changed_oled only, never changed_i2c, so a
            // value the Daisy wrote is never echoed back to it. MSG is
            // deliberately untouched.
            let mut params = param_values.lock().await;
            match params.update_from_i2c(idx as usize, value) {
                Ok(()) => info!("System Bus: SetParameter idx={} value={}", idx, value),
                Err(_) => warn!("System Bus: SetParameter to invalid idx {}", idx),
            }
            None
        }

        other => {
            // Protocol §5.1: ignore the write. A following read is served an
            // 0xFF fill, which cannot pass the Daisy's CRC check.
            warn!("System Bus: unknown command 0x{:02X}", other);
            None
        }
    }
}

/// Serve the read phase of a transaction: serialize, respond, and — only on a
/// confirmed-complete transmission — clear flags and re-evaluate MSG.
async fn serve_read(
    slave: &mut I2cSlave<'static, I2C1>,
    msg: &mut Output<'static>,
    param_values: &'static Mutex<CriticalSectionRawMutex, ParameterValues>,
    cmd: Option<u8>,
    frame: &mut [u8; FRAME_LEN],
) {
    let only_changed = match cmd {
        Some(CMD_GET_ALL) => false,
        Some(CMD_GET_CHANGED) => true,
        _ => {
            // No command, or one with no response: stretch out an 0xFF fill
            // so the controller's read terminates instead of hanging the bus.
            warn!("System Bus: read with no pending query, serving 0xFF fill");
            if let Err(e) = slave.respond_till_stop(0xFF).await {
                warn!("System Bus: fill response failed: {}", e);
            }
            return;
        }
    };

    // Snapshot under the lock; serialize into the response buffer; release.
    // The lock is not held across the transmission.
    let (reported, count) = {
        let params = param_values.lock().await;
        serialize_frame(&params, only_changed, frame)
    };

    match slave.respond_to_read(frame).await {
        // The controller NACKed our last byte: all 97 bytes are on the wire.
        // This is the only outcome that may clear flags.
        Ok(ReadStatus::Done) => {
            let still_pending = {
                let mut params = param_values.lock().await;
                params.clear_i2c_flags_if_unchanged(&reported[..count]);
                params.any_changed_i2c()
            };

            msg.set_level(if still_pending { Level::Low } else { Level::High });

            info!(
                "System Bus: {} -> N={} done, MSG {}",
                if only_changed { "GetChanged" } else { "GetAll" },
                count,
                if still_pending { "LOW" } else { "HIGH" }
            );
        }

        // The controller stopped reading early. Flags and MSG are untouched,
        // so the Daisy's next poll retries — nothing is lost.
        Ok(ReadStatus::LeftoverBytes(n)) => {
            warn!(
                "System Bus: controller stopped {} bytes early, N={} retained",
                n, count
            );
        }

        // The controller wanted more than a frame. Shouldn't happen against a
        // fixed 97-byte read; the frame did go out, but rather than reason
        // about a controller that is not following the protocol, retain the
        // flags and let the next poll re-report.
        Ok(ReadStatus::NeedMoreBytes) => {
            warn!("System Bus: controller read past the frame; N={} retained", count);
            if let Err(e) = slave.respond_till_stop(0xFF).await {
                warn!("System Bus: fill response failed: {}", e);
            }
        }

        Err(e) => {
            warn!("System Bus: respond failed ({}), N={} retained", e, count);
        }
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

#[embassy_executor::main]
async fn main(spawner: Spawner) {
    let p = embassy_rp::init(Default::default());
    info!("spirant-hw-interface starting");

    // —— Pin assignments ————————————————————————————————————————————————————
    // Interface Bus (I2C0, controller):
    //   I2C_SDA → GP20  (p.PIN_20)
    //   I2C_SCL → GP21  (p.PIN_21)
    //   ENC_INT → GP19  (p.PIN_19)  active-low, pull-up enabled
    // System Bus (I2C1, target 0x20):
    //   SYS_SDA → GP10  (p.PIN_10)
    //   SYS_SCL → GP11  (p.PIN_11)
    //   MSG_OUT → GP22  (p.PIN_22)  push-pull, LOW = changes pending
    //
    // The two buses are electrically and logically separate: OLED flushes and
    // encoder reads can never block or collide with System Bus transactions.
    // ———————————————————————————————————————————————————————————————————————

    // Initialise I2C0, shared between the encoder board and OLED display.
    let i2c = I2c::new_async(
        p.I2C0,
        p.PIN_21, // SCL
        p.PIN_20, // SDA
        Irqs,
        i2c::Config::default(),
    );

    // Wrap in a mutex so both drivers can share the peripheral safely.
    let i2c_bus = I2C_BUS.init(Mutex::new(i2c));

    // Each driver gets its own I2cDevice wrapper. The wrapper acquires the
    // mutex before each I2C transaction and releases it after, serialising
    // bus access automatically.
    let i2c_encoder = I2cDevice::new(i2c_bus);
    let i2c_oled = I2cDevice::new(i2c_bus);

    // Encoder board. DEFAULT_ADDRESS is 0x49 (confirmed by hardware testing;
    // design documents show 0x36 which is incorrect).
    let mut encoder_board = QuadEncoderBoard::new(i2c_encoder, DEFAULT_ADDRESS);

    // OLED display at the standard SSD1306 I2C address.
    let oled_driver = OledDriver::new(i2c_oled, 0x3C);

    // Encoder INT pin: active-low, pull-up enabled.
    let int_pin = Input::new(p.PIN_19, Pull::Up);

    // Initialise shared parameter state with every slot flagged for the
    // Daisy. A freshly booted (or rebooted) Pico therefore presents MSG LOW
    // with the full state pending, which is what makes re-sync after a reboot
    // use the ordinary MSG/GetChanged path rather than a special case
    // (protocol §3.2).
    let param_values = PARAM_VALUES.init(Mutex::new({
        let mut values = ParameterValues::new();
        values.mark_all_changed_i2c();
        values
    }));

    // —— System Bus ————————————————————————————————————————————————————————

    // MSG starts LOW to match that all-flagged boot state. Push-pull, since
    // the Daisy is the only reader and enables its internal pull-up.
    let msg_pin = Output::new(p.PIN_22, Level::Low);

    let mut system_bus_config = i2c_slave::Config::default();
    system_bus_config.addr = SYSTEM_BUS_ADDR;
    // We are a point-to-point target; general calls are noise here.
    system_bus_config.general_call = false;

    let system_bus = I2cSlave::new(
        p.I2C1,
        p.PIN_11, // SCL
        p.PIN_10, // SDA
        Irqs,
        system_bus_config,
    );

    // —— Encoder initialisation —————————————————————————————————————————————

    // Read initial positions so the first delta calculation starts from the
    // correct hardware baseline. On failure the encoder task uses [0; 4].
    match encoder_board.read_all_positions().await {
        Ok(positions) => info!(
            "Initial encoder positions: [{}, {}, {}, {}]",
            positions[0], positions[1], positions[2], positions[3]
        ),
        Err(_) => warn!("Could not read initial encoder positions"),
    }

    // Enable hardware interrupts. Without this the INT pin never fires and
    // the encoder task sleeps forever inside wait_for_low(). On failure we
    // log an error and continue — encoder input simply will not work.
    if let Err(_) = encoder_board.enable_all_interrupts().await {
        error!("Failed to enable encoder interrupts");
    }

    // Clear any stale interrupt flags that accumulated at power-on before
    // interrupts were enabled, so INT starts HIGH and clean.
    if let Err(_) = encoder_board.clear_interrupt_flags().await {
        warn!("Failed to clear initial interrupt flags");
    }

    // Configure the four encoder push-buttons as INPUT_PULLUP for page
    // navigation. Polled by the encoder task — no GPIO interrupts enabled.
    if let Err(_) = encoder_board.configure_buttons().await {
        error!("Failed to configure encoder buttons");
    }

    // —— Spawn tasks ————————————————————————————————————————————————————————

    let display_config = DisplayConfig::default(); // 30 Hz refresh rate

    spawner.spawn(oled_task(oled_driver, param_values, display_config)).unwrap();
    spawner.spawn(encoder_task(int_pin, encoder_board, param_values)).unwrap();
    spawner.spawn(system_bus_task(system_bus, msg_pin, param_values)).unwrap();

    info!("All tasks spawned");
}
