// Host test for src/params -- runs on the Mac, no Arduino toolchain needed:
//
//   make -C spirant-hw-interface-esp32/tests run
//
// Plan section 1.7's exit criterion: clamping, page mapping, and flag
// semantics. The expectations here are the same ones asserted on the Pico side
// in spirant-parameter-values-rs; if you change one, change the other.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../spirant_hw_interface/src/params/parameter_values.h"

// --- Table sanity ------------------------------------------------------------

static void test_spec_table()
{
    uint8_t active = 0;
    for (uint8_t p = 0; p < kNumPages; ++p)
    {
        for (uint8_t e = 0; e < kParamsPerPage; ++e)
        {
            const ParamSpec& s = kParamSpecs[p][e];
            if (!s.active) continue;

            ++active;
            assert(s.min <= s.max);
            assert(s.def >= s.min && s.def <= s.max);
            assert(s.step > 0);
            assert(s.label != nullptr && s.label[0] != '\0');
            // The display column is narrow; the Rust spec caps labels at 5.
            assert(strlen(s.label) <= 5);
        }
    }
    assert(active == kNumActiveParams);
    assert(kTotalSlots == 24);

    printf("  spec table                ok (%u active of %u)\n", active, kTotalSlots);
}

// The five null indices the Daisy's param_table.h also names: 2, 3, 10, 11, 23.
static void test_null_slots()
{
    const uint8_t expected_null[] = {2, 3, 10, 11, 23};

    for (uint8_t i = 0; i < kTotalSlots; ++i)
    {
        bool should_be_null = false;
        for (size_t k = 0; k < sizeof(expected_null); ++k)
        {
            if (expected_null[k] == i) should_be_null = true;
        }

        const ParamSpec& s = kParamSpecs[i / kParamsPerPage][i % kParamsPerPage];
        assert(s.active != should_be_null);
    }

    printf("  null slot positions       ok\n");
}

// --- Defaults ----------------------------------------------------------------

static void test_defaults()
{
    ParameterValues pv;

    assert(pv.currentPage() == 0);

    // Every slot seeded to its spec default, both flags clear.
    for (uint8_t i = 0; i < kTotalSlots; ++i)
    {
        const Parameter* p = pv.paramByGlobalIdx(i);
        assert(p != nullptr);
        assert(p->value == p->spec->def);
        assert(!p->changed_display);
        assert(!p->changed_midi);
    }

    // Spot-check against PARAM_SPECS in the Rust.
    assert(pv.paramByGlobalIdx(0)->value == 15);    // Waveshape
    assert(pv.paramByGlobalIdx(6)->value == 150);   // Cutoff Floor, Hz
    assert(pv.paramByGlobalIdx(16)->value == 409);  // Delay Time, ms
    assert(pv.paramByGlobalIdx(22)->value == 7000); // Damp LP, Hz

    assert(pv.countActiveParams(0) == 2);
    assert(pv.countActiveParams(1) == 4);
    assert(pv.countActiveParams(5) == 3);
    assert(pv.countActiveParams(kNumPages) == 0);  // out of range

    printf("  defaults                  ok\n");
}

// --- Clamping ----------------------------------------------------------------

static void test_clamping()
{
    ParameterValues pv;

    // Page 0 encoder 0: Waveshape, 0..100 step 1, default 15.
    pv.updateFromEncoder(0, 42);
    assert(pv.paramByGlobalIdx(0)->value == 57);

    // Clamp at the top, do not wrap.
    pv.updateFromEncoder(0, 1000);
    assert(pv.paramByGlobalIdx(0)->value == 100);
    pv.updateFromEncoder(0, 1);
    assert(pv.paramByGlobalIdx(0)->value == 100);

    // Clamp at the bottom.
    pv.updateFromEncoder(0, -1000);
    assert(pv.paramByGlobalIdx(0)->value == 0);
    pv.updateFromEncoder(0, -1);
    assert(pv.paramByGlobalIdx(0)->value == 0);

    // Step is applied per detent: Cutoff Floor is step 5, min 20, default 150.
    pv.setPage(1);
    pv.updateFromEncoder(2, 3);
    assert(pv.paramByGlobalIdx(6)->value == 165);
    pv.updateFromEncoder(2, -100);
    assert(pv.paramByGlobalIdx(6)->value == 20);  // clamps to min, not 20-ish

    // A non-zero minimum clamps to the minimum, not to zero.
    pv.setPage(5);
    pv.updateFromEncoder(2, -10000);
    assert(pv.paramByGlobalIdx(22)->value == 1000);  // Damp LP min

    printf("  clamping                  ok\n");
}

static void test_null_and_bounds_are_noops()
{
    ParameterValues pv;

    // Page 0 slots 2 and 3 are null: turning those encoders does nothing and
    // must not set any flag.
    pv.updateFromEncoder(2, 10);
    pv.updateFromEncoder(3, -10);
    assert(!pv.anyChangedDisplay());
    assert(!pv.anyChangedMidi());

    // Out-of-range encoder index is a silent no-op, as in the Rust.
    pv.updateFromEncoder(9, 10);
    assert(!pv.anyChangedDisplay());

    assert(pv.paramByGlobalIdx(kTotalSlots) == nullptr);
    assert(pv.paramAt(kNumPages, 0) == nullptr);
    assert(pv.paramAt(0, kParamsPerPage) == nullptr);

    printf("  null / out-of-range       ok\n");
}

// --- Page mapping ------------------------------------------------------------

static void test_page_mapping()
{
    ParameterValues pv;

    // Encoder N maps to the active page's slot N.
    pv.setPage(4);
    pv.updateFromEncoder(1, 5);  // Delay Feedback, default 40 step 1
    assert(pv.paramByGlobalIdx(17)->value == 45);
    // Page 0's slot 1 is untouched.
    assert(pv.paramByGlobalIdx(1)->value == 50);

    // Global index is page * 4 + encoder, both directions.
    for (uint8_t p = 0; p < kNumPages; ++p)
    {
        for (uint8_t e = 0; e < kParamsPerPage; ++e)
        {
            const uint8_t gi = p * kParamsPerPage + e;
            assert(pv.paramAt(p, e) == pv.paramByGlobalIdx(gi));
        }
    }

    assert(pv.setPage(kNumPages) == ParameterError::InvalidPageIndex);
    assert(pv.setActivePage(kNumPages) == ParameterError::InvalidPageIndex);
    assert(pv.currentPage() == 4);  // rejected, so unchanged

    printf("  page mapping              ok\n");
}

static void test_page_wrapping()
{
    ParameterValues pv;

    assert(pv.currentPage() == 0);
    pv.pageBackward();
    assert(pv.currentPage() == kNumPages - 1);
    pv.pageForward();
    assert(pv.currentPage() == 0);

    for (uint8_t i = 0; i < kNumPages; ++i) pv.pageForward();
    assert(pv.currentPage() == 0);

    printf("  page wrapping             ok\n");
}

// --- Flag semantics ----------------------------------------------------------

static void test_encoder_sets_both_flags()
{
    ParameterValues pv;
    ParameterChange changes[kTotalSlots];

    pv.updateFromEncoder(0, 1);
    assert(pv.anyChangedDisplay());
    assert(pv.anyChangedMidi());

    size_t n = pv.takeDisplayChanges(changes, kTotalSlots);
    assert(n == 1);
    assert(changes[0].value == 16);
    assert(changes[0].page == 0 && changes[0].encoder == 0);
    assert(changes[0].global_idx == 0);
    assert(strcmp(changes[0].name, "Waveshape") == 0);

    // Display flag cleared, MIDI flag untouched.
    assert(!pv.anyChangedDisplay());
    assert(pv.anyChangedMidi());
    assert(pv.takeDisplayChanges(changes, kTotalSlots) == 0);

    n = pv.takeMidiChanges(changes, kTotalSlots);
    assert(n == 1);
    assert(changes[0].value == 16);
    assert(!pv.anyChangedMidi());
    assert(pv.takeMidiChanges(changes, kTotalSlots) == 0);

    printf("  encoder sets both flags   ok\n");
}

static void test_midi_write_does_not_echo()
{
    ParameterValues pv;
    ParameterChange changes[kTotalSlots];

    assert(pv.updateFromMidi(5, 77) == ParameterError::None);  // Brightness
    assert(pv.paramByGlobalIdx(5)->value == 77);

    // Display yes, MIDI no -- otherwise the value bounces straight back.
    assert(pv.anyChangedDisplay());
    assert(!pv.anyChangedMidi());
    assert(pv.takeDisplayChanges(changes, kTotalSlots) == 1);

    // Inbound values clamp too.
    assert(pv.updateFromMidi(5, 9999) == ParameterError::None);
    assert(pv.paramByGlobalIdx(5)->value == 100);
    assert(pv.updateFromMidi(5, -9999) == ParameterError::None);
    assert(pv.paramByGlobalIdx(5)->value == 0);

    // Null slot and out-of-range are distinguishable errors.
    assert(pv.updateFromMidi(2, 10) == ParameterError::NullSlot);
    assert(pv.updateFromMidi(kTotalSlots, 10) == ParameterError::InvalidGlobalIndex);

    printf("  midi write does not echo  ok\n");
}

static void test_page_switch_flags_display_only()
{
    ParameterValues pv;
    ParameterChange changes[kTotalSlots];

    // setPage: no flags. setActivePage: display flags on the new page only.
    pv.setPage(1);
    assert(!pv.anyChangedDisplay());

    pv.setActivePage(1);
    size_t n = pv.takeDisplayChanges(changes, kTotalSlots);
    assert(n == 4);  // Filter has four active slots
    for (size_t i = 0; i < n; ++i) assert(changes[i].page == 1);
    assert(!pv.anyChangedMidi());  // a page switch is not news for the Daisy

    // A page with null slots flags only its active ones.
    pv.setActivePage(5);
    n = pv.takeDisplayChanges(changes, kTotalSlots);
    assert(n == 3);

    printf("  page switch flags         ok\n");
}

static void test_take_changes_ordering_and_capacity()
{
    ParameterValues pv;
    ParameterChange changes[kTotalSlots];

    pv.markAllChangedDisplay();
    size_t n = pv.takeDisplayChanges(changes, kTotalSlots);
    assert(n == kNumActiveParams);

    // Ascending global index, so a consumer can rely on the order.
    for (size_t i = 1; i < n; ++i) assert(changes[i].global_idx > changes[i - 1].global_idx);

    // A short buffer must not clear flags it could not report -- a dropped
    // flag is a value that never reaches the screen.
    pv.markAllChangedDisplay();
    n = pv.takeDisplayChanges(changes, 3);
    assert(n == 3);
    n = pv.takeDisplayChanges(changes, kTotalSlots);
    assert(n == kNumActiveParams - 3);
    assert(!pv.anyChangedDisplay());

    pv.markAllChangedMidi();
    assert(pv.anyChangedMidi());
    n = pv.takeMidiChanges(changes, kTotalSlots);
    assert(n == kNumActiveParams);
    assert(!pv.anyChangedMidi());

    printf("  take/capacity semantics   ok\n");
}

int main()
{
    printf("parameter_values_test\n");

    test_spec_table();
    test_null_slots();
    test_defaults();
    test_clamping();
    test_null_and_bounds_are_noops();
    test_page_mapping();
    test_page_wrapping();
    test_encoder_sets_both_flags();
    test_midi_write_does_not_echo();
    test_page_switch_flags_display_only();
    test_take_changes_ordering_and_capacity();

    printf("parameter_values_test passed\n");
    return 0;
}
