#include "audio/Reverb.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <xmmintrin.h>
#define KE_REVERB_FTZ 1
#else
#define KE_REVERB_FTZ 0
#endif

namespace ke::audio {

namespace {

constexpr std::size_t kLineBuffer = 16384;  // per FDN line, samples
constexpr std::size_t kRingBuffer = 32768;  // early/predelay/echo rings (power of two)
constexpr std::size_t kRingMask = kRingBuffer - 1;
constexpr float kPi = 3.14159265358979323846f;
// 60 dB of amplitude decay expressed as an energy time constant: ln(10^6).
constexpr float kLn1e6 = 13.815511f;

// The 24 EAX 3.0 environment presets that soundeax.2da indexes by row. Values
// are the published EAX preset constants; swkotor.exe carries the same table.
// NOTE: retail row 23 ("NOREVERB") holds the GENERIC parameters verbatim, so it
// is not silent by itself (see docs/K1_PLAYABILITY.md).
// clang-format off
const EaxEnvironment kEnvironments[kEaxEnvironmentCount] = {
    // id label               size  diff  room roomHF decay  hfRat  refl   rdly   rvb   vdly   echoT  echoD  modT   modD  air  hfRef
    { 0, "GENERIC",           7.5f, 1.0f, -1000,  -100,  1.49f, 0.83f, -2602, 0.007f,  200, 0.011f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    { 1, "PADDEDCELL",        1.4f, 1.0f, -1000, -6000,  0.17f, 0.10f, -1204, 0.001f,  207, 0.002f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    { 2, "ROOM",              1.9f, 1.0f, -1000,  -454,  0.40f, 0.83f, -1646, 0.002f,   53, 0.003f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    { 3, "BATHROOM",          1.4f, 1.0f, -1000, -1200,  1.49f, 0.54f,  -370, 0.007f, 1030, 0.011f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    { 4, "LIVINGROOM",        2.5f, 1.0f, -1000, -6000,  0.50f, 0.10f, -1376, 0.003f,-1104, 0.004f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    { 5, "STONEROOM",        11.6f, 1.0f, -1000,  -300,  2.31f, 0.64f,  -711, 0.012f,   83, 0.017f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    { 6, "AUDITORIUM",       21.6f, 1.0f, -1000,  -476,  4.32f, 0.59f,  -789, 0.020f, -289, 0.030f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    { 7, "CONCERTHALL",      19.6f, 1.0f, -1000,  -500,  3.92f, 0.70f, -1230, 0.020f,   -2, 0.029f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    { 8, "CAVE",             14.6f, 1.0f, -1000,     0,  2.91f, 1.30f,  -602, 0.015f, -302, 0.022f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    { 9, "ARENA",            36.2f, 1.0f, -1000,  -698,  7.24f, 0.33f, -1166, 0.020f,   16, 0.030f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {10, "HANGAR",           50.3f, 1.0f, -1000, -1000, 10.05f, 0.23f,  -602, 0.020f,  198, 0.030f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {11, "CARPETTEDHALLWAY",  1.9f, 1.0f, -1000, -4000,  0.30f, 0.10f, -1831, 0.002f,-1630, 0.030f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {12, "HALLWAY",           1.8f, 1.0f, -1000,  -300,  1.49f, 0.59f, -1219, 0.007f,  441, 0.011f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {13, "STONECORRIDOR",    13.5f, 1.0f, -1000,  -237,  2.70f, 0.79f, -1214, 0.013f,  395, 0.020f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {14, "ALLEY",             7.5f, 0.3f, -1000,  -270,  1.49f, 0.86f, -1204, 0.007f,   -4, 0.011f, 0.125f,0.95f, 0.25f, 0.0f,   -5.0f, 5000.0f},
    {15, "FOREST",           38.0f, 0.3f, -1000, -3300,  1.49f, 0.54f, -2560, 0.162f, -229, 0.088f, 0.125f,1.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {16, "CITY",              7.5f, 0.5f, -1000,  -800,  1.49f, 0.67f, -2273, 0.007f,-1691, 0.011f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {17, "MOUNTAINS",       100.0f, 0.27f,-1000, -2500,  1.49f, 0.21f, -2780, 0.300f,-1434, 0.100f, 0.25f, 1.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {18, "QUARRY",           17.5f, 1.0f, -1000, -1000,  1.49f, 0.83f,-10000, 0.061f,  500, 0.025f, 0.125f,0.7f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {19, "PLAIN",            42.5f, 0.21f,-1000, -2000,  1.49f, 0.50f, -2466, 0.179f,-1926, 0.100f, 0.25f, 1.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {20, "PARKINGLOT",        8.3f, 1.0f, -1000,     0,  1.65f, 1.50f, -1363, 0.008f,-1153, 0.012f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {21, "SEWERPIPE",         1.7f, 0.8f, -1000, -1000,  2.81f, 0.14f,   429, 0.014f, 1023, 0.021f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
    {22, "UNDERWATER",        1.8f, 1.0f, -1000, -4000,  1.49f, 0.10f,  -449, 0.007f, 1700, 0.011f, 0.25f, 0.0f,  1.18f, 0.348f, -5.0f, 5000.0f},
    {23, "NOREVERB",          7.5f, 1.0f, -1000,  -100,  1.49f, 0.83f, -2602, 0.007f,  200, 0.011f, 0.25f, 0.0f,  0.25f, 0.0f,   -5.0f, 5000.0f},
};
// clang-format on

bool isPrime(std::size_t value)
{
    if (value < 2) {
        return false;
    }
    for (std::size_t divisor = 2; divisor * divisor <= value; ++divisor) {
        if (value % divisor == 0) {
            return false;
        }
    }
    return true;
}

std::size_t nextPrime(std::size_t value)
{
    while (!isPrime(value)) {
        ++value;
    }
    return value;
}

float gainFromMb(float millibel)
{
    return std::pow(10.0f, millibel / 2000.0f);
}

// Coefficient p of the one-pole low-pass y = (1 - p) x + p y1 (unity DC gain)
// whose magnitude at `frequency` equals `gain` (0 < gain <= 1).
float lowPassForGain(float gain, float frequency, int sampleRate)
{
    if (gain >= 0.9999f) {
        return 0.0f;
    }
    const float g2 = gain * gain;
    const float c = std::cos(2.0f * kPi * std::min(frequency, 0.45f * static_cast<float>(sampleRate))
                             / static_cast<float>(sampleRate));
    const float a = 1.0f - g2 * c;
    const float disc = std::max(a * a - (1.0f - g2) * (1.0f - g2), 0.0f);
    const float p = (a - std::sqrt(disc)) / (1.0f - g2);
    return std::clamp(p, 0.0f, 0.99999f);
}

std::atomic<bool> gDenormalProtection{true};

struct FlushScope {
#if KE_REVERB_FTZ
    unsigned int saved;
    FlushScope() : saved(_mm_getcsr())
    {
        if (gDenormalProtection.load(std::memory_order_relaxed)) {
            _mm_setcsr(saved | 0x8040u);  // FTZ | DAZ
        }
    }
    ~FlushScope() { _mm_setcsr(saved); }
#endif
};

// Software flush used on the recursive state when no hardware flush exists.
inline float flush(float value)
{
#if KE_REVERB_FTZ
    return value;
#else
    return std::fabs(value) < 1e-25f ? 0.0f : value;
#endif
}

} // namespace

const EaxEnvironment& eaxEnvironment(int id)
{
    return kEnvironments[(id >= 0 && id < kEaxEnvironmentCount) ? id : 0];
}

int eaxEnvironmentId(const char* label)
{
    if (label == nullptr) {
        return -1;
    }
    for (const EaxEnvironment& environment : kEnvironments) {
        if (std::strcmp(environment.label, label) == 0) {
            return environment.id;
        }
    }
    return -1;
}

void Reverb::setDenormalProtection(bool enabled)
{
    gDenormalProtection.store(enabled);
}

Reverb::Reverb()
{
    for (Line& line : lines_) {
        line.buffer.assign(kLineBuffer, 0.0f);
    }
    early_.assign(kRingBuffer, 0.0f);
    predelay_.assign(kRingBuffer, 0.0f);
    for (std::vector<float>& allpass : diffuser_) {
        allpass.assign(512, 0.0f);
    }
    for (std::vector<float>& channel : echo_) {
        channel.assign(kRingBuffer, 0.0f);
    }
    earlyMask_ = kRingMask;
    predelayMask_ = kRingMask;
    echoMask_ = kRingMask;
    configure(kEnvironments[0], 48000);
}

void Reverb::configure(const EaxEnvironment& environment, int sampleRate)
{
    environment_ = environment;
    sampleRate_ = std::max(sampleRate, 8000);
    const float fs = static_cast<float>(sampleRate_);
    const float decay = std::max(environment.decayTime, 0.1f);

    // Line lengths: mutually distinct primes around the room's mean free path
    // (about 2/3 of the room size at the speed of sound), clamped to a range the
    // fixed buffers cover. The decay time does not depend on them (Jot gains).
    static const float kSpread[kLines] = {1.00f, 1.17f, 1.35f, 1.55f, 1.74f, 1.93f, 2.12f, 2.33f};
    const float base = std::clamp(0.67f * environment.size / 343.0f, 0.004f, 0.045f);
    float totalSamples = 0.0f;
    float minLineSeconds = 1.0f;
    for (int i = 0; i < kLines; ++i) {
        std::size_t length = nextPrime(static_cast<std::size_t>(base * kSpread[i] * fs));
        length = std::min<std::size_t>(length, kLineBuffer - 1);
        lines_[i].length = std::max<std::size_t>(length, 2);
        const float seconds = static_cast<float>(lines_[i].length) / fs;
        minLineSeconds = std::min(minLineSeconds, seconds);
        totalSamples += static_cast<float>(lines_[i].length);

        // Absorption per pass: 60 dB over `decay` seconds at low frequency and
        // over decay * decayHfRatio at the HF reference (only ratios below 1 can
        // be realised by a low-pass; larger ratios leave the HF undamped).
        const float lowGain = std::pow(10.0f, -3.0f * seconds / decay);
        lines_[i].gain = lowGain;
        float hfRatio = 1.0f;
        if (environment.decayHfRatio < 0.9999f) {
            const float hfGain = std::pow(
                10.0f, -3.0f * seconds / (decay * std::max(environment.decayHfRatio, 0.05f)));
            hfRatio = hfGain / lowGain;
        }
        lines_[i].damping.p = lowPassForGain(hfRatio, environment.hfReference, sampleRate_);
    }

    // Unit-norm Householder mixing makes the lossless network energy-preserving;
    // the output taps are normalised so the late field has unit energy at the
    // reference level (engineering choice: EAX does not publish its scaling).
    const float norm = 1.0f / std::sqrt(static_cast<float>(kLines));
    static const float kSignLeft[kLines] = {1, 1, -1, -1, 1, 1, -1, -1};
    static const float kSignRight[kLines] = {1, -1, 1, -1, 1, -1, 1, -1};
    for (int i = 0; i < kLines; ++i) {
        inputGain_[i] = norm * ((i & 1) != 0 ? -1.0f : 1.0f);
    }
    const float lateNorm = std::sqrt(totalSamples * kLn1e6 / (decay * fs));
    const float roomGain = gainFromMb(static_cast<float>(environment.roomMb));
    lateLevel_ = roomGain * gainFromMb(static_cast<float>(environment.reverbMb)) * std::min(lateNorm, 4.0f);
    for (int i = 0; i < kLines; ++i) {
        outputGain_[0][i] = norm * kSignLeft[i];
        outputGain_[1][i] = norm * kSignRight[i];
    }

    // Late field starts `reverbDelay` after the first reflection.
    const float lateOnset = environment.reflectionsDelay + environment.reverbDelay;
    predelaySamples_ = static_cast<std::size_t>(
        std::max(0.0f, lateOnset - minLineSeconds) * fs);
    predelaySamples_ = std::min<std::size_t>(predelaySamples_, kRingBuffer - 1);

    // Input diffusion.
    for (int a = 0; a < 2; ++a) {
        const float scale = fs / 48000.0f;
        const std::size_t length = nextPrime(static_cast<std::size_t>((a == 0 ? 53.0f : 79.0f) * scale));
        diffuserLength_[a] = std::min<std::size_t>(length, diffuser_[a].size() - 1);
    }
    diffuserGain_ = 0.6f * std::clamp(environment.diffusion, 0.0f, 1.0f);

    // Early reflections: tapped delays from the first-reflection delay out to the
    // late onset. Left and right use different spacings and polarities.
    const float window = std::clamp(environment.reverbDelay, 0.002f, 0.06f);
    static const float kFractionLeft[kEarlyTaps] = {0.00f, 0.14f, 0.31f, 0.50f, 0.72f, 0.93f};
    static const float kFractionRight[kEarlyTaps] = {0.00f, 0.09f, 0.24f, 0.43f, 0.66f, 0.88f};
    static const float kPolarityLeft[kEarlyTaps] = {1, -1, 1, -1, 1, -1};
    static const float kPolarityRight[kEarlyTaps] = {-1, 1, 1, -1, -1, 1};
    float energyLeft = 0.0f;
    float energyRight = 0.0f;
    for (int k = 0; k < kEarlyTaps; ++k) {
        const float decayStep = std::pow(0.82f, static_cast<float>(k));
        const float timeLeft = environment.reflectionsDelay + window * kFractionLeft[k];
        const float timeRight = environment.reflectionsDelay + window * kFractionRight[k];
        earlyOffset_[0][k] = std::min<std::size_t>(
            std::max<std::size_t>(static_cast<std::size_t>(timeLeft * fs), 1), kRingBuffer - 1);
        earlyOffset_[1][k] = std::min<std::size_t>(
            std::max<std::size_t>(static_cast<std::size_t>(timeRight * fs), 1), kRingBuffer - 1);
        earlyGain_[0][k] = kPolarityLeft[k] * decayStep;
        earlyGain_[1][k] = kPolarityRight[k] * decayStep;
        energyLeft += decayStep * decayStep;
        energyRight += decayStep * decayStep;
    }
    for (int k = 0; k < kEarlyTaps; ++k) {
        earlyGain_[0][k] /= std::sqrt(energyLeft);
        earlyGain_[1][k] /= std::sqrt(energyRight);
    }
    earlyLevel_ = roomGain * gainFromMb(static_cast<float>(environment.reflectionsMb));

    // Wet high-frequency attenuation at the HF reference.
    roomHfCoefficient_ = lowPassForGain(gainFromMb(static_cast<float>(environment.roomHfMb)),
                                        environment.hfReference, sampleRate_);

    echoSamples_ = std::clamp<std::size_t>(static_cast<std::size_t>(environment.echoTime * fs), 1,
                                           kRingBuffer - 1);
    echoFeedback_ = 0.6f * std::clamp(environment.echoDepth, 0.0f, 1.0f);

    reset();
}

void Reverb::reset()
{
    for (Line& line : lines_) {
        std::fill(line.buffer.begin(), line.buffer.end(), 0.0f);
        line.index = 0;
        line.damping.z = 0.0f;
    }
    std::fill(early_.begin(), early_.end(), 0.0f);
    std::fill(predelay_.begin(), predelay_.end(), 0.0f);
    for (std::vector<float>& allpass : diffuser_) {
        std::fill(allpass.begin(), allpass.end(), 0.0f);
    }
    for (std::vector<float>& channel : echo_) {
        std::fill(channel.begin(), channel.end(), 0.0f);
    }
    earlyWrite_ = 0;
    predelayWrite_ = 0;
    diffuserIndex_[0] = diffuserIndex_[1] = 0;
    echoWrite_ = 0;
    roomHfState_[0] = roomHfState_[1] = 0.0f;
}

std::size_t Reverb::denormalCount() const
{
    std::size_t count = 0;
    const auto scan = [&count](const std::vector<float>& values) {
        for (const float value : values) {
            if (std::fpclassify(value) == FP_SUBNORMAL) {
                ++count;
            }
        }
    };
    for (const Line& line : lines_) {
        scan(line.buffer);
        count += std::fpclassify(line.damping.z) == FP_SUBNORMAL ? 1 : 0;
    }
    scan(early_);
    scan(predelay_);
    for (const std::vector<float>& allpass : diffuser_) {
        scan(allpass);
    }
    for (const std::vector<float>& channel : echo_) {
        scan(channel);
    }
    return count;
}

void Reverb::process(const float* input, float* outLeft, float* outRight, int frames)
{
    if (input == nullptr || outLeft == nullptr || outRight == nullptr || frames <= 0) {
        return;
    }
    FlushScope flushScope;
    const bool echoOn = echoFeedback_ > 0.0f;
    float taps[kLines];
    float damped[kLines];
    for (int n = 0; n < frames; ++n) {
        const float in = input[n];

        // Early reflections.
        early_[earlyWrite_] = in;
        float earlyLeft = 0.0f;
        float earlyRight = 0.0f;
        for (int k = 0; k < kEarlyTaps; ++k) {
            earlyLeft += earlyGain_[0][k] * early_[(earlyWrite_ - earlyOffset_[0][k]) & earlyMask_];
            earlyRight += earlyGain_[1][k] * early_[(earlyWrite_ - earlyOffset_[1][k]) & earlyMask_];
        }
        earlyWrite_ = (earlyWrite_ + 1) & earlyMask_;

        // Late input: two Schroeder all-pass diffusers, then the pre-delay.
        float diffused = in;
        for (int a = 0; a < 2; ++a) {
            std::vector<float>& buffer = diffuser_[a];
            const float delayed = buffer[diffuserIndex_[a]];
            const float w = diffused + diffuserGain_ * delayed;
            diffused = -diffuserGain_ * w + delayed;
            buffer[diffuserIndex_[a]] = flush(w);
            diffuserIndex_[a] = diffuserIndex_[a] + 1 >= diffuserLength_[a] ? 0 : diffuserIndex_[a] + 1;
        }
        predelay_[predelayWrite_] = diffused;
        const float lateIn = predelay_[(predelayWrite_ - predelaySamples_) & predelayMask_];
        predelayWrite_ = (predelayWrite_ + 1) & predelayMask_;

        // Feedback delay network: read, absorb, mix with a Householder matrix.
        float lateLeft = 0.0f;
        float lateRight = 0.0f;
        float sum = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            Line& line = lines_[i];
            const float tap = line.buffer[line.index];
            taps[i] = tap;
            lateLeft += outputGain_[0][i] * tap;
            lateRight += outputGain_[1][i] * tap;
            const float absorbed = tap * line.gain;
            line.damping.z = flush((1.0f - line.damping.p) * absorbed + line.damping.p * line.damping.z);
            damped[i] = line.damping.z;
            sum += damped[i];
        }
        const float mix = sum * (2.0f / static_cast<float>(kLines));
        for (int i = 0; i < kLines; ++i) {
            Line& line = lines_[i];
            line.buffer[line.index] = flush(damped[i] - mix + inputGain_[i] * lateIn);
            line.index = line.index + 1 >= line.length ? 0 : line.index + 1;
        }
        lateLeft *= lateLevel_;
        lateRight *= lateLevel_;

        if (echoOn) {
            const std::size_t read = (echoWrite_ - echoSamples_) & echoMask_;
            lateLeft += echoFeedback_ * echo_[0][read];
            lateRight += echoFeedback_ * echo_[1][read];
            echo_[0][echoWrite_] = flush(lateLeft);
            echo_[1][echoWrite_] = flush(lateRight);
            echoWrite_ = (echoWrite_ + 1) & echoMask_;
        }

        // Room HF attenuation on the combined wet signal.
        const float wetLeft = earlyLevel_ * earlyLeft + lateLeft;
        const float wetRight = earlyLevel_ * earlyRight + lateRight;
        roomHfState_[0] = flush((1.0f - roomHfCoefficient_) * wetLeft + roomHfCoefficient_ * roomHfState_[0]);
        roomHfState_[1] = flush((1.0f - roomHfCoefficient_) * wetRight + roomHfCoefficient_ * roomHfState_[1]);
        outLeft[n] += roomHfState_[0];
        outRight[n] += roomHfState_[1];
        (void)taps;
    }
}

} // namespace ke::audio
