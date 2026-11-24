// Headless tests for VODeclicker (run by ctest on every platform in CI).
// Drives the plugin's processor directly, no plugin host needed.
//
// Each test states a promise the plugin makes, in words a user would recognise.
// The test signal is a synthetic "voice": syllables of a breathy harmonic tone
// with pauses between them and quiet room noise underneath, with mouth clicks
// added at known places so we can measure exactly how much of each is removed.
//
// Extra mode for real recordings:
//   VODeclickerTest --render take.wav [outDir]
// processes the take at a few settings, writes take_<setting>.wav files, and
// prints how many spans were repaired and how much was changed.
//   VODeclickerTest --sweep take.wav outDir freqHz lengthMs [sensitivity%] [strong] [--wav]
// processes the take at one setting with Listen on and writes
// take_f<freq>_l<len>_repairs.txt (Audacity labels); --wav also writes what was
// removed. Run it in parallel over a grid to compare Frequency/Length settings.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
#include <random>

namespace
{
int failures = 0;
int checks = 0;

#define CHECK(cond, ...)                                          \
    do {                                                          \
        ++checks;                                                 \
        if (! (cond)) {                                           \
            ++failures;                                           \
            std::printf ("FAIL %s:%d: ", __FILE__, __LINE__);     \
            std::printf (__VA_ARGS__);                            \
            std::printf ("\n");                                   \
        }                                                         \
    } while (0)

constexpr int kBlock = 512;
const double kRates[] = { 44100.0, 48000.0, 96000.0 };

void setParam (VODeclickerProcessor& p, const char* id, float plainValue)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

std::unique_ptr<VODeclickerProcessor> makeProcessor (double fs, int channels)
{
    auto p = std::make_unique<VODeclickerProcessor>();
    juce::AudioProcessor::BusesLayout layout;
    const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    layout.inputBuses.add (set);
    layout.outputBuses.add (set);
    p->setBusesLayout (layout);
    p->setPlayConfigDetails (channels, channels, fs, kBlock);
    p->prepareToPlay (fs, kBlock);
    return p;
}

juce::AudioBuffer<float> process (VODeclickerProcessor& p, const juce::AudioBuffer<float>& input)
{
    juce::AudioBuffer<float> out (input);
    juce::MidiBuffer midi;
    for (int start = 0; start < out.getNumSamples(); start += kBlock)
    {
        const int len = std::min (kBlock, out.getNumSamples() - start);
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), out.getNumChannels(), start, len);
        p.processBlock (view, midi);
    }
    return out;
}

// ---- Synthetic voice ---------------------------------------------------------
struct Take
{
    juce::AudioBuffer<float> clean;     // voice + room, no clicks
    juce::AudioBuffer<float> withClicks;
    std::vector<int> clickPositions;
};

constexpr double kSyllable = 0.25, kPause = 0.15;   // seconds

// level: overall scale. Peaks of the voice sit near 0.25 * level (-12 dBFS at level 1).
Take makeTake (double fs, int channels, double seconds, float level, bool clicksInPauses,
               bool clicksInWords, float clickAmp = 0.05f, uint32_t seed = 1)
{
    const int n = (int) (seconds * fs);
    Take t;
    t.clean.setSize (channels, n);
    t.clean.clear();

    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> uni (0.0, 1.0);
    std::normal_distribution<double> gauss (0.0, 1.0);

    const double f0 = 140.0;
    std::vector<double> phase (64);
    for (auto& ph : phase) ph = uni (rng) * juce::MathConstants<double>::twoPi;

    const double period = kSyllable + kPause;
    const double ramp = 0.025;
    double aspState = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double time = i / fs;
        const double pos = std::fmod (time, period);
        double env = 0.0;
        if (pos < kSyllable)
        {
            env = 1.0;
            if (pos < ramp) env = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * pos / ramp);
            else if (pos > kSyllable - ramp) env = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (kSyllable - pos) / ramp);
        }

        // Harmonics up to 8 kHz falling at 6 dB/octave, plus some breath (high-passed noise).
        double voice = 0.0;
        const double vibrato = 1.0 + 0.01 * std::sin (juce::MathConstants<double>::twoPi * 5.0 * time);
        for (int h = 1; h * f0 < 8000.0 && h < 64; ++h)
            voice += std::cos (juce::MathConstants<double>::twoPi * h * f0 * vibrato * time + phase[(size_t) h]) / h;
        const double noise = gauss (rng);
        const double breath = noise - aspState;   // crude high-pass
        aspState = noise;
        const double room = gauss (rng) * 0.0003;  // about -70 dBFS

        const double s = level * (0.12 * env * voice + 0.004 * env * breath + room);
        for (int ch = 0; ch < channels; ++ch)
            t.clean.setSample (ch, i, (float) s);
    }

    t.withClicks = t.clean;

    // A mouth click: a ~1 ms damped burst centred around 4-6 kHz, with a 0.1 ms attack.
    auto addClick = [&] (int at, float amp)
    {
        const double f = 4000.0 + 2000.0 * uni (rng);
        const double ph = uni (rng) * juce::MathConstants<double>::twoPi;
        const int len = (int) (0.0012 * fs);
        for (int k = 0; k < len && at + k < n; ++k)
        {
            const double tt = k / fs;
            const double attack = std::min (1.0, tt / 0.0001);
            const double v = amp * level * attack * std::exp (-tt / 0.0003) * std::sin (juce::MathConstants<double>::twoPi * f * tt + ph);
            for (int ch = 0; ch < channels; ++ch)
                t.withClicks.setSample (ch, at + k, t.withClicks.getSample (ch, at + k) + (float) v);
        }
        t.clickPositions.push_back (at);
    };

    const int syllables = (int) (seconds / period);
    for (int s = 1; s < syllables; ++s)   // skip the first second-ish so detectors have settled
    {
        const double start = s * period;
        if (clicksInPauses)
            addClick ((int) ((start + kSyllable + 0.03 + 0.08 * uni (rng)) * fs), clickAmp);
        if (clicksInWords)
            addClick ((int) ((start + 0.08 + 0.1 * uni (rng)) * fs), clickAmp);
    }
    return t;
}

// How much louder than the clean take (no click) the click window is, above
// `highBandHz`, in dB. For the input this is how loud the click is; for the
// output, ~0 dB means the click is gone and the rebuild sounds like what was
// there. (Error-vs-clean would be unfair: the rebuilt breath/room noise is a
// different noise from the original, with the same level and colour.)
double excessDb (const juce::AudioBuffer<float>& audio, int offset, const juce::AudioBuffer<float>& clean,
                 int pos, double fs, double highBandHz)
{
    const int a = pos - (int) (0.002 * fs), b = pos + (int) (0.006 * fs);
    const int warm = (int) (0.01 * fs);
    double e = 0.0, c = 0.0;
    for (int ch = 0; ch < clean.getNumChannels(); ++ch)
    {
        declick::Butter4 hpA, hpC;
        hpA.set (true, fs, highBandHz);
        hpC.set (true, fs, highBandHz);
        for (int i = a - warm; i < b; ++i)
        {
            const double x = hpA.process (audio.getSample (ch, i + offset));
            const double y = hpC.process (clean.getSample (ch, i));
            if (i >= a) { e += x * x; c += y * y; }
        }
    }
    return 10.0 * std::log10 ((e + 1.0e-20) / (c + 1.0e-20));
}

struct ClickStats { double loudestBefore = -1.0e9, loudestAfter = -1.0e9, quietestBefore = 1.0e9; };

// Measured well inside the repaired band (1.5 x Frequency), where the promise is clear-cut.
ClickStats clickStats (const Take& t, const juce::AudioBuffer<float>& out, int latency, double fs,
                       double highBandHz = 4500.0)
{
    ClickStats s;
    for (int pos : t.clickPositions)
    {
        const double before = excessDb (t.withClicks, 0, t.clean, pos, fs, highBandHz);
        const double after = excessDb (out, latency, t.clean, pos, fs, highBandHz);
        s.loudestBefore = std::max (s.loudestBefore, before);
        s.quietestBefore = std::min (s.quietestBefore, before);
        s.loudestAfter = std::max (s.loudestAfter, after);
    }
    return s;
}

// True if `out` is `in` delayed by `latency` samples, bit for bit.
bool isDelayedCopy (const juce::AudioBuffer<float>& in, const juce::AudioBuffer<float>& out, int latency)
{
    for (int ch = 0; ch < in.getNumChannels(); ++ch)
        for (int i = 0; i + latency < in.getNumSamples(); ++i)
            if (out.getSample (ch, i + latency) != in.getSample (ch, i))
                return false;
    return true;
}

const char* chName (int ch) { return ch == 1 ? "mono" : "stereo"; }

// ---- Tests -------------------------------------------------------------------

// "It reports its latency, and it's about 24 ms."
void testLatency()
{
    for (double fs : kRates)
    {
        auto p = makeProcessor (fs, 2);
        const double ms = 1000.0 * p->getLatencySamples() / fs;
        CHECK (ms >= 23.9 && ms <= 24.1, "latency at %.0f Hz is %.2f ms", fs, ms);
    }
}

// "On a voice without clicks it changes nothing: the output is the input, delayed."
void testCleanVoiceUntouched()
{
    for (double fs : kRates)
        for (int ch : { 1, 2 })
        {
            auto t = makeTake (fs, ch, 4.0, 1.0f, false, false);
            auto p = makeProcessor (fs, ch);
            const auto out = process (*p, t.clean);
            CHECK (p->getRepairCount() == 0, "%.0f Hz %s: %u repairs on a clean voice", fs, chName (ch), p->getRepairCount());
            CHECK (isDelayedCopy (t.clean, out, p->getLatencySamples()),
                   "%.0f Hz %s: clean voice not passed through bit-exact", fs, chName (ch));
        }
}

// "Clicks in the pauses are removed, every one: afterwards the click's few
//  milliseconds are no more than 3 dB louder (above Frequency) than the take
//  without the click."
void testClicksInPausesRemoved()
{
    for (double fs : kRates)
        for (int ch : { 1, 2 })
        {
            auto t = makeTake (fs, ch, 4.0, 1.0f, true, false);
            auto p = makeProcessor (fs, ch);
            const auto out = process (*p, t.withClicks);
            const auto st = clickStats (t, out, p->getLatencySamples(), fs);
            CHECK (st.quietestBefore > 15.0, "%.0f Hz %s: test clicks too quiet (%.1f dB)", fs, chName (ch), st.quietestBefore);
            CHECK (st.loudestAfter <= 3.0, "%.0f Hz %s: a click is still %.1f dB above the clean take (was up to %.1f dB)",
                   fs, chName (ch), st.loudestAfter, st.loudestBefore);
            CHECK (p->getRepairCount() == t.clickPositions.size(),
                   "%.0f Hz %s: %u repairs for %zu clicks", fs, chName (ch), p->getRepairCount(), t.clickPositions.size());
        }
}

// "A click whose energy sits just under the 3 kHz detection split (2.5-3 kHz)
//  is removed too, not only its top end." (Found against RX Mouth De-click on a
//  real take: many clicks live there, and rebuilding only above 3 kHz left them.)
void testLowerClicksRemoved()
{
    for (double fs : kRates)
    {
        auto t = makeTake (fs, 1, 4.0, 1.0f, false, false);
        t.withClicks = t.clean;
        std::mt19937 rng (5);
        std::uniform_real_distribution<double> uni (0.0, 1.0);
        const double period = kSyllable + kPause;
        std::vector<int> clicks;
        for (int s = 1; s < (int) (4.0 / period); ++s)
        {
            // A ringing click centred at 2.5-3 kHz, in the pause.
            const int at = (int) ((s * period + kSyllable + 0.04 + 0.06 * uni (rng)) * fs);
            const double f = 2500.0 + 500.0 * uni (rng), ph = uni (rng) * juce::MathConstants<double>::twoPi;
            for (int i = 0; i < (int) (0.003 * fs); ++i)
            {
                const double tt = i / fs;
                const double v = 0.05 * std::min (1.0, tt / 0.0001) * std::exp (-tt / 0.0004)
                               * std::sin (juce::MathConstants<double>::twoPi * f * tt + ph);
                t.withClicks.setSample (0, at + i, t.withClicks.getSample (0, at + i) + (float) v);
            }
            clicks.push_back (at);
        }
        auto p = makeProcessor (fs, 1);
        const auto out = process (*p, t.withClicks);
        double before = 1.0e9, worst = -1.0e9;
        for (int pos : clicks)
        {
            before = std::min (before, excessDb (t.withClicks, 0, t.clean, pos, fs, 2000.0));
            worst = std::max (worst, excessDb (out, p->getLatencySamples(), t.clean, pos, fs, 2000.0));
        }
        CHECK (before > 15.0, "%.0f Hz: test clicks too quiet above 2 kHz (%.1f dB)", fs, before);
        CHECK (worst <= 3.0, "%.0f Hz: a 2.5-3 kHz click is still %.1f dB above the clean take above 2 kHz", fs, worst);
    }
}

// "A lip smack (a quick train of pops, each one landing before the last has
//  settled) is removed too, pop by pop, not mistaken for a consonant."
void testLipSmacksRemoved()
{
    for (double fs : kRates)
    {
        auto t = makeTake (fs, 1, 4.0, 1.0f, false, false);
        t.withClicks = t.clean;
        std::mt19937 rng (7);
        std::uniform_real_distribution<double> uni (0.0, 1.0);
        const double period = kSyllable + kPause;
        std::vector<int> pops;
        for (int s = 1; s < (int) (4.0 / period); ++s)
        {
            // Four ringing pops 2.2-3.2 ms apart, within 3 dB of each other, in the
            // pause: each one lands while the last is still ringing.
            int at = (int) ((s * period + kSyllable + 0.04 + 0.04 * uni (rng)) * fs);
            for (int k = 0; k < 4; ++k)
            {
                const double f = 4000.0 + 2000.0 * uni (rng), ph = uni (rng) * juce::MathConstants<double>::twoPi;
                const double amp = 0.03 * std::pow (10.0, -3.0 * uni (rng) / 20.0);
                for (int i = 0; i < (int) (0.004 * fs); ++i)
                {
                    const double tt = i / fs;
                    const double v = amp * std::min (1.0, tt / 0.0001) * std::exp (-tt / 0.0006)
                                   * std::sin (juce::MathConstants<double>::twoPi * f * tt + ph);
                    t.withClicks.setSample (0, at + i, t.withClicks.getSample (0, at + i) + (float) v);
                }
                pops.push_back (at);
                at += (int) ((0.0022 + 0.001 * uni (rng)) * fs);
            }
        }
        for (float sens : { 20.0f, 50.0f })
        {
            auto p = makeProcessor (fs, 1);
            setParam (*p, VODeclickerProcessor::sensitivityId, sens);
            const auto out = process (*p, t.withClicks);
            double worst = -1.0e9;
            for (int pos : pops)
                worst = std::max (worst, excessDb (out, p->getLatencySamples(), t.clean, pos, fs, 4500.0));
            CHECK (worst <= 3.0, "%.0f Hz sensitivity %.0f%%: a lip smack pop is still %.1f dB above the clean take",
                   fs, sens, worst);
        }
    }
}

// "It works the same on a quiet take (-29 LUFS-ish) as on a hot one: no dBFS thresholds."
void testLevelIndependent()
{
    const double fs = 48000.0;
    for (float level : { 0.1f, 1.0f, 3.0f })
    {
        auto t = makeTake (fs, 1, 4.0, level, true, false);
        auto p = makeProcessor (fs, 1);
        const auto out = process (*p, t.withClicks);
        const auto st = clickStats (t, out, p->getLatencySamples(), fs);
        CHECK (st.loudestAfter <= 3.0, "level x%.1f: a click is still %.1f dB above the clean take", level, st.loudestAfter);
        CHECK (p->getRepairCount() == t.clickPositions.size(),
               "level x%.1f: %u repairs for %zu clicks", level, p->getRepairCount(), t.clickPositions.size());
    }
}

// "Sounds longer than Length (an 's', breath) are speech, and are left alone."
void testLongSoundsLeftAlone()
{
    const double fs = 48000.0;
    for (double burstMs : { 12.0, 60.0 })
    {
        auto t = makeTake (fs, 1, 3.0, 1.0f, false, false);
        std::mt19937 rng (7);
        std::normal_distribution<double> gauss (0.0, 1.0);
        // Fricative-like noise bursts in each pause, with 2 ms ramps.
        const double period = kSyllable + kPause;
        for (int s = 1; s < (int) (3.0 / period); ++s)
        {
            const int at = (int) ((s * period + kSyllable + 0.04) * fs);
            const int len = (int) (burstMs * 0.001 * fs), r = (int) (0.002 * fs);
            double prev = 0.0;
            for (int k = 0; k < len; ++k)
            {
                const double env = std::min ({ 1.0, k / (double) r, (len - k) / (double) r });
                const double nz = gauss (rng);
                const double v = 0.03 * env * (nz - prev);
                prev = nz;
                t.clean.setSample (0, at + k, t.clean.getSample (0, at + k) + (float) v);
            }
        }
        auto p = makeProcessor (fs, 1);
        const auto out = process (*p, t.clean);
        CHECK (p->getRepairCount() == 0, "%.0f ms bursts: %u repairs (should be left alone)", burstMs, p->getRepairCount());
        CHECK (isDelayedCopy (t.clean, out, p->getLatencySamples()), "%.0f ms bursts were changed", burstMs);
    }
}

// "A consonant release (a 't' or 'k' burst running into breath and the vowel) is
//  not a click, even straight after a pause: it is left alone."
void testConsonantReleasesLeftAlone()
{
    for (double fs : kRates)
    {
        auto t = makeTake (fs, 1, 4.0, 1.0f, false, false);
        std::mt19937 rng (11);
        std::normal_distribution<double> gauss (0.0, 1.0);
        const double period = kSyllable + kPause;
        // Just before each syllable: a 1 ms burst, then aspiration that carries on
        // into the vowel (like "t" in "top"). The vowel onset is 25 ms later.
        for (int s = 1; s < (int) (4.0 / period); ++s)
        {
            const int at = (int) ((s * period - 0.03) * fs);
            const int burst = (int) (0.001 * fs), asp = (int) (0.055 * fs);
            double prev = 0.0;
            for (int k = 0; k < burst + asp; ++k)
            {
                const double nz = gauss (rng);
                const double hf = nz - prev;
                prev = nz;
                const double env = k < burst ? 0.08 : 0.0006;   // aspiration ~10 dB over the room
                t.clean.setSample (0, at + k, t.clean.getSample (0, at + k) + (float) (env * hf));
            }
        }
        for (bool strong : { false, true })
        {
            auto p = makeProcessor (fs, 1);
            setParam (*p, VODeclickerProcessor::strongId, strong ? 1.0f : 0.0f);
            const auto out = process (*p, t.clean);
            CHECK (p->getRepairCount() == 0, "%.0f Hz %s: %u consonant releases treated as clicks", fs,
                   strong ? "Strong" : "Gentle", p->getRepairCount());
            CHECK (isDelayedCopy (t.clean, out, p->getLatencySamples()), "%.0f Hz %s: consonants changed", fs,
                   strong ? "Strong" : "Gentle");
        }
    }
}

// "Gentle leaves clicks inside voiced words alone; Strong removes them too."
void testGentleVsStrong()
{
    const double fs = 48000.0;
    auto t = makeTake (fs, 1, 4.0, 1.0f, false, true, 0.5f);

    auto gentle = makeProcessor (fs, 1);
    const auto outG = process (*gentle, t.withClicks);
    CHECK (gentle->getRepairCount() == 0, "Gentle repaired %u clicks inside words", gentle->getRepairCount());
    CHECK (isDelayedCopy (t.withClicks, outG, gentle->getLatencySamples()), "Gentle changed voiced speech");

    auto strong = makeProcessor (fs, 1);
    setParam (*strong, VODeclickerProcessor::strongId, 1.0f);
    const auto outS = process (*strong, t.withClicks);
    const auto st = clickStats (t, outS, strong->getLatencySamples(), fs);
    CHECK (strong->getRepairCount() == t.clickPositions.size(),
           "Strong: %u repairs for %zu clicks in words", strong->getRepairCount(), t.clickPositions.size());
    CHECK (st.loudestAfter <= 3.0, "Strong: a click in a word is still %.1f dB above the clean take (was up to %.1f dB)",
           st.loudestAfter, st.loudestBefore);
}

// "Sensitivity 0 % turns it off: bit-exact, even with clicks."
void testSensitivityZeroIsOff()
{
    const double fs = 48000.0;
    auto t = makeTake (fs, 2, 3.0, 1.0f, true, true);
    auto p = makeProcessor (fs, 2);
    setParam (*p, VODeclickerProcessor::sensitivityId, 0.0f);
    setParam (*p, VODeclickerProcessor::strongId, 1.0f);
    const auto out = process (*p, t.withClicks);
    CHECK (p->getRepairCount() == 0, "0 %% sensitivity still repaired %u clicks", p->getRepairCount());
    CHECK (isDelayedCopy (t.withClicks, out, p->getLatencySamples()), "0 %% sensitivity isn't bit-exact");
}

// "Bypass gives back exactly the input, delayed by the reported latency."
void testBypassBitExact()
{
    for (double fs : kRates)
    {
        auto t = makeTake (fs, 2, 2.0, 1.0f, true, true);
        auto p = makeProcessor (fs, 2);
        setParam (*p, VODeclickerProcessor::bypassId, 1.0f);
        p->prepareToPlay (fs, kBlock);   // start already bypassed
        const auto out = process (*p, t.withClicks);
        CHECK (isDelayedCopy (t.withClicks, out, p->getLatencySamples()), "%.0f Hz: bypass not bit-exact", fs);
        CHECK (p->getBypassParameter() == p->apvts.getParameter (VODeclickerProcessor::bypassId),
               "host bypass should be the internal Bypass parameter");
    }
}

// "Listen plays exactly what is removed: Listen + normal output = the input."
void testListenIsTheDifference()
{
    const double fs = 48000.0;
    auto t = makeTake (fs, 2, 3.0, 1.0f, true, false);
    auto normal = makeProcessor (fs, 2);
    auto listen = makeProcessor (fs, 2);
    setParam (*listen, VODeclickerProcessor::listenId, 1.0f);
    listen->prepareToPlay (fs, kBlock);
    const auto outN = process (*normal, t.withClicks);
    const auto outL = process (*listen, t.withClicks);
    const int lat = normal->getLatencySamples();
    double maxErr = 0.0, removed = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i + lat < t.withClicks.getNumSamples(); ++i)
        {
            maxErr = std::max (maxErr, (double) std::abs (outN.getSample (ch, i + lat) + outL.getSample (ch, i + lat)
                                                          - t.withClicks.getSample (ch, i)));
            removed = std::max (removed, (double) std::abs (outL.getSample (ch, i + lat)));
        }
    CHECK (maxErr < 1.0e-6, "Listen + output differs from input by %g", maxErr);
    CHECK (removed > 0.01, "Listen output is (nearly) silent: peak %g", removed);
}

// "Turning Frequency or Length while playing can't click: with nothing to repair, output stays bit-exact."
void testKnobMovesAreSilent()
{
    const double fs = 48000.0;
    auto t = makeTake (fs, 1, 3.0, 1.0f, false, false);
    auto p = makeProcessor (fs, 1);
    juce::AudioBuffer<float> out (t.clean);
    juce::MidiBuffer midi;
    int blockNo = 0;
    for (int start = 0; start < out.getNumSamples(); start += kBlock, ++blockNo)
    {
        setParam (*p, VODeclickerProcessor::frequencyId, 1000.0f + 7000.0f * (float) ((blockNo * 37) % 100) / 100.0f);
        setParam (*p, VODeclickerProcessor::lengthId, 0.5f + 7.5f * (float) ((blockNo * 53) % 100) / 100.0f);
        const int len = std::min (kBlock, out.getNumSamples() - start);
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 1, start, len);
        p->processBlock (view, midi);
    }
    CHECK (isDelayedCopy (t.clean, out, p->getLatencySamples()), "knob moves changed a clean voice");
}

// "The low band (below Frequency) of the voice is left alone, even where a click is repaired."
void testLowBandUntouched()
{
    const double fs = 48000.0;
    auto t = makeTake (fs, 1, 4.0, 1.0f, false, true, 0.5f);
    auto p = makeProcessor (fs, 1);
    setParam (*p, VODeclickerProcessor::strongId, 1.0f);
    const auto out = process (*p, t.withClicks);
    const int lat = p->getLatencySamples();

    // Compare below 800 Hz (well under the 1.5 kHz repair split): output vs input, over every repaired click.
    declick::Butter4 lpIn, lpOut;
    lpIn.set (false, fs, 800.0);
    lpOut.set (false, fs, 800.0);
    std::vector<double> lowIn ((size_t) out.getNumSamples()), lowOut ((size_t) out.getNumSamples());
    for (int i = 0; i < out.getNumSamples(); ++i)
    {
        lowIn[(size_t) i] = lpIn.process (i >= lat ? t.withClicks.getSample (0, i - lat) : 0.0);
        lowOut[(size_t) i] = lpOut.process (out.getSample (0, i));
    }
    double diff = 0.0, sig = 0.0;
    for (int pos : t.clickPositions)
        for (int i = pos + lat - (int) (0.005 * fs); i < pos + lat + (int) (0.015 * fs); ++i)
        {
            diff += juce::square (lowOut[(size_t) i] - lowIn[(size_t) i]);
            sig += juce::square (lowIn[(size_t) i]);
        }
    const double db = 10.0 * std::log10 ((diff + 1.0e-20) / (sig + 1.0e-20));
    CHECK (db < -30.0, "low band changed by %.1f dB relative to the voice around repairs", db);
}

void testStateRoundTrip()
{
    auto a = makeProcessor (48000.0, 2);
    setParam (*a, VODeclickerProcessor::sensitivityId, 72.0f);
    setParam (*a, VODeclickerProcessor::frequencyId, 4500.0f);
    setParam (*a, VODeclickerProcessor::lengthId, 6.0f);
    setParam (*a, VODeclickerProcessor::strongId, 1.0f);
    setParam (*a, VODeclickerProcessor::bypassId, 1.0f);
    juce::MemoryBlock state;
    a->getStateInformation (state);

    auto b = makeProcessor (48000.0, 2);
    b->setStateInformation (state.getData(), (int) state.getSize());
    auto get = [&b] (const char* id) { return b->apvts.getRawParameterValue (id)->load(); };
    CHECK (std::abs (get (VODeclickerProcessor::sensitivityId) - 72.0f) < 0.1f, "sensitivity not restored");
    CHECK (std::abs (get (VODeclickerProcessor::frequencyId) - 4500.0f) < 2.0f, "frequency not restored");
    CHECK (std::abs (get (VODeclickerProcessor::lengthId) - 6.0f) < 0.02f, "length not restored");
    CHECK (get (VODeclickerProcessor::strongId) > 0.5f, "mode not restored");
    CHECK (get (VODeclickerProcessor::bypassId) > 0.5f, "bypass not restored");
}

void testDefaults()
{
    auto p = makeProcessor (48000.0, 2);
    auto text = [&p] (const char* id) { auto* q = p->apvts.getParameter (id); return q->getText (q->getValue(), 32); };
    CHECK (text (VODeclickerProcessor::sensitivityId) == "50 %", "sensitivity default shows %s", text (VODeclickerProcessor::sensitivityId).toRawUTF8());
    CHECK (text (VODeclickerProcessor::frequencyId) == "3.0 kHz", "frequency default shows %s", text (VODeclickerProcessor::frequencyId).toRawUTF8());
    CHECK (text (VODeclickerProcessor::lengthId) == "6.0 ms", "length default shows %s", text (VODeclickerProcessor::lengthId).toRawUTF8());
    CHECK (text (VODeclickerProcessor::strongId) == "Gentle", "mode default shows %s", text (VODeclickerProcessor::strongId).toRawUTF8());
}

void testVersionCompare()
{
    CHECK (UpdateChecker::compareVersions ("v1.2.0", "1.1.9") > 0, "v1.2.0 > 1.1.9");
    CHECK (UpdateChecker::compareVersions ("1.0", "1.0.0") == 0, "1.0 == 1.0.0");
    CHECK (UpdateChecker::compareVersions ("0.9.10", "0.10.0") < 0, "0.9.10 < 0.10.0");
    CHECK (UpdateChecker::compareVersions ("1.0.0-beta", "1.0.0") == 0, "pre-release suffix ignored");
}

void testEditorOpens()
{
   #if JUCE_LINUX
    if (juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isEmpty())
    {
        std::printf ("Skipping editor test: no X display.\n");
        return;
    }
   #endif
    auto p = makeProcessor (48000.0, 2);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
    CHECK (editor != nullptr && editor->getWidth() > 0, "editor failed to open");
    CHECK (editor->getHeight() == VODeclickerEditor::collapsedHeight, "editor should open with Advanced closed");

    // Advanced opens below and grows the window; the choice is saved with the session.
    auto* link = dynamic_cast<juce::HyperlinkButton*> (
        [&]() -> juce::Component* {
            for (auto* c : editor->getChildren())
                if (dynamic_cast<juce::HyperlinkButton*> (c) != nullptr)
                    return c;
            return nullptr;
        }());
    CHECK (link != nullptr, "no Advanced link");
    if (link == nullptr)
        return;
    link->onClick();   // what a click calls (triggerClick is async)
    CHECK (editor->getHeight() == VODeclickerEditor::collapsedHeight + VODeclickerEditor::advancedHeight,
           "Advanced should grow the window");

    juce::MemoryBlock state;
    p->getStateInformation (state);
    editor.reset();

    auto q = makeProcessor (48000.0, 2);
    q->setStateInformation (state.getData(), (int) state.getSize());
    std::unique_ptr<juce::AudioProcessorEditor> reopened (q->createEditor());
    CHECK (reopened->getHeight() == VODeclickerEditor::collapsedHeight + VODeclickerEditor::advancedHeight,
           "Advanced open state should be restored");
}

// ---- --render mode -------------------------------------------------------------
double lufsLike (const juce::AudioBuffer<float>& b, int from, int to)
{
    // Plain gated RMS (not K-weighted): enough to compare before/after.
    double sum = 0.0; int n = 0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = from; i < to; ++i) { sum += juce::square ((double) b.getSample (ch, i)); ++n; }
    return 10.0 * std::log10 (sum / std::max (1, n) + 1.0e-20);
}

int render (const juce::File& inFile, juce::File outDir)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (inFile));
    if (reader == nullptr) { std::printf ("Can't read %s\n", inFile.getFullPathName().toRawUTF8()); return 1; }
    if (outDir == juce::File()) outDir = inFile.getParentDirectory();
    outDir.createDirectory();

    const int ch = (int) std::min (2u, reader->numChannels);
    const double fs = reader->sampleRate;
    juce::AudioBuffer<float> input (ch, (int) reader->lengthInSamples);
    reader->read (&input, 0, input.getNumSamples(), 0, true, ch > 1);
    std::printf ("%s: %.1f s, %.0f Hz, %d ch, RMS %.1f dBFS, peak %.1f dBFS\n", inFile.getFileName().toRawUTF8(),
                 input.getNumSamples() / fs, fs, ch, lufsLike (input, 0, input.getNumSamples()),
                 juce::Decibels::gainToDecibels (input.getMagnitude (0, input.getNumSamples())));

    struct Setting { const char* name; float sens; bool strong; bool listen; };
    const Setting settings[] = { { "Gentle50", 50.0f, false, false }, { "Gentle50_listen", 50.0f, false, true },
                                 { "Strong50", 50.0f, true, false },  { "Strong50_listen", 50.0f, true, true },
                                 { "Strong80", 80.0f, true, false } };
    for (const auto& s : settings)
    {
        auto p = makeProcessor (fs, ch);
        setParam (*p, VODeclickerProcessor::sensitivityId, s.sens);
        setParam (*p, VODeclickerProcessor::strongId, s.strong ? 1.0f : 0.0f);
        setParam (*p, VODeclickerProcessor::listenId, s.listen ? 1.0f : 0.0f);
        p->prepareToPlay (fs, kBlock);

        // Pad with the latency so the end of the take comes out, then trim it off the front.
        const int lat = p->getLatencySamples();
        juce::AudioBuffer<float> padded (ch, input.getNumSamples() + lat);
        padded.clear();
        for (int c = 0; c < ch; ++c) padded.copyFrom (c, 0, input, c, 0, input.getNumSamples());
        const auto out = process (*p, padded);
        juce::AudioBuffer<float> trimmed (ch, input.getNumSamples());
        for (int c = 0; c < ch; ++c) trimmed.copyFrom (c, 0, out, c, lat, input.getNumSamples());

        const double secs = input.getNumSamples() / fs;
        std::printf ("  %-16s %5u repairs (%.1f per min), %u not settled", s.name, p->getRepairCount(),
                     p->getRepairCount() / secs * 60.0, p->getEngine().rejected.load());
        if (s.listen)
            std::printf ("  removed: RMS %.1f dBFS, peak %.1f dBFS\n", lufsLike (trimmed, 0, trimmed.getNumSamples()),
                         juce::Decibels::gainToDecibels (trimmed.getMagnitude (0, trimmed.getNumSamples())));
        else
            std::printf ("  output RMS %.2f dB vs input\n",
                         lufsLike (trimmed, 0, trimmed.getNumSamples()) - lufsLike (input, 0, input.getNumSamples()));

        if (s.listen)
        {
            // One line per repair (Audacity label format: start, end, label), plus a
            // summary of how loud the removed bits were and how long the spans were.
            const auto& eng = p->getEngine();
            const int logged = (int) std::min<uint32_t> (p->getRepairCount(), (uint32_t) declick::Engine::kLogSize);
            juce::String labels;
            int peakHist[6] = {}, lenHist[5] = {};
            for (int r = 0; r < logged; ++r)
            {
                const auto span = eng.repairLog[r];
                float peak = 0.0f;
                for (int c = 0; c < ch; ++c)
                    for (int64_t i = span.start; i <= span.end && i < trimmed.getNumSamples(); ++i)
                        peak = std::max (peak, std::abs (trimmed.getSample (c, (int) i)));
                const double peakDb = juce::Decibels::gainToDecibels (peak, -200.0f);
                const double lenMs = 1000.0 * (double) (span.end - span.start + 1) / fs;
                peakHist[peakDb > -20 ? 0 : peakDb > -30 ? 1 : peakDb > -40 ? 2 : peakDb > -50 ? 3 : peakDb > -60 ? 4 : 5]++;
                lenHist[lenMs < 3 ? 0 : lenMs < 4 ? 1 : lenMs < 6 ? 2 : lenMs < 8 ? 3 : 4]++;
                labels << juce::String ((double) span.start / fs, 4) << "\t" << juce::String ((double) span.end / fs, 4)
                       << "\t" << juce::String (peakDb, 1) << " dBFS\n";
            }
            std::printf ("    removed peak per repair: >-20 dB %d | -20..-30 %d | -30..-40 %d | -40..-50 %d | -50..-60 %d | <-60 %d\n",
                         peakHist[0], peakHist[1], peakHist[2], peakHist[3], peakHist[4], peakHist[5]);
            std::printf ("    span length (incl. 2 ms padding): <3 ms %d | 3-4 %d | 4-6 %d | 6-8 %d | >=8 %d%s\n",
                         lenHist[0], lenHist[1], lenHist[2], lenHist[3], lenHist[4],
                         logged < (int) p->getRepairCount() ? "  (first 4096 only)" : "");
            outDir.getChildFile (inFile.getFileNameWithoutExtension() + "_" + s.name + "_repairs.txt").replaceWithText (labels);
        }

        auto outFile = outDir.getChildFile (inFile.getFileNameWithoutExtension() + "_" + s.name + ".wav");
        outFile.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream());
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (fs)
                                                       .withNumChannels (ch)
                                                       .withBitsPerSample (24));
        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer (trimmed, 0, trimmed.getNumSamples());
    }
    return 0;
}
int sweep (const juce::File& inFile, const juce::File& outDir, float freq, float lenMs,
           float sens, bool strong, bool writeWav)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (inFile));
    if (reader == nullptr) { std::printf ("Can't read %s\n", inFile.getFullPathName().toRawUTF8()); return 1; }
    outDir.createDirectory();

    const int ch = (int) std::min (2u, reader->numChannels);
    const double fs = reader->sampleRate;
    auto p = makeProcessor (fs, ch);
    setParam (*p, VODeclickerProcessor::frequencyId, freq);
    setParam (*p, VODeclickerProcessor::lengthId, lenMs);
    setParam (*p, VODeclickerProcessor::sensitivityId, sens);
    setParam (*p, VODeclickerProcessor::strongId, strong ? 1.0f : 0.0f);
    setParam (*p, VODeclickerProcessor::listenId, 1.0f);
    p->prepareToPlay (fs, kBlock);
    const int lat = p->getLatencySamples();

    const int total = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> removed (ch, writeWav ? total : 0);
    juce::AudioBuffer<float> block (ch, kBlock);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < total + lat; pos += kBlock)
    {
        const int n = std::min (kBlock, total + lat - pos);
        block.setSize (ch, n, false, false, true);
        block.clear();
        if (pos < total)
            reader->read (&block, 0, std::min (n, total - pos), pos, true, ch > 1);
        p->processBlock (block, midi);
        if (writeWav)
            for (int i = 0; i < n; ++i)
            {
                const int at = pos + i - lat;
                if (at >= 0 && at < total)
                    for (int c = 0; c < ch; ++c)
                        removed.setSample (c, at, block.getSample (c, i));
            }
    }

    const auto tag = inFile.getFileNameWithoutExtension() + "_f" + juce::String ((int) freq) + "_l"
                     + juce::String (lenMs, 1) + (strong ? "_strong" : "");
    const auto& eng = p->getEngine();
    const int logged = (int) std::min<uint32_t> (p->getRepairCount(), (uint32_t) declick::Engine::kLogSize);
    juce::String labels;
    for (int r = 0; r < logged; ++r)
        labels << juce::String ((double) eng.repairLog[r].start / fs, 4) << "\t"
               << juce::String ((double) eng.repairLog[r].end / fs, 4) << "\t" << r << "\n";
    outDir.getChildFile (tag + "_repairs.txt").replaceWithText (labels);

    if (writeWav)
    {
        auto outFile = outDir.getChildFile (tag + "_removed.wav");
        outFile.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream());
        juce::WavAudioFormat wav;
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (fs)
                                                           .withNumChannels (ch).withBitsPerSample (24)))
            writer->writeFromAudioSampleBuffer (removed, 0, total);
    }
    std::printf ("%g,%g,%u,%u%s\n", freq, lenMs, p->getRepairCount(), eng.rejected.load(),
                 logged < (int) p->getRepairCount() ? ",log-truncated" : "");
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;

    if (argc >= 3 && juce::String (argv[1]) == "--render")
        return render (juce::File (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2])),
                       argc >= 4 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]) : juce::File());
    if (argc >= 6 && juce::String (argv[1]) == "--sweep")
    {
        bool strong = false, wav = false;
        float sens = 50.0f;
        for (int i = 6; i < argc; ++i)
        {
            const juce::String a (argv[i]);
            if (a == "strong") strong = true;
            else if (a == "--wav") wav = true;
            else sens = a.getFloatValue();
        }
        const auto cwd = juce::File::getCurrentWorkingDirectory();
        return sweep (cwd.getChildFile (argv[2]), cwd.getChildFile (argv[3]),
                      juce::String (argv[4]).getFloatValue(), juce::String (argv[5]).getFloatValue(), sens, strong, wav);
    }

    testLatency();
    testCleanVoiceUntouched();
    testClicksInPausesRemoved();
    testLipSmacksRemoved();
    testLowerClicksRemoved();
    testLevelIndependent();
    testLongSoundsLeftAlone();
    testConsonantReleasesLeftAlone();
    testGentleVsStrong();
    testSensitivityZeroIsOff();
    testBypassBitExact();
    testListenIsTheDifference();
    testKnobMovesAreSilent();
    testLowBandUntouched();
    testStateRoundTrip();
    testDefaults();
    testVersionCompare();
    testEditorOpens();

    if (failures == 0)
        std::printf ("All VODeclicker tests passed (%d checks).\n", checks);
    else
        std::printf ("%d of %d checks failed.\n", failures, checks);
    return failures == 0 ? 0 : 1;
}
