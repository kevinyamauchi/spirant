#include "daisy_pod.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

DaisyPod hw;

// --- Michael Brecker EWI-style lead -----------------------------------------
// A saw-ish VariableShapeOscillator run through the classic expressive-lead
// chain: resonant lowpass -> overdrive -> chorus (mono thickener), with a
// stereo ping-pong delay widening the field, into a hall reverb.
//
//   voice (var_shape)
//     -> LadderFilter (LP24)      breath-driven, resonant "vocal" cutoff
//     -> Overdrive                dynamic soft-clip grit (post-envelope)
//     -> Chorus (mono mix)        lush analog thickening, kept dead center
//     +  stereo ping-pong delay   width, fed from the mono pre-chorus lead
//     -> ReverbSc (stereo)        hall space
//
// The key to the EWI feel is that breath pressure moves *both* brightness and
// volume together. There is no breath controller here, so a per-note ADSR
// envelope stands in: it opens the filter and swells the amplitude on each note.
// The envelope amplitude is applied *before* the overdrive, so soft notes stay
// clean and hard notes bite -- dynamic timbre, like a real reed.
//
// The dry lead is kept dead center (mono); stereo width comes entirely from the
// ping-pong delay's L/R taps (and the reverb).
//
// Pots:
//   knob1 -> ping-pong delay time
//   knob2 -> overdrive amount (SetDrive)
// Encoder:
//   turn  -> reverb send amount (dry -> wet), accumulated from detent ticks
VariableShapeOscillator var_shape;
LadderFilter            filt;
Overdrive               drive;
Chorus                  chorus;
ReverbSc                verb;
Adsr                    env;

// Filter brightness ceiling, fixed at 66% (knob1 free).
constexpr float kBrightness = 0.66f;

// Reverb send (dry + wet). The encoder nudges this live; starts at 75% wet.
constexpr float kReverbStep = 0.05f;   // change per encoder detent
float           reverb_send = 0.75f;   // current send, clamped to [0, 1]

// --- Arpeggio test settings -------------------------------------------------
// Plays a fixed arpeggio so the lead can be auditioned without a MIDI/breath
// controller.
constexpr float kTempoBpm     = 110.f;
constexpr float kStepsPerBeat = 2.f;  // eighth notes
constexpr float kArpStepHz    = kTempoBpm / 60.f * kStepsPerBeat;
constexpr float kPlayLevel    = 0.4f;  // peak output level (before reverb)

// Fraction of each arp step the note is held "on" (gate high). The remainder
// lets the envelope release, giving each note an attack/release breath shape.
constexpr float kGateFraction = 0.8f;

// MIDI notes of the arpeggio: C major triad ascending over one octave.
constexpr uint8_t kArpNotes[] = {60, 64, 67, 72};  // C4 E4 G4 C5
constexpr size_t  kArpLength  = sizeof(kArpNotes) / sizeof(kArpNotes[0]);

Metro   arp_clock;
size_t  arp_index      = 0;
int     gate_samples   = 0;  // samples remaining with the note gated on
int     step_samples   = 0;  // total samples per arp step (set in init)

// --- Stereo ping-pong delay -------------------------------------------------
// Two delay lines fed from the mono lead. The left tap feeds the right line and
// the right tap feeds (damped) back into the left, so repeats bounce L->R->L
// and decay. The delay lines are large, so they live in SDRAM.
#define MAX_DELAY ((size_t)(48000 * 1.0f))  // 1 s per line @ 48 kHz
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay_l;
DelayLine<float, MAX_DELAY> DSY_SDRAM_BSS delay_r;
OnePole                     delay_damp;  // darkens repeats in the feedback loop

constexpr float kDelayFeedback = 0.4f;   // repeat decay
constexpr float kDelayMix      = 0.5f;   // wet level of the ping-pong taps

// Knob1 sweeps the delay time over this range (seconds), mapped exponentially
// so the low end has fine control. Kept below MAX_DELAY (1 s).
constexpr float kDelayMinSec = 0.04f;
constexpr float kDelayMaxSec = 0.75f;

// Dotted-eighth = 3/4 of a beat: a musical default the knob starts near.
constexpr float kBeatSec      = 60.f / kTempoBpm;
constexpr float kDelayTimeSec = 0.75f * kBeatSec;

float g_sample_rate    = 48000.f;  // set in init, needed to convert time->samples
float delay_time_smooth = 0.f;     // glided delay length in samples

// Set the oscillator pitch. VariableShapeOscillator's audible (slave) pitch is
// SetSyncFreq; SetFreq only feeds the (disabled) sync master.
void set_pitch(float freq)
{
    var_shape.SetSyncFreq(freq);
}

void audio_callback(AudioHandle::InterleavingInputBuffer  in,
                    AudioHandle::InterleavingOutputBuffer out,
                    size_t                                size)
{
    hw.ProcessAllControls();

    // Encoder ticks nudge the reverb send (ProcessAllControls already debounced
    // the encoder). Increment() returns -1/0/+1 per detent; clamp to [0, 1].
    reverb_send = fclamp(reverb_send + hw.encoder.Increment() * kReverbStep,
                         0.f,
                         1.f);

    const float drive_amount = hw.knob2.Value();  // overdrive

    // Knob1 sets the ping-pong delay time (samples), smoothed per-sample below.
    const float delay_target = g_sample_rate
        * fmap(hw.knob1.Value(), kDelayMinSec, kDelayMaxSec, Mapping::EXP);

    // Fixed saw-leaning timbre for the classic analog lead.
    var_shape.SetWaveshape(0.15f);  // toward saw/ramp
    var_shape.SetPW(0.5f);

    drive.SetDrive(drive_amount);

    // Cutoff ceiling the envelope opens toward, mapped exponentially so the
    // fixed 66% brightness sits musically in the 400 Hz .. 9 kHz range.
    const float cutoff_ceiling = fmap(kBrightness, 400.f, 9000.f, Mapping::EXP);

    // LED1: white status, brightness proportional to the play level.
    hw.led1.Set(kPlayLevel, kPlayLevel, kPlayLevel);
    // LED2: blue, brightness proportional to the reverb send (encoder).
    hw.led2.Set(0.f, 0.f, reverb_send);
    hw.UpdateLeds();

    for(size_t i = 0; i < size; i += 2)
    {
        if(arp_clock.Process())
        {
            arp_index = (arp_index + 1) % kArpLength;
            set_pitch(mtof(kArpNotes[arp_index]));
            gate_samples = static_cast<int>(step_samples * kGateFraction);
        }

        // Gate stays high for the first part of the step, then releases.
        const bool gate = gate_samples > 0;
        if(gate_samples > 0)
            gate_samples--;

        // Breath stand-in: one envelope drives brightness and amplitude.
        const float e = env.Process(gate);

        // Envelope opens the filter from a nearly-closed base toward the
        // knob-set ceiling; squared for a more natural swell.
        const float cutoff = 150.f + e * e * (cutoff_ceiling - 150.f);
        filt.SetFreq(cutoff);

        float sig = var_shape.Process();
        sig = filt.Process(sig);
        sig *= e;               // breath dynamics feed the drive (level-dependent)
        sig = drive.Process(sig);
        sig *= kPlayLevel;      // output trim after the drive's gain

        // Chorus as a mono center thickener (take its mono mix, not L/R), so the
        // direct lead stays dead center. sig is the mono pre-chorus lead.
        float dry = chorus.Process(sig);

        // Glide the delay length toward the knob target to avoid zipper/pitch
        // artifacts, then apply it to both lines.
        fonepole(delay_time_smooth, delay_target, 0.0005f);
        delay_l.SetDelay(delay_time_smooth);
        delay_r.SetDelay(delay_time_smooth);

        // Ping-pong: left tap feeds the right line; right tap feeds (damped)
        // back into the left. Repeats alternate L->R->L and darken as they decay.
        float dl = delay_l.Read();
        float dr = delay_r.Read();
        delay_l.Write(sig + delay_damp.Process(dr) * kDelayFeedback);
        delay_r.Write(dl * kDelayFeedback);

        // Dry dead center; ping-pong taps spread hard L/R.
        float pre_l = dry + dl * kDelayMix;
        float pre_r = dry + dr * kDelayMix;

        // Reverb send (stereo in/out); wet amount set live by the encoder.
        float wetl, wetr;
        verb.Process(pre_l, pre_r, &wetl, &wetr);
        out[i]     = pre_l + wetl * reverb_send;  // left
        out[i + 1] = pre_r + wetr * reverb_send;  // right
    }
}

void init_synth(float sample_rate)
{
    var_shape.Init(sample_rate);

    filt.Init(sample_rate);
    filt.SetFilterMode(LadderFilter::FilterMode::LP24);
    filt.SetRes(0.35f);  // slight vocal resonant peak

    drive.Init();

    chorus.Init(sample_rate);
    chorus.SetLfoFreq(0.5f);    // slow analog drift
    chorus.SetLfoDepth(0.35f);
    chorus.SetDelay(0.6f);
    chorus.SetFeedback(0.2f);

    verb.Init(sample_rate);
    verb.SetFeedback(0.85f);  // long hall tail
    verb.SetLpFreq(7000.f);   // natural, not-too-bright tail

    delay_l.Init();
    delay_r.Init();
    // Seed the smoothed time at the musical default so it doesn't glide up from
    // zero at startup; knob1 takes over from there.
    delay_time_smooth = sample_rate * kDelayTimeSec;
    delay_l.SetDelay(delay_time_smooth);
    delay_r.SetDelay(delay_time_smooth);

    delay_damp.Init();
    delay_damp.SetFilterMode(OnePole::FILTER_MODE_LOW_PASS);
    delay_damp.SetFrequency(0.08f);  // ~3.8 kHz @ 48 kHz, darkens each repeat

    env.Init(sample_rate);
    env.SetAttackTime(0.05f);
    env.SetDecayTime(0.1f);
    env.SetSustainLevel(0.9f);
    env.SetReleaseTime(0.15f);

    set_pitch(mtof(kArpNotes[0]));

    arp_clock.Init(kArpStepHz, sample_rate);
    step_samples = static_cast<int>(sample_rate / kArpStepHz);
    gate_samples = static_cast<int>(step_samples * kGateFraction);
}

int main()
{
    hw.Init();
    hw.SetAudioBlockSize(48);
    float sample_rate = hw.AudioSampleRate();
    g_sample_rate     = sample_rate;
    init_synth(sample_rate);

    hw.StartAdc();
    hw.StartAudio(audio_callback);

    for(;;) {}
}
