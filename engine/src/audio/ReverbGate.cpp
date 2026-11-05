#include "audio/ReverbGate.h"

#include "audio/Audio.h"
#include "audio/Reverb.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace ke::audio {

namespace {

constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// Tolerances. The RT60 tolerance is the engineering bound for a parametric
// algorithmic reverb measured on a band-limited impulse response; ISO 3382's
// just-noticeable difference for reverberation time is 5 percent, so 15 percent
// is deliberately looser and is reported next to every measurement.
constexpr double kRtTolerance = 0.15;
constexpr double kHfRatioTolerance = 0.15;  // absolute, on the HF/LF decay-time ratio
constexpr double kOnsetToleranceMs = 0.5;
constexpr double kDecorrelationLimit = 0.30;
constexpr double kCrossfadeJumpRatio = 1.5;
constexpr double kDenormalTimeRatio = 1.5;
constexpr double kCpuLimitSingle = 0.15;  // fraction of real time, one reverb
constexpr double kCpuLimitFading = 0.30;  // fraction of real time, two reverbs

std::string format(const char* pattern, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    return buffer;
}

struct Gate {
    ReverbGateResult result;

    void line(const std::string& text) { result.lines.push_back(text); }

    void check(bool condition, const std::string& label)
    {
        result.lines.push_back(std::string(condition ? "PASS " : "FAIL ") + label);
        result.passed = result.passed && condition;
    }
};

std::shared_ptr<const SoundData> impulseSound()
{
    auto sound = std::make_shared<SoundData>();
    sound->name = "gate_impulse";
    sound->sampleRate = kRate;
    sound->channels = 1;
    sound->samples = {0.5f};
    return sound;
}

std::shared_ptr<const SoundData> sineSound(double frequency, float amplitude)
{
    auto sound = std::make_shared<SoundData>();
    sound->name = "gate_sine";
    sound->sampleRate = kRate;
    sound->channels = 1;
    // A whole number of cycles so the loop has no seam.
    const int cycles = std::max(1, static_cast<int>(std::lround(frequency)));
    sound->samples.resize(static_cast<std::size_t>(kRate));
    for (int i = 0; i < kRate; ++i) {
        sound->samples[static_cast<std::size_t>(i)] = amplitude
            * static_cast<float>(std::sin(2.0 * kPi * cycles * i / kRate));
    }
    return sound;
}

struct Capture {
    std::vector<float> left;
    std::vector<float> right;
};

Mixer& prepare(Mixer& mixer)
{
    mixer.setSampleRate(kRate);
    mixer.setListener({0.0f, 0.0f, 0.0f}, 0.0f);
    mixer.setBusVolume(Bus::Effects, 1.0f);
    return mixer;
}

void pull(Mixer& mixer, Capture& out, int frames, int block)
{
    std::vector<float> buffer(static_cast<std::size_t>(block) * 2u);
    int done = 0;
    while (done < frames) {
        const int count = std::min(block, frames - done);
        mixer.mix(buffer.data(), count);
        for (int i = 0; i < count; ++i) {
            out.left.push_back(buffer[static_cast<std::size_t>(i) * 2u]);
            out.right.push_back(buffer[static_cast<std::size_t>(i) * 2u + 1u]);
        }
        done += count;
    }
}

// Wet impulse response of one environment (the dry click at frame 0 is dropped).
Capture impulseResponse(int environment, double seconds)
{
    Mixer mixer;
    prepare(mixer);
    mixer.setReverbEnvironment(environment, false);
    mixer.play(impulseSound(), Bus::Effects, 1.0f, false, true, {0.0f, 0.0f, 0.0f}, 1.0f, 10.0f);
    Capture capture;
    pull(mixer, capture, static_cast<int>(seconds * kRate), 1024);
    capture.left.erase(capture.left.begin());
    capture.right.erase(capture.right.begin());
    return capture;
}

std::vector<double> toDouble(const std::vector<float>& values)
{
    return std::vector<double>(values.begin(), values.end());
}

std::vector<double> onePole(const std::vector<double>& x, double cutoff)
{
    const double a = std::exp(-2.0 * kPi * cutoff / kRate);
    std::vector<double> y(x.size());
    double state = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        state = (1.0 - a) * x[i] + a * state;
        y[i] = state;
    }
    return y;
}

std::vector<double> lowBand(const std::vector<double>& x)
{
    // Two cascaded one-poles at 800 Hz: the "low and middle frequency" band in
    // which EAX defines the decay time.
    return onePole(onePole(x, 800.0), 800.0);
}

std::vector<double> biquadHighPass(const std::vector<double>& x, double cutoff, double q)
{
    // RBJ cookbook high-pass section.
    const double w0 = 2.0 * kPi * cutoff / kRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double c = std::cos(w0);
    const double b0 = (1.0 + c) / 2.0, b1 = -(1.0 + c), b2 = (1.0 + c) / 2.0;
    const double a0 = 1.0 + alpha, a1 = -2.0 * c, a2 = 1.0 - alpha;
    std::vector<double> y(x.size());
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double out = (b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1;
        x1 = x[i];
        y2 = y1;
        y1 = out;
        y[i] = out;
    }
    return y;
}

// 4th-order Butterworth high-pass: steep enough that the strong low-frequency
// tail does not leak into the band and mask its (faster) decay.
std::vector<double> highBand(const std::vector<double>& x, double cutoff)
{
    return biquadHighPass(biquadHighPass(x, cutoff, 0.5411961), cutoff, 1.3065630);
}

// Reverberation time from the Schroeder backward-integrated energy decay curve:
// least-squares line through the -5 dB to -35 dB span, extrapolated to -60 dB.
double reverbTime(const std::vector<double>& x)
{
    std::vector<double> edc(x.size());
    double accumulator = 0.0;
    for (std::size_t i = x.size(); i-- > 0;) {
        accumulator += x[i] * x[i];
        edc[i] = accumulator;
    }
    if (edc.empty() || edc[0] <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::size_t first = 0;
    std::size_t last = 0;
    for (std::size_t i = 0; i < edc.size(); ++i) {
        const double db = 10.0 * std::log10(std::max(edc[i], 1e-300) / edc[0]);
        if (first == 0 && db <= -5.0) {
            first = i;
        }
        if (db <= -35.0) {
            last = i;
            break;
        }
    }
    if (first == 0 || last <= first + 8) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double sumT = 0.0, sumD = 0.0, sumTT = 0.0, sumTD = 0.0;
    double count = 0.0;
    for (std::size_t i = first; i <= last; i += 4) {
        const double t = static_cast<double>(i) / kRate;
        const double d = 10.0 * std::log10(std::max(edc[i], 1e-300) / edc[0]);
        sumT += t;
        sumD += d;
        sumTT += t * t;
        sumTD += t * d;
        count += 1.0;
    }
    const double slope = (count * sumTD - sumT * sumD) / (count * sumTT - sumT * sumT);
    return -60.0 / slope;
}

double iacc(const std::vector<float>& left, const std::vector<float>& right,
            std::size_t begin, std::size_t end, double& zeroLag)
{
    end = std::min({end, left.size(), right.size()});
    double energyL = 0.0, energyR = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        energyL += static_cast<double>(left[i]) * left[i];
        energyR += static_cast<double>(right[i]) * right[i];
    }
    const double norm = std::sqrt(energyL * energyR);
    if (norm <= 0.0) {
        return 1.0;
    }
    double best = 0.0;
    zeroLag = 0.0;
    for (int lag = -48; lag <= 48; ++lag) {
        double sum = 0.0;
        for (std::size_t i = begin + 48; i + 48 < end; ++i) {
            sum += static_cast<double>(left[i]) * right[static_cast<std::size_t>(
                       static_cast<long long>(i) + lag)];
        }
        const double rho = sum / norm;
        if (lag == 0) {
            zeroLag = rho;
        }
        best = std::max(best, std::fabs(rho));
    }
    return best;
}

double maxJump(const std::vector<double>& x, std::size_t begin, std::size_t end)
{
    double best = 0.0;
    end = std::min(end, x.size());
    for (std::size_t i = std::max<std::size_t>(begin, 1); i < end; ++i) {
        best = std::max(best, std::fabs(x[i] - x[i - 1]));
    }
    return best;
}

// Wet-only signal of a crossfade run: output minus the identical run with the
// reverb off, so the dry part cancels exactly.
std::vector<double> crossfadeWet(bool crossfade, int fromEnvironment, int toEnvironment,
                                 int switchFrame, int totalFrames, double fadeSeconds)
{
    const auto run = [&](bool reverb) {
        Mixer mixer;
        prepare(mixer);
        mixer.setReverbCrossfadeSeconds(static_cast<float>(fadeSeconds));
        mixer.setReverbEnvironment(reverb ? fromEnvironment : -1, false);
        mixer.play(sineSound(100.0, 0.8f), Bus::Effects, 1.0f, true, true,
                   {0.0f, 0.0f, 0.0f}, 1.0f, 10.0f);
        Capture capture;
        pull(mixer, capture, switchFrame, 500);
        if (reverb) {
            mixer.setReverbEnvironment(toEnvironment, crossfade);
        }
        pull(mixer, capture, totalFrames - switchFrame, 500);
        return capture.left;
    };
    const std::vector<float> wet = run(true);
    const std::vector<float> dry = run(false);
    std::vector<double> difference(wet.size());
    for (std::size_t i = 0; i < wet.size(); ++i) {
        difference[i] = static_cast<double>(wet[i]) - static_cast<double>(dry[i]);
    }
    return difference;
}

double nowMs()
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

ReverbGateResult runReverbGate()
{
    Gate gate;
    gate.line("reverb gate: PARAMETER-FAITHFUL, NOT BIT-EXACT MILES/EAX; FDN + early reflections");

    // ---- 1. RT60 (Schroeder integral, LF band) against DecayTime, plus ER onset,
    //         HF damping direction and stereo decorrelation per preset.
    struct Row {
        const char* label;
        double measuredRt = 0.0;
        double measuredHfRatio = 0.0;
    };
    const char* rtPresets[] = {"ROOM", "GENERIC", "STONEROOM", "CAVE", "AUDITORIUM", "ARENA", "HANGAR"};
    std::vector<Row> rows;
    int rtPassed = 0;
    for (const char* label : rtPresets) {
        const int id = eaxEnvironmentId(label);
        const EaxEnvironment& env = eaxEnvironment(id);
        const double seconds = std::max(1.0, 2.2 * env.decayTime);
        const Capture ir = impulseResponse(id, seconds);
        const std::vector<double> mono = toDouble(ir.left);

        const double rtBroadband = reverbTime(mono);
        const double rtLow = reverbTime(lowBand(mono));
        const double rtHigh = reverbTime(highBand(mono, env.hfReference));
        const double rtError = (rtLow - env.decayTime) / env.decayTime;
        const bool rtOk = std::fabs(rtError) <= kRtTolerance;
        rtPassed += rtOk ? 1 : 0;
        gate.line(format("  %-10s DecayTime %.2f s  RT60(LF band) %.2f s (%+.1f%%)  broadband %.2f s  "
                         "HF band %.2f s  HF/LF %.2f (DecayHFRatio %.2f)",
                         label, env.decayTime, rtLow, rtError * 100.0, rtBroadband, rtHigh,
                         rtHigh / rtLow, env.decayHfRatio));
        gate.check(rtOk, format("rt60 %s within +-%.0f%% of DecayTime", label, kRtTolerance * 100.0));
        rows.push_back({label, rtLow, rtHigh / rtLow});

        // Early-reflection onset: first wet sample above 20 percent of the peak
        // seen up to the late-field onset.
        const std::size_t peakEnd = static_cast<std::size_t>(
            (env.reflectionsDelay + std::min(env.reverbDelay, 0.004f)) * kRate);
        double peak = 0.0;
        for (std::size_t i = 0; i < std::min(peakEnd, ir.left.size()); ++i) {
            peak = std::max(peak, std::fabs(static_cast<double>(ir.left[i])));
        }
        std::size_t onset = 0;
        while (onset < ir.left.size() && std::fabs(static_cast<double>(ir.left[onset])) < 0.2 * peak) {
            ++onset;
        }
        // Frame 0 (the dry click) was dropped, so wet index 0 is 1 sample after it.
        const double onsetMs = (static_cast<double>(onset) + 1.0) * 1000.0 / kRate;
        const double expectedMs = env.reflectionsDelay * 1000.0;
        gate.line(format("  %-10s ER onset %.2f ms vs ReflectionsDelay %.2f ms", label, onsetMs, expectedMs));
        gate.check(std::fabs(onsetMs - expectedMs) <= kOnsetToleranceMs,
                   format("er onset %s within %.1f ms of ReflectionsDelay", label, kOnsetToleranceMs));

        // Stereo decorrelation over the first part of the field after the late onset.
        const std::size_t begin = static_cast<std::size_t>(
            (env.reflectionsDelay + env.reverbDelay) * kRate);
        const std::size_t end = begin + static_cast<std::size_t>(std::min(env.decayTime, 2.0f) * kRate);
        double zeroLag = 0.0;
        const double rho = iacc(ir.left, ir.right, begin, end, zeroLag);
        gate.line(format("  %-10s L/R correlation: lag-0 %+.3f  max |rho| over +-1 ms %.3f", label, zeroLag, rho));
        gate.check(rho <= kDecorrelationLimit,
                   format("decorrelation %s max|rho| <= %.2f", label, kDecorrelationLimit));
    }
    gate.check(rtPassed >= 3 && rows.size() >= 3, "rt60 matched for at least 3 presets with distinct decays");

    // ---- 2. HF damping: measured HF/LF decay ratio must fall below 1 for presets
    //         with DecayHFRatio < 1 and keep the parameter ordering.
    bool directionOk = true;
    bool orderingOk = true;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const EaxEnvironment& a = eaxEnvironment(eaxEnvironmentId(rows[i].label));
        if (a.decayHfRatio < 0.9f && !(rows[i].measuredHfRatio < 0.95)) {
            directionOk = false;
        }
        for (std::size_t j = 0; j < rows.size(); ++j) {
            const EaxEnvironment& b = eaxEnvironment(eaxEnvironmentId(rows[j].label));
            if (a.decayHfRatio < 0.9f && b.decayHfRatio < 0.9f
                && a.decayHfRatio > 1.2f * b.decayHfRatio
                && !(rows[i].measuredHfRatio > rows[j].measuredHfRatio)) {
                orderingOk = false;
            }
        }
    }
    bool magnitudeOk = true;
    double worstHfError = 0.0;
    for (const Row& row : rows) {
        const EaxEnvironment& env = eaxEnvironment(eaxEnvironmentId(row.label));
        if (env.decayHfRatio < 0.9f) {
            const double error = std::fabs(row.measuredHfRatio - env.decayHfRatio);
            worstHfError = std::max(worstHfError, error);
            magnitudeOk = magnitudeOk && error <= kHfRatioTolerance;
        }
    }
    gate.line(format("  hf damping: worst |measured HF/LF - DecayHFRatio| = %.3f over presets with ratio < 0.9 "
                     "(band above %.0f Hz, 4th-order high-pass)", worstHfError, 5000.0));
    gate.check(magnitudeOk, format("hf damping magnitude within %.2f of DecayHFRatio", kHfRatioTolerance));
    gate.check(directionOk, "hf damping direction: HF RT60 < LF RT60 when DecayHFRatio < 1");
    gate.check(orderingOk, "hf damping ordering follows DecayHFRatio across presets");
    gate.line("  note: CAVE/PARKINGLOT use DecayHFRatio > 1; a low-pass cannot lengthen HF decay, so "
              "the realised HF/LF is ~1 (documented non-parity)");

    // ---- 3. Denormals: the longest preset, then 140 s of silence.
    {
        Mixer mixer;
        prepare(mixer);
        mixer.setReverbEnvironment(eaxEnvironmentId("HANGAR"), false);
        mixer.play(impulseSound(), Bus::Effects, 1.0f, false, true, {0.0f, 0.0f, 0.0f}, 1.0f, 10.0f);
        std::vector<float> buffer(1024 * 2);
        const int blocks = 140 * kRate / 1024;
        std::vector<double> times(static_cast<std::size_t>(blocks));
        float lastPeak = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            const double t0 = nowMs();
            mixer.mix(buffer.data(), 1024);
            times[static_cast<std::size_t>(b)] = nowMs() - t0;
        }
        for (const float value : buffer) {
            lastPeak = std::max(lastPeak, std::fabs(value));
        }
        std::vector<double> sorted = times;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        std::vector<double> window;
        for (int b = 100 * kRate / 1024; b < blocks; ++b) {  // 100 s .. 140 s: denormal-prone
            window.push_back(times[static_cast<std::size_t>(b)]);
        }
        std::sort(window.begin(), window.end());
        // Medians, not means: a denormal slowdown hits every block of the window,
        // while one preempted block would only skew a mean.
        const double windowMean = window.empty() ? 0.0 : window[window.size() / 2];
        gate.line(format("  denormal run: 140 s HANGAR tail, median %.4f ms/block, median over 100-140 s %.4f ms/block "
                         "(ratio %.2f), final block peak %.3g, denormal values in state %zu",
                         median, windowMean, windowMean / std::max(median, 1e-9), lastPeak,
                         mixer.reverbDenormalCount()));
        gate.check(mixer.reverbDenormalCount() == 0, "no denormal values left in reverb state after 140 s decay");
        gate.check(windowMean <= kDenormalTimeRatio * std::max(median, 1e-9),
                   format("no denormal CPU spike (100-140 s median <= %.1fx overall median)", kDenormalTimeRatio));
    }

    // ---- 3b. Sensitivity: with flush-to-zero off the same decay does leave denormals
    //          in the state (and costs CPU), so the protected run above proves something.
    {
        struct Probe {
            std::size_t maxDenormals = 0;
            double slowMs = 0.0;
            double fastMs = 0.0;
        };
        const auto probe = [&](bool protect) {
            Reverb::setDenormalProtection(protect);
            Mixer mixer;
            prepare(mixer);
            mixer.setReverbEnvironment(eaxEnvironmentId("PADDEDCELL"), false);
            mixer.play(impulseSound(), Bus::Effects, 1.0f, false, true, {0.0f, 0.0f, 0.0f}, 1.0f, 10.0f);
            std::vector<float> buffer(1024 * 2);
            Probe result;
            double beforeSum = 0.0, duringSum = 0.0;
            int beforeCount = 0, duringCount = 0;
            for (int b = 0; b < 300; ++b) {  // about 6.4 s; amplitude crosses 1e-38 near 2.2 s
                const double t0 = nowMs();
                mixer.mix(buffer.data(), 1024);
                const double elapsed = nowMs() - t0;
                const std::size_t count = mixer.reverbDenormalCount();
                result.maxDenormals = std::max(result.maxDenormals, count);
                if (count > 0) {
                    duringSum += elapsed;
                    ++duringCount;
                } else if (b < 40) {
                    beforeSum += elapsed;
                    ++beforeCount;
                }
            }
            result.fastMs = beforeSum / std::max(beforeCount, 1);
            result.slowMs = duringSum / std::max(duringCount, 1);
            return result;
        };
        const Probe unprotected = probe(false);
        const Probe protectedRun = probe(true);
        Reverb::setDenormalProtection(true);
        gate.line(format("  sensitivity (PADDEDCELL): without flush-to-zero max %zu denormals in state, "
                         "%.4f ms/block while present vs %.4f ms/block early; with flush-to-zero max %zu",
                         unprotected.maxDenormals, unprotected.slowMs, unprotected.fastMs,
                         protectedRun.maxDenormals));
        gate.check(unprotected.maxDenormals > 0 && protectedRun.maxDenormals == 0,
                   "denormals appear without flush-to-zero and never with it");
    }

    // ---- 4. Determinism and block-size independence.
    {
        const auto render = [&](int block) {
            Mixer mixer;
            prepare(mixer);
            mixer.setReverbCrossfadeSeconds(0.15f);
            mixer.setReverbEnvironment(eaxEnvironmentId("STONEROOM"), false);
            mixer.play(impulseSound(), Bus::Effects, 1.0f, false, true, {0.0f, 0.0f, 0.0f}, 1.0f, 10.0f);
            mixer.play(sineSound(440.0, 0.3f), Bus::Effects, 1.0f, true, true, {3.0f, -2.0f, 0.0f}, 1.0f, 20.0f);
            Capture capture;
            pull(mixer, capture, 24000, block);
            mixer.setReverbEnvironment(eaxEnvironmentId("ARENA"), true);
            pull(mixer, capture, 48000, block);
            return capture;
        };
        const Capture a = render(1000);
        const Capture b = render(1000);
        const Capture c = render(500);
        const bool same = a.left.size() == b.left.size()
            && std::memcmp(a.left.data(), b.left.data(), a.left.size() * sizeof(float)) == 0
            && std::memcmp(a.right.data(), b.right.data(), a.right.size() * sizeof(float)) == 0;
        const bool blockIndependent = a.left.size() == c.left.size()
            && std::memcmp(a.left.data(), c.left.data(), a.left.size() * sizeof(float)) == 0
            && std::memcmp(a.right.data(), c.right.data(), a.right.size() * sizeof(float)) == 0;
        gate.check(same, "deterministic: two runs are bit-identical (72000 frames, incl. a crossfade)");
        gate.check(blockIndependent, "deterministic: 1000-frame and 500-frame blocks are bit-identical");
    }

    // ---- 5. Room-change crossfade: no discontinuity in the wet signal.
    {
        const int switchFrame = 24000;
        const int total = 24000 + 24000;
        const double fade = 0.15;
        const int fadeFrames = static_cast<int>(fade * kRate);
        const std::vector<double> soft = crossfadeWet(true, eaxEnvironmentId("GENERIC"),
                                                      eaxEnvironmentId("HANGAR"), switchFrame, total, fade);
        const std::vector<double> hard = crossfadeWet(false, eaxEnvironmentId("GENERIC"),
                                                      eaxEnvironmentId("HANGAR"), switchFrame, total, fade);
        const std::size_t sw = static_cast<std::size_t>(switchFrame);
        const std::size_t fe = sw + static_cast<std::size_t>(fadeFrames);
        const double before = maxJump(soft, sw - 12000, sw);
        const double during = maxJump(soft, sw, fe + 480);
        const double after = maxJump(soft, fe + 4800, fe + 4800 + 12000);
        const double hardDuring = maxJump(hard, sw, fe + 480);
        const double reference = std::max(before, after);
        gate.line(format("  crossfade: max |wet[n]-wet[n-1]| steady-before %.3g, during %.3g, steady-after %.3g; "
                         "hard switch %.3g (fade %.0f ms)",
                         before, during, after, hardDuring, fade * 1000.0));
        gate.check(during <= kCrossfadeJumpRatio * reference,
                   format("crossfade has no discontinuity (during <= %.1fx steady-state jump)", kCrossfadeJumpRatio));
        gate.check(hardDuring > during,
                   "metric sensitivity: a hard reset jumps more than the crossfade");
    }

    // ---- 6. CPU cost on the existing 48 kHz mixer path.
    {
        const auto timeRun = [&](bool fading) {
            Mixer mixer;
            prepare(mixer);
            mixer.setReverbCrossfadeSeconds(2.0f);
            mixer.setReverbEnvironment(eaxEnvironmentId("HANGAR"), false);
            mixer.play(sineSound(220.0, 0.3f), Bus::Effects, 1.0f, true, true, {2.0f, 1.0f, 0.0f}, 1.0f, 20.0f);
            std::vector<float> buffer(1024 * 2);
            for (int b = 0; b < 20; ++b) {  // warm up
                mixer.mix(buffer.data(), 1024);
            }
            if (fading) {
                mixer.setReverbEnvironment(eaxEnvironmentId("AUDITORIUM"), true);
            }
            const int blocks = fading ? 80 : 400;  // 1.7 s of fade, or 8.5 s steady
            const double t0 = nowMs();
            for (int b = 0; b < blocks; ++b) {
                mixer.mix(buffer.data(), 1024);
            }
            const double elapsed = nowMs() - t0;
            const double audioMs = blocks * 1024.0 * 1000.0 / kRate;
            return std::pair<double, double>{elapsed / blocks * 1000.0, elapsed / audioMs};
        };
        const auto single = timeRun(false);
        const auto fading = timeRun(true);
        gate.line(format("  cpu: one reverb %.1f us per 1024-frame block (%.2f%% of real time); "
                         "two reverbs during a crossfade %.1f us (%.2f%%)",
                         single.first, single.second * 100.0, fading.first, fading.second * 100.0));
        gate.check(single.second <= kCpuLimitSingle, format("cpu: one reverb <= %.0f%% of real time", kCpuLimitSingle * 100));
        gate.check(fading.second <= kCpuLimitFading, format("cpu: crossfading pair <= %.0f%% of real time", kCpuLimitFading * 100));
    }

    return gate.result;
}

} // namespace ke::audio
