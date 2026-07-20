#include "daisy_pod.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

DaisyPod hw;

// --- Oscillators under test -------------------------------------------------
// Four DaisySP oscillators auditioned side by side. SW2 cycles the selection,
// LED2 encodes it by color, and the two pots tweak the selected oscillator's
// two most expressive (non-pitch) parameters. Pitch always comes from the
// shared arpeggio below.
constexpr int kHarmonics = 16;

VariableShapeOscillator     var_shape;
VariableShapeOscillator     var_shape_sync;
VariableSawOscillator       var_saw;
ZOscillator                 zosc;
VosimOscillator             vosim;
HarmonicOscillator<kHarmonics> harmonic;

enum OscType
{
    OSC_VAR_SHAPE = 0,   // red
    OSC_VAR_SHAPE_SYNC,  // magenta
    OSC_VAR_SAW,         // green
    OSC_ZOSC,            // blue
    OSC_VOSIM,           // yellow
    OSC_HARMONIC,        // cyan
    OSC_COUNT
};

int selected_osc = OSC_VAR_SHAPE;

// --- Arpeggio test settings -------------------------------------------------
// Plays a fixed arpeggio so oscillators can be auditioned without a MIDI/breath
// controller. All notes sound at the same level (no per-note dynamics).
constexpr float kTempoBpm     = 110.f;
constexpr float kStepsPerBeat = 2.f;  // eighth notes
constexpr float kArpStepHz    = kTempoBpm / 60.f * kStepsPerBeat;
constexpr float kPlayLevel    = 0.4f;  // constant output level (40%)

// MIDI notes of the arpeggio: C major triad ascending over one octave.
constexpr uint8_t kArpNotes[] = {60, 64, 67, 72};  // C4 E4 G4 C5
constexpr size_t  kArpLength  = sizeof(kArpNotes) / sizeof(kArpNotes[0]);

Metro  arp_clock;
size_t arp_index = 0;

// Most recent note frequency, used by the sync oscillator to derive its slave
// frequency as a ratio of the current note.
float g_note_freq = 440.f;

// Set the fundamental on every oscillator so switching is instantly in tune.
// Note the setter names differ between modules.
void set_pitch(float freq)
{
    g_note_freq = freq;
    // VariableShapeOscillator's audible (slave) pitch is SetSyncFreq; SetFreq
    // only feeds the sync master, which is inert while sync is disabled.
    var_shape.SetSyncFreq(freq);
    // In sync mode the master (SetFreq) is the perceived pitch; the slave
    // frequency is knob-driven in update_params.
    var_shape_sync.SetFreq(freq);
    var_saw.SetFreq(freq);
    zosc.SetFreq(freq);
    vosim.SetFreq(freq);
    harmonic.SetFreq(freq);
}

// HarmonicOscillator has a whole spectrum (16 amplitudes) rather than a couple
// of scalar params, so the two knobs drive macros that generate the spectrum:
//   brightness -> harmonic rolloff (sine -> saw)
//   even_odd   -> attenuate even harmonics (hollow -> full)
void update_harmonic(float brightness, float even_odd)
{
    // Rolloff factor; capped below 1 so the spectrum never goes fully flat.
    const float decay = fmap(brightness, 0.f, 0.92f);

    float amps[kHarmonics];
    float sum = 0.f;
    float a   = 1.f;  // decay^i
    for(int i = 0; i < kHarmonics; ++i)
    {
        float amp = a;
        if(((i + 1) % 2) == 0)  // harmonic number i+1 even -> attenuate
            amp *= even_odd;
        amps[i] = amp;
        sum += amp;
        a *= decay;
    }

    // Normalize so amplitudes sum to < 1 (module requirement) with headroom.
    const float norm = (sum > 0.f) ? 0.9f / sum : 0.f;
    for(int i = 0; i < kHarmonics; ++i)
        harmonic.SetSingleAmp(amps[i] * norm, i);
}

// Map the two pots to the selected oscillator's parameters. Ranges chosen to
// span each parameter's musically useful region.
void update_params(float k1, float k2)
{
    switch(selected_osc)
    {
        case OSC_VAR_SHAPE:
            var_shape.SetWaveshape(k1);              // saw/tri -> square
            var_shape.SetPW(fmap(k2, 0.05f, 0.95f)); // pulse width / shape
            break;
        case OSC_VAR_SHAPE_SYNC:
            // Hard sync: pitch is the master (set in set_pitch). Knob 1 sweeps
            // the slave frequency from 1x to 8x the note for the sync sweep;
            // knob 2 morphs the underlying waveform.
            var_shape_sync.SetSyncFreq(g_note_freq * fmap(k1, 1.f, 8.f));
            var_shape_sync.SetWaveshape(k2);         // saw/tri -> square
            break;
        case OSC_VAR_SAW:
            var_saw.SetWaveshape(k1);                // 0 = notch, 1 = slope
            var_saw.SetPW(fmap(k2, -1.f, 1.f));      // notch/slope amount
            break;
        case OSC_ZOSC:
            zosc.SetFormantFreq(
                fmap(k1, 200.f, 4000.f, Mapping::EXP)); // formant peak
            zosc.SetShape(k2);                          // contour (0-1)
            break;
        case OSC_VOSIM:
            vosim.SetForm1Freq(
                fmap(k1, 200.f, 4000.f, Mapping::EXP)); // formant peak 1
            vosim.SetForm2Freq(
                fmap(k2, 200.f, 4000.f, Mapping::EXP)); // formant peak 2
            break;
        case OSC_HARMONIC:
            update_harmonic(k1, k2);  // k1 = brightness, k2 = even/odd
            break;
        default: break;
    }
}

// LED2 color per oscillator, at constant full brightness.
void update_led2()
{
    switch(selected_osc)
    {
        case OSC_VAR_SHAPE: hw.led2.Set(1.f, 0.f, 0.f); break;       // red
        case OSC_VAR_SHAPE_SYNC: hw.led2.Set(1.f, 0.f, 1.f); break;  // magenta
        case OSC_VAR_SAW: hw.led2.Set(0.f, 1.f, 0.f); break;    // green
        case OSC_ZOSC: hw.led2.Set(0.f, 0.f, 1.f); break;       // blue
        case OSC_VOSIM: hw.led2.Set(1.f, 1.f, 0.f); break;      // yellow
        case OSC_HARMONIC: hw.led2.Set(0.f, 1.f, 1.f); break;   // cyan
        default: hw.led2.Set(0.f, 0.f, 0.f); break;
    }
}

float process_selected()
{
    switch(selected_osc)
    {
        case OSC_VAR_SHAPE: return var_shape.Process();
        case OSC_VAR_SHAPE_SYNC: return var_shape_sync.Process();
        case OSC_VAR_SAW: return var_saw.Process();
        case OSC_ZOSC: return zosc.Process();
        case OSC_VOSIM: return vosim.Process();
        case OSC_HARMONIC: return harmonic.Process();
        default: return 0.f;
    }
}

void audio_callback(AudioHandle::InterleavingInputBuffer  in,
                    AudioHandle::InterleavingOutputBuffer out,
                    size_t                                size)
{
    hw.ProcessAllControls();

    // SW2 cycles through the oscillators.
    if(hw.button2.RisingEdge())
        selected_osc = (selected_osc + 1) % OSC_COUNT;

    // Pots set the selected oscillator's parameters.
    update_params(hw.knob1.Value(), hw.knob2.Value());

    // LED1: white status, brightness proportional to the play level.
    hw.led1.Set(kPlayLevel, kPlayLevel, kPlayLevel);
    // LED2: color-coded to the selected oscillator.
    update_led2();
    hw.UpdateLeds();

    for(size_t i = 0; i < size; i += 2)
    {
        if(arp_clock.Process())
        {
            arp_index = (arp_index + 1) % kArpLength;
            set_pitch(mtof(kArpNotes[arp_index]));
        }

        float sample = process_selected() * kPlayLevel;
        out[i]     = sample;  // left
        out[i + 1] = sample;  // right
    }
}

void init_synth(float sample_rate)
{
    var_shape.Init(sample_rate);
    var_shape_sync.Init(sample_rate);
    var_saw.Init(sample_rate);
    zosc.Init(sample_rate);
    vosim.Init(sample_rate);
    harmonic.Init(sample_rate);

    // Enable hard sync on the dedicated sync oscillator and give it a fixed
    // pulse width; its slave frequency and waveshape are knob-driven.
    var_shape_sync.SetSync(true);
    var_shape_sync.SetPW(0.5f);

    // Fixed third parameters (the pots drive the other two per oscillator).
    zosc.SetMode(0.f);   // phase-shift/offset blend, neutral
    vosim.SetShape(0.f); // waveshaping, neutral

    set_pitch(mtof(kArpNotes[0]));

    arp_clock.Init(kArpStepHz, sample_rate);
}

int main()
{
    hw.Init();
    hw.SetAudioBlockSize(48);
    float sample_rate = hw.AudioSampleRate();
    init_synth(sample_rate);

    hw.StartAdc();
    hw.StartAudio(audio_callback);

    for(;;) {}
}
