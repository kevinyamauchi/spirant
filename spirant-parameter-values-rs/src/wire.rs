//! System Bus wire format — response frame serialization and CRC-8.
//!
//! This module implements the Pico (I2C target) side of the
//! `GetAll` / `GetChanged` response frame specified in
//! `plans/inter_mcu_communication_protocol.md` §5.2. It lives in this crate
//! rather than in the firmware crate so that it is host-testable: the frame
//! layout and the CRC are a cross-language contract with the Daisy's C++
//! decoder (`spirant-daisy/v1/comm/`), and that contract is only worth
//! anything if both sides are pinned by tests.
//!
//! # Frame layout (97 bytes, fixed)
//!
//! ```text
//! byte  0        record count N (0..=19)
//! bytes 1..1+5N  N records, 5 bytes each, ascending global_idx
//! bytes ..95     0xFF padding
//! byte  96       CRC-8 over bytes 0..=95
//! ```
//!
//! Each record is `[global_idx: u8][value: i32 big-endian]`. A full
//! `GetAll` frame (`N = 19`) has no padding: 1 + 5×19 + 1 = 97 exactly.
//!
//! The length is fixed so the Daisy can issue one constant-size
//! `ReceiveBlocking` with no variable-length parsing mid-transfer.
//!
//! # CRC
//!
//! [`crc8`] is the SMBus PEC: polynomial `0x07`, initial value `0x00`, no
//! input/output reflection, no final XOR (CRC-8/SMBUS in the Rocksoft
//! catalogue, check value `0xF4` over `"123456789"`). The Daisy validates
//! the CRC **before** decoding any record; a mismatch discards the whole
//! frame and escalates to the `GetAll` fallback (protocol §4.4).

use crate::parameter_values::{ParameterValues, PARAMS_PER_PAGE};

/// Total length of a response frame in bytes.
pub const FRAME_LEN: usize = 97;

/// Maximum number of records a frame can carry (the number of active slots).
pub const MAX_RECORDS: usize = 19;

/// Size of one record: 1 index byte + 4 value bytes.
pub const RECORD_LEN: usize = 5;

/// Byte written into unused frame space.
pub const PAD_BYTE: u8 = 0xFF;

/// Index of the CRC byte within the frame.
pub const CRC_OFFSET: usize = FRAME_LEN - 1;

/// A `(global_idx, value)` pair as it was written into a response frame.
///
/// Returned by [`serialize_frame`] and consumed by
/// [`ParameterValues::clear_i2c_flags_if_unchanged`] once the frame is known
/// to have been fully transmitted.
pub type Reported = (u8, i32);

/// Compute the SMBus PEC (CRC-8, poly `0x07`, init `0x00`) over `data`.
///
/// # Examples
///
/// ```
/// use spirant::wire::crc8;
///
/// // Canonical CRC-8/SMBUS check value.
/// assert_eq!(crc8(b"123456789"), 0xF4);
/// assert_eq!(crc8(&[]), 0x00);
/// ```
pub fn crc8(data: &[u8]) -> u8 {
    let mut crc: u8 = 0x00;
    for &byte in data {
        crc ^= byte;
        for _ in 0..8 {
            crc = if crc & 0x80 != 0 {
                (crc << 1) ^ 0x07
            } else {
                crc << 1
            };
        }
    }
    crc
}

/// Serialize a response frame into `buf` and report what went into it.
///
/// Walks the parameter table in ascending `global_idx` order, emitting one
/// record per active slot — every active slot when `only_changed` is
/// `false` (`GetAll`), or only those with `changed_i2c` set when it is
/// `true` (`GetChanged`). Null slots never appear. The remainder of the
/// frame is padded with [`PAD_BYTE`] and byte 96 is set to the CRC.
///
/// Returns the reported `(global_idx, value)` pairs as a fixed array plus a
/// count; callers iterate `&reported[..count]`. That list is the input to
/// [`ParameterValues::clear_i2c_flags_if_unchanged`], which must be called
/// only **after** the controller has fully read the frame.
///
/// The returned count always equals `buf[0]`.
///
/// # Examples
///
/// ```
/// use spirant::parameter_values::ParameterValues;
/// use spirant::wire::{crc8, serialize_frame, FRAME_LEN, CRC_OFFSET};
///
/// let pv = ParameterValues::new();
/// let mut buf = [0u8; FRAME_LEN];
///
/// // GetAll: every active slot.
/// let (_reported, count) = serialize_frame(&pv, false, &mut buf);
/// assert_eq!(count, 19);
/// assert_eq!(buf[0], 19);
/// assert_eq!(buf[CRC_OFFSET], crc8(&buf[..CRC_OFFSET]));
///
/// // GetChanged on a fresh table: nothing pending.
/// let (_reported, count) = serialize_frame(&pv, true, &mut buf);
/// assert_eq!(count, 0);
/// ```
pub fn serialize_frame(
    values: &ParameterValues,
    only_changed: bool,
    buf: &mut [u8; FRAME_LEN],
) -> ([Reported; MAX_RECORDS], usize) {
    let mut reported = [(0u8, 0i32); MAX_RECORDS];
    let mut count = 0usize;
    let mut offset = 1usize;

    for (page_idx, page) in values.pages.iter().enumerate() {
        for (enc_idx, slot) in page.params.iter().enumerate() {
            let Some(param) = slot.as_ref() else { continue };
            if only_changed && !param.changed_i2c {
                continue;
            }

            // Guaranteed by the table layout (19 active slots), but the
            // frame is fixed-size, so refuse to overrun it rather than
            // trusting the invariant.
            if count >= MAX_RECORDS {
                break;
            }

            let global_idx = (page_idx * PARAMS_PER_PAGE + enc_idx) as u8;
            buf[offset] = global_idx;
            buf[offset + 1..offset + RECORD_LEN].copy_from_slice(&param.value.to_be_bytes());
            offset += RECORD_LEN;

            reported[count] = (global_idx, param.value);
            count += 1;
        }
    }

    buf[0] = count as u8;
    for byte in &mut buf[offset..CRC_OFFSET] {
        *byte = PAD_BYTE;
    }
    buf[CRC_OFFSET] = crc8(&buf[..CRC_OFFSET]);

    (reported, count)
}

// ── Unit Tests ───────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parameter_values::{ParameterSlot, N_PAGES};

    /// Global indices that are `None` in `PARAM_SPECS` and must never be
    /// emitted in a frame (protocol §6).
    const NULL_INDICES: [u8; 5] = [2, 3, 10, 11, 23];

    // ── CRC golden vectors ───────────────────────────────────────────
    //
    // These literals are the cross-language contract. The identical
    // vectors are asserted in spirant-daisy/v1/tests/crc8_test.cpp; if you
    // change one side you must change the other, and the frame fixture
    // below will fail until you do.
    //
    // Independently cross-checked against a reference CRC-8/SMBUS
    // implementation (poly 0x07, init 0x00, no reflection, no final XOR).

    #[test]
    fn crc8_golden_vectors() {
        assert_eq!(crc8(&[]), 0x00, "empty slice");
        assert_eq!(crc8(&[0x00]), 0x00, "single zero byte");
        assert_eq!(crc8(&[0xFF]), 0xF3, "single 0xFF byte");
        assert_eq!(crc8(&[0x01, 0x02, 0x03, 0x04]), 0xE3, "short sequence");
        // Canonical check value for CRC-8/SMBUS.
        assert_eq!(crc8(b"123456789"), 0xF4, "canonical check value");
    }

    #[test]
    fn crc8_all_zero_96_byte_frame() {
        // A 96-byte all-zero payload: exercises the full frame length with
        // a trivially reproducible input.
        assert_eq!(crc8(&[0x00; CRC_OFFSET]), 0x00);
    }

    #[test]
    fn crc8_all_ff_96_byte_frame() {
        assert_eq!(crc8(&[0xFF; CRC_OFFSET]), 0xBD);
    }

    #[test]
    fn crc8_detects_single_bit_flips() {
        let mut frame = [0u8; CRC_OFFSET];
        for (i, byte) in frame.iter_mut().enumerate() {
            *byte = i as u8;
        }
        let good = crc8(&frame);

        for bit in 0..8 {
            let mut corrupted = frame;
            corrupted[42] ^= 1 << bit;
            assert_ne!(crc8(&corrupted), good, "bit {} flip went undetected", bit);
        }
    }

    // ── GetAll framing ───────────────────────────────────────────────

    #[test]
    fn getall_reports_all_nineteen_slots() {
        let pv = ParameterValues::new();
        let mut buf = [0u8; FRAME_LEN];

        let (reported, count) = serialize_frame(&pv, false, &mut buf);

        assert_eq!(count, MAX_RECORDS);
        assert_eq!(buf[0], MAX_RECORDS as u8);
        // 1 + 5*19 + 1 == 97: a full frame has no padding at all.
        assert_eq!(1 + count * RECORD_LEN, CRC_OFFSET);
        assert_eq!(reported.len(), MAX_RECORDS);
    }

    #[test]
    fn getall_indices_ascend_and_skip_nulls() {
        let pv = ParameterValues::new();
        let mut buf = [0u8; FRAME_LEN];
        let (reported, count) = serialize_frame(&pv, false, &mut buf);

        let mut previous: Option<u8> = None;
        for &(idx, _) in &reported[..count] {
            if let Some(prev) = previous {
                assert!(idx > prev, "indices must strictly ascend: {} after {}", idx, prev);
            }
            assert!(
                !NULL_INDICES.contains(&idx),
                "null slot {} must never be reported",
                idx
            );
            assert!(idx < (N_PAGES * PARAMS_PER_PAGE) as u8);
            previous = Some(idx);
        }
    }

    #[test]
    fn getall_values_match_the_table_and_are_big_endian() {
        let pv = ParameterValues::new();
        let mut buf = [0u8; FRAME_LEN];
        let (reported, count) = serialize_frame(&pv, false, &mut buf);

        for (record, &(idx, value)) in reported[..count].iter().enumerate() {
            let offset = 1 + record * RECORD_LEN;
            assert_eq!(buf[offset], idx);
            assert_eq!(
                i32::from_be_bytes(buf[offset + 1..offset + RECORD_LEN].try_into().unwrap()),
                value
            );
            assert_eq!(pv.get_param_by_global_idx(idx as usize).unwrap().value, value);
        }
    }

    #[test]
    fn crc_covers_bytes_zero_through_ninety_five() {
        let pv = ParameterValues::new();
        let mut buf = [0u8; FRAME_LEN];
        serialize_frame(&pv, false, &mut buf);

        assert_eq!(buf[CRC_OFFSET], crc8(&buf[..CRC_OFFSET]));
    }

    // ── GetChanged framing ───────────────────────────────────────────

    #[test]
    fn getchanged_with_no_flags_yields_empty_frame() {
        let pv = ParameterValues::new();
        let mut buf = [0u8; FRAME_LEN];

        let (_, count) = serialize_frame(&pv, true, &mut buf);

        assert_eq!(count, 0);
        assert_eq!(buf[0], 0);
        // Everything between the count byte and the CRC is padding.
        assert!(buf[1..CRC_OFFSET].iter().all(|&b| b == PAD_BYTE));
        assert_eq!(buf[CRC_OFFSET], crc8(&buf[..CRC_OFFSET]));
    }

    #[test]
    fn getchanged_with_one_flag_yields_one_record_and_pads_the_rest() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 1); // Waveshape: 15 -> 16

        let mut buf = [0u8; FRAME_LEN];
        let (reported, count) = serialize_frame(&pv, true, &mut buf);

        assert_eq!(count, 1);
        assert_eq!(reported[0], (0, 16));
        assert_eq!(buf[0], 1);
        assert_eq!(buf[1], 0);
        assert_eq!(i32::from_be_bytes(buf[2..6].try_into().unwrap()), 16);
        assert!(
            buf[6..CRC_OFFSET].iter().all(|&b| b == PAD_BYTE),
            "unused bytes must be 0xFF padding"
        );
    }

    #[test]
    fn getchanged_with_all_flags_matches_getall() {
        let mut pv = ParameterValues::new();
        pv.mark_all_changed_i2c();

        let mut changed_buf = [0u8; FRAME_LEN];
        let (_, changed_count) = serialize_frame(&pv, true, &mut changed_buf);

        let mut all_buf = [0u8; FRAME_LEN];
        let (_, all_count) = serialize_frame(&pv, false, &mut all_buf);

        assert_eq!(changed_count, MAX_RECORDS);
        assert_eq!(changed_count, all_count);
        assert_eq!(changed_buf, all_buf);
    }

    #[test]
    fn getchanged_reports_only_flagged_slots() {
        let mut pv = ParameterValues::new();
        pv.set_page(4).unwrap(); // Delay
        pv.update_from_encoder(1, 2); // Delay Feedback: 40 + 2*1 = 42 (global 17)
        pv.set_page(1).unwrap(); // Filter
        pv.update_from_encoder(2, 1); // Cutoff Floor: 150 + 5 = 155 (global 6)

        let mut buf = [0u8; FRAME_LEN];
        let (reported, count) = serialize_frame(&pv, true, &mut buf);

        assert_eq!(count, 2);
        // Ascending global_idx, regardless of the order they were touched.
        assert_eq!(reported[0], (6, 155));
        assert_eq!(reported[1], (17, 42));
    }

    #[test]
    fn serialize_is_read_only_and_repeatable() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 1);

        let mut first = [0u8; FRAME_LEN];
        serialize_frame(&pv, true, &mut first);
        let mut second = [0u8; FRAME_LEN];
        serialize_frame(&pv, true, &mut second);

        // Serializing must not clear flags — that only happens after the
        // frame is confirmed transmitted (protocol §5.2).
        assert_eq!(first, second);
        assert!(pv.any_changed_i2c());
    }

    #[test]
    fn stale_buffer_contents_are_fully_overwritten() {
        let pv = ParameterValues::new();
        let mut buf = [0xAAu8; FRAME_LEN];

        serialize_frame(&pv, true, &mut buf);

        assert!(
            !buf[..CRC_OFFSET].contains(&0xAA),
            "a short frame must not leak bytes from a previous longer frame"
        );
    }

    // ── Boot state ───────────────────────────────────────────────────

    #[test]
    fn boot_state_reports_all_nineteen_via_getchanged() {
        let mut pv = ParameterValues::new();
        pv.mark_all_changed_i2c();

        let mut buf = [0u8; FRAME_LEN];
        let (_, count) = serialize_frame(&pv, true, &mut buf);

        assert!(pv.any_changed_i2c(), "MSG must be LOW at boot");
        assert_eq!(count, MAX_RECORDS);
    }

    #[test]
    fn any_changed_i2c_tracks_the_msg_invariant() {
        let mut pv = ParameterValues::new();
        assert!(!pv.any_changed_i2c());

        pv.update_from_encoder(0, 1);
        assert!(pv.any_changed_i2c());

        let mut buf = [0u8; FRAME_LEN];
        let (reported, count) = serialize_frame(&pv, true, &mut buf);
        pv.clear_i2c_flags_if_unchanged(&reported[..count]);
        assert!(!pv.any_changed_i2c());
    }

    #[test]
    fn mark_all_changed_i2c_leaves_oled_flags_alone() {
        let mut pv = ParameterValues::new();
        pv.mark_all_changed_i2c();

        let (_, oled_count) = pv.take_oled_changes();
        assert_eq!(oled_count, 0);
    }

    // ── clear-if-unchanged (protocol §5.2) ───────────────────────────

    #[test]
    fn clear_if_unchanged_clears_a_settled_slot() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 1); // Waveshape -> 16

        pv.clear_i2c_flags_if_unchanged(&[(0, 16)]);

        assert!(!pv.get_param_by_global_idx(0).unwrap().changed_i2c);
    }

    #[test]
    fn clear_if_unchanged_retains_a_slot_that_moved_mid_transmission() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 1); // Waveshape -> 16

        // Snapshot the frame the way system_bus_task would...
        let mut buf = [0u8; FRAME_LEN];
        let (reported, count) = serialize_frame(&pv, true, &mut buf);
        assert_eq!(reported[0], (0, 16));

        // ...then the user turns the encoder again while the 97 bytes are
        // still being clocked out.
        pv.update_from_encoder(0, 1); // Waveshape -> 17

        pv.clear_i2c_flags_if_unchanged(&reported[..count]);

        assert!(
            pv.get_param_by_global_idx(0).unwrap().changed_i2c,
            "the newer value must stay pending, or it is lost forever"
        );
        // The next GetChanged carries the newer value.
        let (reported, count) = serialize_frame(&pv, true, &mut buf);
        assert_eq!(count, 1);
        assert_eq!(reported[0], (0, 17));
    }

    #[test]
    fn clear_if_unchanged_retains_slots_that_were_not_reported() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 1); // global 0
        pv.update_from_encoder(1, 1); // global 1

        // Only slot 0 made it into the frame.
        pv.clear_i2c_flags_if_unchanged(&[(0, 16)]);

        assert!(!pv.get_param_by_global_idx(0).unwrap().changed_i2c);
        assert!(pv.get_param_by_global_idx(1).unwrap().changed_i2c);
    }

    #[test]
    fn clear_if_unchanged_ignores_null_and_out_of_range_indices() {
        let mut pv = ParameterValues::new();
        pv.mark_all_changed_i2c();

        // Null slots and a past-the-end index must not panic.
        pv.clear_i2c_flags_if_unchanged(&[(2, 0), (23, 0), (200, 0)]);

        assert!(pv.any_changed_i2c());
    }

    #[test]
    fn clear_if_unchanged_does_not_touch_oled_flags() {
        let mut pv = ParameterValues::new();
        pv.update_from_encoder(0, 1);

        pv.clear_i2c_flags_if_unchanged(&[(0, 16)]);

        let (_, oled_count) = pv.take_oled_changes();
        assert_eq!(oled_count, 1, "the OLED must still redraw the change");
    }

    #[test]
    fn full_round_trip_clears_every_boot_flag() {
        let mut pv = ParameterValues::new();
        pv.mark_all_changed_i2c();

        let mut buf = [0u8; FRAME_LEN];
        let (reported, count) = serialize_frame(&pv, true, &mut buf);
        pv.clear_i2c_flags_if_unchanged(&reported[..count]);

        assert!(!pv.any_changed_i2c(), "MSG must go HIGH after a full sync");
        for page in &pv.pages {
            for slot in &page.params {
                if let ParameterSlot::Active(param) = slot {
                    assert!(!param.changed_i2c);
                }
            }
        }
    }

    // ── Cross-language fixture ───────────────────────────────────────

    /// Emits the default-state `GetAll` frame as a C array and asserts it
    /// against a checked-in literal.
    ///
    /// The literal below is the byte-for-byte contract with the Daisy's C++
    /// decoder: the same 97 bytes appear in
    /// `spirant-daisy/v1/tests/frame_test.cpp`. This test failing after a
    /// `PARAM_SPECS` edit is not a bug — it means the C++ fixture is now
    /// stale and must be regenerated:
    ///
    /// ```text
    /// cargo test -p spirant wire::tests::default_getall_frame -- --nocapture
    /// ```
    ///
    /// Paste the printed array into both this test and `frame_test.cpp`.
    #[test]
    fn default_getall_frame() {
        let pv = ParameterValues::new();
        let mut buf = [0u8; FRAME_LEN];
        let (_, count) = serialize_frame(&pv, false, &mut buf);
        assert_eq!(count, MAX_RECORDS);

        // Print in C-array syntax so the paste into frame_test.cpp is
        // mechanical rather than transcribed by hand.
        println!("// Default-state GetAll frame, generated by");
        println!("// cargo test -p spirant wire::tests::default_getall_frame -- --nocapture");
        println!("static const uint8_t kDefaultGetAllFrame[97] = {{");
        for chunk in buf.chunks(12) {
            let mut line = String::from("   ");
            for byte in chunk {
                line.push_str(&format!(" 0x{:02X},", byte));
            }
            println!("{}", line);
        }
        println!("}};");

        #[rustfmt::skip]
        const EXPECTED: [u8; FRAME_LEN] = [
            0x13,
            0x00, 0x00, 0x00, 0x00, 0x0F,
            0x01, 0x00, 0x00, 0x00, 0x32,
            0x04, 0x00, 0x00, 0x00, 0x23,
            0x05, 0x00, 0x00, 0x00, 0x42,
            0x06, 0x00, 0x00, 0x00, 0x96,
            0x07, 0x00, 0x00, 0x00, 0x00,
            0x08, 0x00, 0x00, 0x00, 0x00,
            0x09, 0x00, 0x00, 0x00, 0x28,
            0x0C, 0x00, 0x00, 0x00, 0x32,
            0x0D, 0x00, 0x00, 0x00, 0x23,
            0x0E, 0x00, 0x00, 0x00, 0x3C,
            0x0F, 0x00, 0x00, 0x00, 0x14,
            0x10, 0x00, 0x00, 0x01, 0x99,
            0x11, 0x00, 0x00, 0x00, 0x28,
            0x12, 0x00, 0x00, 0x00, 0x32,
            0x13, 0x00, 0x00, 0x00, 0x50,
            0x14, 0x00, 0x00, 0x00, 0x4B,
            0x15, 0x00, 0x00, 0x00, 0x55,
            0x16, 0x00, 0x00, 0x1B, 0x58,
            0x35,
        ];

        assert_eq!(
            buf, EXPECTED,
            "the default GetAll frame changed — regenerate the C++ fixture too"
        );
    }
}
