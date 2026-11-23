#pragma once

// VODeclicker engine: finds mouth clicks and lip smacks in the high band of a
// voice and rebuilds those few milliseconds from the audio either side.
//
// Signal flow (all channels linked, so a click in either side of a stereo
// track is repaired in both, over the same span):
//
//   x ──┬─ Butterworth HP @ Frequency ─ detector (high band energy)
//       └─ Butterworth LP @ Frequency ─ low band level: "is this voiced?" (Gentle)
//
//   At repair time only, the high band around the span is re-derived with a
//   steep (8th-order) high-pass run forwards and backwards (zero phase), so that
//   low = x - high is an exact, phase-true complement: removing the high band
//   over the span removes the click's high band and nothing else.
//
//   output = x(delayed) - correction(delayed)
//
// The correction is zero everywhere except inside repaired spans, where it is
// (original high band - rebuilt high band). So outside repairs the output is
// the input delayed, bit for bit, and only high-passed content is ever
// replaced: the low band is never touched. Moving
// the Frequency knob only changes how future corrections are computed, so it
// can't click.
//
// Detection is level independent: the click has to stand out from the high
// band's own recent level (a ratio, never a dBFS threshold) and must be over
// within `Length`. Anything that stays loud for longer (an "s", a "t" release
// into a vowel, breath) is speech and is left alone.
//
// Repair: Burg autoregressive models fitted to the high band just before and
// just after the span, extrapolated forward and backward and cross-faded.
// The deterministic parts are cross-faded linearly; a little excitation noise,
// shaped by the same models at the residual level, is cross-faded at equal
// power so long spans keep the texture of the surrounding breath/fricative
// rather than going dead.

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>
#include <vector>

namespace declick
{
// ---- Fixed timing (milliseconds) --------------------------------------------
constexpr double kMaxLengthMs  = 8.0;   // top of the Length knob
constexpr double kPrePadMs     = 1.0;   // repaired before the detected onset (detector lag, filter ringing)
constexpr double kPostPadMs    = 1.0;   // repaired after the detected end
constexpr double kRightCtxMs   = 1.0;   // clean audio after the span used for the backward model
constexpr double kLeftCtxMs    = 4.0;   // clean audio before the span used for the forward model
constexpr double kLatencyMs    = 11.0;  // >= max length + pads + right context
constexpr double kSettleMs     = 6.0;   // how long after a click the high band must settle
constexpr int    kOrder        = 16;    // AR model order

static_assert (kLatencyMs >= kMaxLengthMs + kPrePadMs + kPostPadMs + kRightCtxMs,
               "latency must cover the longest span plus its right-hand context");

struct Settings
{
    float sensitivity = 0.5f;   // 0..1, 0 = off
    float frequencyHz = 3000.0f;
    float lengthMs    = 4.0f;
    bool  strong      = false;  // false = Gentle: leave clicks inside voiced speech alone
};

// Threshold (dB above the high band's recent level) for a given Sensitivity.
inline float thresholdDbFor (float sensitivity) { return 26.0f - 20.0f * sensitivity; }

// -----------------------------------------------------------------------------
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }

    // RBJ cookbook low/high pass. Keeps the filter state.
    void set (bool highPass, double fs, double fc, double q) noexcept
    {
        const double w = juce::MathConstants<double>::twoPi * fc / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        if (highPass) { b0 = (1.0 + cw) * 0.5; b1 = -(1.0 + cw); b2 = b0; }
        else          { b0 = (1.0 - cw) * 0.5; b1 =   1.0 - cw;  b2 = b0; }
        b0 /= a0; b1 /= a0; b2 /= a0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }
};

// 4th-order Butterworth as two biquads.
struct Butter4
{
    Biquad s1, s2;
    void set (bool highPass, double fs, double fc) noexcept
    {
        s1.set (highPass, fs, fc, 0.54119610);
        s2.set (highPass, fs, fc, 1.30656296);
    }
    inline double process (double x) noexcept { return s2.process (s1.process (x)); }
    void reset() noexcept { s1.reset(); s2.reset(); }
};

// Burg's method. Fits x[n] = -sum_{k=1..p} a[k] x[n-k] + e[n] to x[0..N).
// `a` gets p+1 coefficients (a[0] = 1). Returns the residual power.
// f and b are scratch of size >= N. Always yields a stable filter.
inline double burg (const double* x, int N, int p, double* a, double* f, double* b) noexcept
{
    double energy = 0.0;
    for (int n = 0; n < N; ++n) { f[n] = b[n] = x[n]; energy += x[n] * x[n]; }
    for (int i = 0; i <= p; ++i) a[i] = 0.0;
    a[0] = 1.0;
    double err = N > 0 ? energy / N : 0.0;

    for (int m = 0; m < p && m + 1 < N; ++m)
    {
        double num = 0.0, den = 0.0;
        for (int n = m + 1; n < N; ++n)
        {
            num += f[n] * b[n - 1];
            den += f[n] * f[n] + b[n - 1] * b[n - 1];
        }
        const double k = den > 1.0e-30 ? -2.0 * num / den : 0.0;

        for (int i = 0; i <= (m + 1) / 2; ++i)
        {
            const double ai = a[i], aj = a[m + 1 - i];
            a[i] = ai + k * aj;
            if (i != m + 1 - i)
                a[m + 1 - i] = aj + k * ai;
        }
        for (int n = N - 1; n >= m + 1; --n)
        {
            const double fn = f[n] + k * b[n - 1];
            const double bn = b[n - 1] + k * f[n];
            f[n] = fn;
            b[n] = bn;
        }
        err *= (1.0 - k * k);
    }
    return std::max (0.0, err);
}

// 8th-order Butterworth as four biquads (steep split for the repair band).
struct Butter8
{
    Biquad s[4];
    void set (bool highPass, double fs, double fc) noexcept
    {
        const double q[4] = { 0.50979558, 0.60134489, 0.89997622, 2.56291545 };
        for (int i = 0; i < 4; ++i) s[i].set (highPass, fs, fc, q[i]);
    }
    inline double process (double x) noexcept
    {
        return s[3].process (s[2].process (s[1].process (s[0].process (x))));
    }
    void reset() noexcept { for (auto& b : s) b.reset(); }
};

// -----------------------------------------------------------------------------
class Engine
{
public:
    void prepare (double sampleRate, int maxChannels)
    {
        fs = sampleRate;
        numChannels = juce::jlimit (1, 2, maxChannels);

        latency   = (int) std::ceil (kLatencyMs * 0.001 * fs);
        prePad    = std::max (1, (int) std::round (kPrePadMs * 0.001 * fs));
        postPad   = std::max (1, (int) std::round (kPostPadMs * 0.001 * fs));
        rightCtx  = std::max (2 * kOrder, (int) std::round (kRightCtxMs * 0.001 * fs));
        leftCtx   = std::max (4 * kOrder, (int) std::round (kLeftCtxMs * 0.001 * fs));
        maxLenLimit = (int) std::floor (kMaxLengthMs * 0.001 * fs);
        maxRegion = maxLenLimit + prePad + postPad + 4;

        int size = 1;
        edgePad = (int) std::ceil (4.0 * 0.001 * fs);   // settles the zero-phase filter at both ends
        while (size < latency + leftCtx + edgePad + maxRegion + rightCtx + 64)
            size <<= 1;
        mask = size - 1;

        for (int ch = 0; ch < 2; ++ch)
        {
            xBuf[ch].assign ((size_t) size, 0.0f);
            corrBuf[ch].assign ((size_t) size, 0.0f);
        }

        const int maxCtx = std::max (leftCtx, rightCtx);
        ctx.assign ((size_t) maxCtx, 0.0);
        burgF.assign ((size_t) maxCtx, 0.0);
        burgB.assign ((size_t) maxCtx, 0.0);
        fwd.assign ((size_t) (maxRegion + kOrder), 0.0);
        bwd.assign ((size_t) (maxRegion + kOrder), 0.0);
        fwdNoise.assign ((size_t) (maxRegion + kOrder), 0.0);
        bwdNoise.assign ((size_t) (maxRegion + kOrder), 0.0);
        rebuilt.assign ((size_t) maxRegion, 0.0);
        seg.assign ((size_t) (size + edgePad), 0.0);
        segHigh.assign ((size_t) (size + edgePad), 0.0);

        auto coeff = [this] (double ms) { return 1.0 - std::exp (-1.0 / (ms * 0.001 * fs)); };
        aFast   = coeff (0.2);
        aBgUp   = coeff (20.0);
        aBgDown = coeff (4.0);
        aMid    = coeff (2.0);
        holdLen = std::max (2, (int) std::round (0.5 * 0.001 * fs));
        settleLen = (int) std::round (kSettleMs * 0.001 * fs);
        aLow    = coeff (5.0);
        aLvlUp  = coeff (50.0);
        aLvlDn  = coeff (5000.0);
        floorRise = 0.6908 / fs;   // 3 dB (energy) per second, in nepers per sample

        lastFreq = -1.0f;
        setSettings (settings);
        reset();
    }

    void reset()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            std::fill (xBuf[ch].begin(), xBuf[ch].end(), 0.0f);
            std::fill (corrBuf[ch].begin(), corrBuf[ch].end(), 0.0f);
            split[ch].reset();
            detect[ch].reset();
        }
        now = 0;
        eFast = eMid = 0.0; logBg = std::log (kEnergyFloor); lowEnv = 0.0;
        speechLvl = kEnergyFloor; logFloor = 0.0;
        state = State::idle;
        pending = false;
        rng = 0x12345678u;
    }

    void setSettings (const Settings& s)
    {
        settings = s;
        if (fs > 0.0 && std::abs (s.frequencyHz - lastFreq) > 0.01f)
        {
            const double fc = juce::jlimit (200.0, fs * 0.45, (double) s.frequencyHz);
            for (int ch = 0; ch < 2; ++ch)
            {
                split[ch].set (false, fs, fc);
                detect[ch].set (true, fs, fc);
            }
            zeroPhase.set (true, fs, fc);
            lastFreq = s.frequencyHz;
        }
        enabled = s.sensitivity > 0.0f;
        threshold = std::pow (10.0, thresholdDbFor (s.sensitivity) / 10.0);
        logThreshold = std::log (threshold);
        maxLen = juce::jlimit (1, maxLenLimit, (int) std::round (s.lengthMs * 0.001 * fs));
    }

    int getLatencySamples() const noexcept { return latency; }

    // Pushes one frame of `numCh` input samples; writes the input and the
    // correction, both delayed by the latency. Output = dryOut - corrOut.
    inline void processFrame (const float* in, int numCh, float* dryOut, float* corrOut) noexcept
    {
        const int idx = (int) (now & mask);
        double dSum = 0.0, lowSum = 0.0;

        for (int ch = 0; ch < numCh; ++ch)
        {
            const double x = in[ch];
            const double low = split[ch].process (x);
            const double d = detect[ch].process (x);
            xBuf[ch][(size_t) idx] = in[ch];
            corrBuf[ch][(size_t) idx] = 0.0f;
            dSum += d * d;
            lowSum += low * low;
        }
        dSum /= numCh;
        lowSum /= numCh;

        eFast += (dSum - eFast) * aFast;
        eMid  += (dSum - eMid) * aMid;
        const double lowBefore = lowEnv;
        lowEnv += (lowSum - lowEnv) * aLow;
        trackSpeechLevel();
        runDetector (numCh, lowBefore);

        if (pending)
        {
            if (now >= settleFrom && now <= settleTo)
            {
                settleSum += std::log (eFast + kEnergyFloor) - pendingBg;
                ++settleCount;
            }
            if (now >= pendingRepairAt)
                finishPending (numCh, pendingB + rightCtx);
        }

        const int outIdx = (int) ((now - latency) & mask);
        for (int ch = 0; ch < numCh; ++ch)
        {
            dryOut[ch] = xBuf[ch][(size_t) outIdx];
            corrOut[ch] = corrBuf[ch][(size_t) outIdx];
        }
        ++now;
    }

    // Number of spans repaired since construction (read from the UI thread).
    std::atomic<uint32_t> repairs { 0 };
    // Click-like events left alone because they didn't settle (consonant releases).
    std::atomic<uint32_t> rejected { 0 };

    // The first kLogSize repaired spans, in input samples (for tests and --render).
    static constexpr int kLogSize = 4096;
    struct Span { int64_t start, end; };
    Span repairLog[kLogSize] {};

private:
    static constexpr double kEnergyFloor = 1.0e-13;   // about -130 dB: digital silence never triggers

    enum class State { idle, candidate, longSound };

    void trackSpeechLevel() noexcept
    {
        // Peak-ish follower of the low band (the voice's level), and a slow
        // minimum follower (the room's level). Both relative, so Gentle's
        // "is this voiced speech?" decision works at any recording level.
        if (lowEnv > speechLvl) speechLvl += (lowEnv - speechLvl) * aLvlUp;
        else                    speechLvl += (lowEnv - speechLvl) * aLvlDn;
        speechLvl = std::max (speechLvl, kEnergyFloor);

        // Room level: drops straight to any new minimum, then creeps up at
        // 3 dB a second (log domain, so a long sentence can't drag it up to
        // the voice's level).
        const double logLow = std::log (lowEnv + kEnergyFloor);
        if (logLow < logFloor) logFloor = logLow;
        else                   logFloor += floorRise;
    }

    bool isVoiced (double low) const noexcept
    {
        constexpr double aboveRoom = 31.6;     // +15 dB over the room
        constexpr double belowVoice = 0.01;    // within 20 dB of the voice's level
        return std::log (low + kEnergyFloor) > logFloor + std::log (aboveRoom) && low > speechLvl * belowVoice;
    }

    void updateBackground() noexcept
    {
        // Log-domain level of the high band: rises slowly (a click barely moves
        // it), falls fast (so a click straight after a word is still seen).
        const double target = std::log (eMid + kEnergyFloor);
        logBg += (target - logBg) * (target > logBg ? aBgUp : aBgDown);
    }

    void runDetector (int numCh, double lowBefore) noexcept
    {
        const bool above = std::log (eFast + kEnergyFloor) > logBg + logThreshold;

        switch (state)
        {
            case State::idle:
                if (enabled && above)
                {
                    if (pending)   // a new event would land in the pending repair's right context: decide now
                        finishPending (numCh, now - prePad - 1);
                    state = State::candidate;
                    onset = now;
                    onsetLow = lowBefore;
                    belowCount = 0;
                    break;   // background stays frozen while we look at the event
                }
                updateBackground();
                break;

            case State::candidate:
            {
                // Over once it has stayed 3 dB under the threshold for holdLen samples.
                const bool below = std::log (eFast + kEnergyFloor) < logBg + logThreshold - 0.693;
                if (below)
                {
                    if (belowCount++ == 0)
                        belowStart = now;
                }
                else
                {
                    belowCount = 0;
                }

                if (belowCount > 0 && belowStart - onset > maxLen)
                {
                    state = State::longSound;
                }
                else if (belowCount >= holdLen)
                {
                    state = State::idle;
                    if (settings.strong || ! isVoiced (onsetLow))
                    {
                        pendingA = std::max (onset - prePad, now - latency + 1);
                        pendingB = belowStart + postPad;
                        pendingBg = logBg;   // still frozen at its pre-click value
                        // Watch the high band settle after the click for as long
                        // as the latency allows (up to settleLen), then decide.
                        settleFrom = belowStart + holdLen;
                        settleTo = std::min (belowStart + settleLen, pendingA + latency - 1);
                        settleSum = 0.0;
                        settleCount = 0;
                        pendingRepairAt = std::max ({ now, pendingB + rightCtx, settleTo });
                        pending = true;
                    }
                }
                else if (belowCount == 0 && now - onset > maxLen)
                {
                    state = State::longSound;   // speech, not a click
                }
                break;
            }

            case State::longSound:
                // Stay here until the background has caught up with the sound
                // (or it has ended), so its own flutter isn't taken for clicks.
                updateBackground();
                if (std::log (eMid + kEnergyFloor) < logBg + 0.69)
                    state = State::idle;
                break;
        }
    }

    // A click has to settle: afterwards the high band must fall back near its
    // level before the click. A consonant release ("t", "k") runs straight
    // into aspiration or the vowel instead, so it fails this and is left alone.
    void finishPending (int numCh, int64_t rightEnd) noexcept
    {
        pending = false;
        const bool settled = settleCount == 0 || settleSum / settleCount < 0.5 * logThreshold;
        if (settled)
            repair (numCh, pendingA, pendingB, rightEnd);
        else
            rejected.fetch_add (1, std::memory_order_relaxed);
    }

    inline float rand01() noexcept   // uniform in [-1, 1)
    {
        rng = rng * 1664525u + 1013904223u;
        return (float) ((rng >> 8) * (1.0 / 8388608.0) - 1.0);
    }

    // Rebuilds the high band over absolute samples [a, b] using clean audio
    // before a and in (b, rightEnd]. Writes the correction for every channel.
    void repair (int numCh, int64_t a, int64_t b, int64_t rightEnd) noexcept
    {
        a = std::max (a, now - latency + 1);   // never touch what's already been output
        b = std::min (b, now);                 // nor what hasn't arrived yet
        const int m = (int) (b - a + 1);
        if (m <= 0 || m > maxRegion)
            return;

        const int nRight = (int) juce::jlimit<int64_t> (0, rightCtx, rightEnd - b);
        const bool useRight = nRight >= 2 * kOrder;
        double aF[kOrder + 1], aB[kOrder + 1];

        // Segment the zero-phase high-pass runs over: from the left context
        // (plus settling room) up to the newest sample, then a mirrored tail.
        const int64_t s0 = a - leftCtx - edgePad;
        const int segLen = (int) (now - s0 + 1);
        const int total = segLen + edgePad;

        for (int ch = 0; ch < numCh; ++ch)
        {
            // y = what will be output so far (input minus earlier corrections).
            for (int i = 0; i < segLen; ++i)
            {
                const auto k = (size_t) ((s0 + i) & mask);
                seg[(size_t) i] = (double) xBuf[ch][k] - (double) corrBuf[ch][k];
            }
            const double last = seg[(size_t) (segLen - 1)];
            for (int i = 0; i < edgePad; ++i)   // odd reflection keeps the end continuous
                seg[(size_t) (segLen + i)] = 2.0 * last - seg[(size_t) std::max (0, segLen - 2 - i)];

            Butter8 f = zeroPhase;
            f.reset();
            for (int i = 0; i < total; ++i) segHigh[(size_t) i] = f.process (seg[(size_t) i]);
            f.reset();
            for (int i = total - 1; i >= 0; --i) segHigh[(size_t) i] = f.process (segHigh[(size_t) i]);

            auto high = [this, s0] (int64_t t) { return segHigh[(size_t) (t - s0)]; };

            // ---- forward model from the left -------------------------------
            for (int i = 0; i < leftCtx; ++i)
                ctx[(size_t) i] = high (a - leftCtx + i);
            double ctxEnergy = 0.0;
            for (int i = 0; i < leftCtx; ++i) ctxEnergy += ctx[(size_t) i] * ctx[(size_t) i];
            int ctxCount = leftCtx;

            const double errF = burg (ctx.data(), leftCtx, kOrder, aF, burgF.data(), burgB.data());
            extrapolate (ctx.data() + leftCtx - kOrder, aF, std::sqrt (errF), m, fwd.data(), fwdNoise.data());

            // ---- backward model from the right (time reversed) -------------
            if (useRight)
            {
                for (int i = 0; i < nRight; ++i)
                    ctx[(size_t) i] = high (b + nRight - i);
                for (int i = 0; i < nRight; ++i) ctxEnergy += ctx[(size_t) i] * ctx[(size_t) i];
                ctxCount += nRight;

                const double errB = burg (ctx.data(), nRight, kOrder, aB, burgF.data(), burgB.data());
                extrapolate (ctx.data() + nRight - kOrder, aB, std::sqrt (errB), m, bwd.data(), bwdNoise.data());
            }

            // ---- cross-fade -----------------------------------------------
            double outEnergy = 0.0;
            for (int i = 0; i < m; ++i)
            {
                double v;
                if (useRight)
                {
                    const double w = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * (i + 1) / (m + 1)));
                    const int j = m - 1 - i;   // backward arrays run from b down to a
                    v = w * fwd[(size_t) (kOrder + i)] + (1.0 - w) * bwd[(size_t) (kOrder + j)]
                      + std::sqrt (w) * fwdNoise[(size_t) (kOrder + i)]
                      + std::sqrt (1.0 - w) * bwdNoise[(size_t) (kOrder + j)];
                }
                else
                {
                    v = fwd[(size_t) (kOrder + i)] + fwdNoise[(size_t) (kOrder + i)];
                }
                rebuilt[(size_t) i] = v;
                outEnergy += v * v;
            }

            // Safety: the rebuild may never be louder than the audio around it.
            const double ctxRms = std::sqrt (ctxEnergy / std::max (1, ctxCount));
            const double outRms = std::sqrt (outEnergy / m);
            const double scale = outRms > 1.5 * ctxRms && outRms > 0.0 ? 1.5 * ctxRms / outRms : 1.0;

            // Swap the high band for the rebuild, easing in over the outer
            // half of the pre-pad and out over the outer half of the post-pad.
            const int fadeIn = std::max (1, prePad / 2), fadeOut = std::max (1, postPad / 2);
            for (int i = 0; i < m; ++i)
            {
                double fade = 1.0;
                if (i < fadeIn)             fade = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (i + 0.5) / fadeIn);
                else if (i >= m - fadeOut)  fade = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (m - i - 0.5) / fadeOut);

                const auto k = (size_t) ((a + i) & mask);
                const double removed = fade * (high (a + i) - rebuilt[(size_t) i] * scale);
                corrBuf[ch][k] = (float) ((double) corrBuf[ch][k] + removed);
            }
        }
        const auto count = repairs.fetch_add (1, std::memory_order_relaxed);
        if (count < (uint32_t) kLogSize)
            repairLog[count] = { a, b };
    }

    // Continues the last kOrder samples of `history` for m samples with the AR
    // model `a`. det gets the noise-free prediction, noise the model driven by
    // white noise at the residual level (both with kOrder leading samples).
    void extrapolate (const double* history, const double* a, double sigma, int m,
                      double* det, double* noise) noexcept
    {
        for (int i = 0; i < kOrder; ++i) { det[i] = history[i]; noise[i] = 0.0; }
        const double g = sigma * 1.7320508;   // uniform [-1,1) has variance 1/3
        for (int i = kOrder; i < kOrder + m; ++i)
        {
            double pd = 0.0, pn = 0.0;
            for (int k = 1; k <= kOrder; ++k)
            {
                pd -= a[k] * det[i - k];
                pn -= a[k] * noise[i - k];
            }
            det[i] = pd;
            noise[i] = pn + g * rand01();
        }
    }

    double fs = 0.0;
    int numChannels = 2;
    int latency = 0, prePad = 1, postPad = 1, rightCtx = 32, leftCtx = 64;
    int maxLenLimit = 1, maxLen = 1, maxRegion = 8;
    int mask = 0;
    int64_t now = 0;

    std::vector<float> xBuf[2], corrBuf[2];
    std::vector<double> seg, segHigh, ctx, burgF, burgB, fwd, bwd, fwdNoise, bwdNoise, rebuilt;
    Butter4 split[2], detect[2];
    Butter8 zeroPhase;
    int edgePad = 1;

    Settings settings;
    float lastFreq = -1.0f;
    bool enabled = true;
    double threshold = 40.0, logThreshold = 3.7;

    double aFast = 0, aMid = 0, aBgUp = 0, aBgDown = 0, aLow = 0, aLvlUp = 0, aLvlDn = 0, floorRise = 0;
    double eFast = 0, eMid = 0, logBg = 0, lowEnv = 0, speechLvl = kEnergyFloor, logFloor = 0.0;
    int holdLen = 2, belowCount = 0;
    int64_t belowStart = 0;

    State state = State::idle;
    int64_t onset = 0;
    double onsetLow = 0;

    bool pending = false;
    int64_t pendingA = 0, pendingB = 0, pendingRepairAt = 0;
    int64_t settleFrom = 0, settleTo = 0;
    double pendingBg = 0.0, settleSum = 0.0;
    int settleCount = 0, settleLen = 1;
    uint32_t rng = 0x12345678u;
};
} // namespace declick
