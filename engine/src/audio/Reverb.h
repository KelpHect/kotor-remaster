#pragma once

#include <cstddef>
#include <vector>

namespace ke::audio {

// PARAMETER-FAITHFUL, NOT BIT-EXACT MILES/EAX.
//
// Retail KotOR 1.0.3.0 hands environment parameters to the Miles Sound System
// EAX provider (Msseax.m3d, Creative EAX 2/3) and the reverb DSP itself runs
// inside that provider or the sound card. Its algorithm is not available, so
// this engine renders the same published parameters with a standard
// feedback-delay-network reverberator (Jot and Chaigne, "Digital delay
// networks for designing artificial reverberators", AES 90th Convention 1991)
// plus a tapped-delay early-reflection stage. Decay time, HF decay ratio,
// reflection delay, reverb delay, room/reflection/reverb levels, room HF
// attenuation, diffusion, size and echo are honoured with their documented
// meaning; the exact late-field texture, modulation, air absorption, LF decay
// and room rolloff of the Creative DSP are not reproduced.

// One EAX 3.0 environment preset. The values are the published EAX preset
// constants (EAX 3.0 SDK / I3DL2 environment table); the retail swkotor.exe
// copies the same 24 rows into a 0x70-byte-per-row table at VA 0x74c610 that
// its environment setter forwards to the provider (verified by dumping the
// table; the Miles preference keys it uses are the EAX2/EAX3 ones).
struct EaxEnvironment {
    int id = 0;               // soundeax.2da row index == retail environment id
    const char* label = "";   // soundeax.2da "label"
    float size = 7.5f;        // metres
    float diffusion = 1.0f;   // 0..1
    int roomMb = -1000;       // overall wet level (millibel)
    int roomHfMb = -100;      // wet high-frequency attenuation at hfReference
    float decayTime = 1.49f;  // RT60, seconds
    float decayHfRatio = 0.83f;
    int reflectionsMb = -2602;      // relative to room
    float reflectionsDelay = 0.007f; // seconds after the direct sound
    int reverbMb = 200;             // relative to room
    float reverbDelay = 0.011f;     // late onset after the first reflection
    float echoTime = 0.25f;
    float echoDepth = 0.0f;
    float modulationTime = 0.25f;  // not rendered
    float modulationDepth = 0.0f;  // not rendered
    float airAbsorptionHfMb = -5.0f; // not rendered
    float hfReference = 5000.0f;
};

constexpr int kEaxEnvironmentCount = 24;

// Retail clamps ids >= 24 to environment 0 without touching the other keys
// (swkotor.exe 005d66d0); here such ids use environment 0's row.
const EaxEnvironment& eaxEnvironment(int id);
// -1 when `label` is not a soundeax.2da label.
int eaxEnvironmentId(const char* label);

// Mono-in, stereo-out reverberator. All memory is allocated in the
// constructor; configure(), reset() and process() never allocate.
class Reverb {
public:
    Reverb();

    void configure(const EaxEnvironment& environment, int sampleRate);
    void reset();
    // `input` is the mono send. Output is the wet signal only, added into
    // outLeft/outRight (callers zero them first when they want wet alone).
    void process(const float* input, float* outLeft, float* outRight, int frames);

    const EaxEnvironment& environment() const { return environment_; }
    // Debug: denormal float values currently held in the delay memory.
    std::size_t denormalCount() const;
    // Test hook. The reverb runs with flush-to-zero/denormals-are-zero set on the
    // calling thread (restored on return); the gate switches that off for one
    // run to prove it is what removes the denormal slowdown. Process-wide.
    static void setDenormalProtection(bool enabled);

    static constexpr int kLines = 8;
    static constexpr int kEarlyTaps = 6;

private:
    struct Damping {
        float p = 0.0f;
        float z = 0.0f;
    };
    struct Line {
        std::vector<float> buffer;
        std::size_t length = 1;
        std::size_t index = 0;
        float gain = 0.0f;
        Damping damping;
    };

    EaxEnvironment environment_;
    int sampleRate_ = 48000;
    Line lines_[kLines];
    std::vector<float> early_;
    std::size_t earlyMask_ = 0;
    std::size_t earlyWrite_ = 0;
    std::size_t earlyOffset_[2][kEarlyTaps] = {};
    float earlyGain_[2][kEarlyTaps] = {};
    std::vector<float> predelay_;
    std::size_t predelayMask_ = 0;
    std::size_t predelayWrite_ = 0;
    std::size_t predelaySamples_ = 0;
    std::vector<float> diffuser_[2];
    std::size_t diffuserLength_[2] = {1, 1};
    std::size_t diffuserIndex_[2] = {0, 0};
    float diffuserGain_ = 0.0f;
    float inputGain_[kLines] = {};
    float outputGain_[2][kLines] = {};
    float earlyLevel_ = 0.0f;
    float lateLevel_ = 0.0f;
    float roomHfCoefficient_ = 0.0f;
    float roomHfState_[2] = {0.0f, 0.0f};
    std::vector<float> echo_[2];
    std::size_t echoMask_ = 0;
    std::size_t echoWrite_ = 0;
    std::size_t echoSamples_ = 1;
    float echoFeedback_ = 0.0f;
};

} // namespace ke::audio
