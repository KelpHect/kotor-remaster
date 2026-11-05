#include "audio/Audio.h"

#include "audio/Reverb.h"

#include "dr_mp3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace ke::audio {

namespace {

constexpr int kMixRate = 48000;
constexpr int kChannels = 2;

std::uint16_t readU16(const std::uint8_t* data, std::size_t size, std::size_t offset)
{
    if (offset + 2 > size) {
        return 0;
    }
    return static_cast<std::uint16_t>(data[offset]
                                      | (static_cast<std::uint16_t>(data[offset + 1]) << 8));
}

std::uint32_t readU32(const std::uint8_t* data, std::size_t size, std::size_t offset)
{
    if (offset + 4 > size) {
        return 0;
    }
    return static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

float clampSample(float value)
{
    return std::clamp(value, -1.0f, 1.0f);
}

bool decodeImaAdpcm(const std::uint8_t* data, std::size_t dataSize, std::uint16_t channels,
                    std::uint32_t sampleRate, std::uint16_t blockAlign,
                    std::uint16_t samplesPerBlock, SoundData& out)
{
    static const int indexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    static const int stepTable[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767,
    };
    if (channels == 0 || blockAlign < 4u * channels || dataSize < blockAlign) {
        return false;
    }
    if (samplesPerBlock == 0) {
        samplesPerBlock = static_cast<std::uint16_t>(
            (blockAlign - 4u * channels) * 2u / channels + 1u);
    }
    out.name.clear();
    out.sampleRate = static_cast<int>(sampleRate);
    out.channels = static_cast<int>(channels);
    out.samples.clear();
    const std::size_t blocks = dataSize / blockAlign;
    out.samples.reserve(blocks * static_cast<std::size_t>(samplesPerBlock) * channels);
    for (std::size_t block = 0; block < blocks; ++block) {
        const std::uint8_t* base = data + block * blockAlign;
        std::vector<std::int32_t> predictor(channels, 0);
        std::vector<int> index(channels, 0);
        for (std::uint16_t channel = 0; channel < channels; ++channel) {
            predictor[channel] = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(base[channel * 4])
                | (static_cast<std::uint16_t>(base[channel * 4 + 1]) << 8));
            index[channel] = std::clamp(static_cast<int>(base[channel * 4 + 2]), 0, 88);
        }
        std::vector<std::vector<float>> decoded(
            channels, std::vector<float>());
        for (std::vector<float>& channel : decoded) {
            channel.reserve(samplesPerBlock);
        }
        const auto emit = [&](std::uint16_t channel, int nibble) {
            const int step = stepTable[index[channel]];
            int diff = step >> 3;
            if ((nibble & 1) != 0) diff += step >> 2;
            if ((nibble & 2) != 0) diff += step >> 1;
            if ((nibble & 4) != 0) diff += step;
            if ((nibble & 8) != 0) diff = -diff;
            predictor[channel] = std::clamp(predictor[channel] + diff, -32768, 32767);
            index[channel] = std::clamp(index[channel] + indexTable[nibble], 0, 88);
            decoded[channel].push_back(static_cast<float>(predictor[channel]) / 32768.0f);
        };
        const std::uint8_t* payload = base + 4u * channels;
        const std::size_t payloadSize = blockAlign - 4u * channels;
        if (channels == 1) {
            std::size_t cursor = 0;
            while (cursor < payloadSize && decoded[0].size() < samplesPerBlock) {
                const std::uint8_t byte = payload[cursor++];
                emit(0, byte & 0x0F);
                if (decoded[0].size() < samplesPerBlock) {
                    emit(0, (byte >> 4) & 0x0F);
                }
            }
        } else {
            // IMA stereo/multichannel: 8-sample groups per channel, 4 bytes each.
            std::size_t cursor = 0;
            const std::size_t group = 8u * static_cast<std::size_t>(channels);
            while (cursor + group <= payloadSize
                   && decoded[0].size() < samplesPerBlock) {
                for (std::uint16_t channel = 0; channel < channels; ++channel) {
                    for (std::size_t byte = 0; byte < 4; ++byte) {
                        const std::uint8_t value = payload[cursor + channel * 4u + byte];
                        if (decoded[channel].size() < samplesPerBlock) {
                            emit(channel, value & 0x0F);
                        }
                        if (decoded[channel].size() < samplesPerBlock) {
                            emit(channel, (value >> 4) & 0x0F);
                        }
                    }
                }
                cursor += group;
            }
        }
        const std::size_t frames = std::min<std::size_t>(
            samplesPerBlock, decoded[0].size());
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::uint16_t channel = 0; channel < channels; ++channel) {
                out.samples.push_back(decoded[channel][frame]);
            }
        }
    }
    return !out.samples.empty();
}

} // namespace

std::size_t SoundData::frameCount() const
{
    if (channels <= 0) {
        return 0;
    }
    return samples.size() / static_cast<std::size_t>(channels);
}

double SoundData::durationSeconds() const
{
    if (sampleRate <= 0) {
        return 0.0;
    }
    return static_cast<double>(frameCount()) / static_cast<double>(sampleRate);
}

bool decodeWav(const std::vector<std::uint8_t>& bytes, const std::string& name,
               SoundData& out, std::string& error)
{
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        error = name + ": not a RIFF/WAVE stream";
        return false;
    }
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bits = 0;
    std::uint16_t samplesPerBlock = 0;
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const std::uint8_t* id = bytes.data() + offset;
        const std::uint32_t size = readU32(bytes.data(), bytes.size(), offset + 4);
        const std::size_t payload = offset + 8;
        if (payload + size > bytes.size()) {
            error = name + ": WAV chunk overruns file";
            return false;
        }
        if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
            format = readU16(bytes.data(), bytes.size(), payload);
            channels = readU16(bytes.data(), bytes.size(), payload + 2);
            sampleRate = readU32(bytes.data(), bytes.size(), payload + 4);
            blockAlign = readU16(bytes.data(), bytes.size(), payload + 12);
            bits = readU16(bytes.data(), bytes.size(), payload + 14);
            if (size >= 20) {
                samplesPerBlock = readU16(bytes.data(), bytes.size(), payload + 18);
            }
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = bytes.data() + payload;
            dataSize = size;
        }
        offset = payload + size + (size & 1u);
    }
    if (format == 0x11u) {
        if (!decodeImaAdpcm(data, dataSize, channels, sampleRate, blockAlign,
                            samplesPerBlock, out)) {
            error = name + ": IMA ADPCM decode failed";
            return false;
        }
        out.name = name;
        return true;
    }
    if (format != 1 && format != 0xFFFEu) {
        error = name + ": unsupported WAV format " + std::to_string(format);
        return false;
    }
    if (channels == 0 || sampleRate == 0 || (bits != 8 && bits != 16) || data == nullptr
        || dataSize == 0) {
        error = name + ": unsupported WAV layout";
        return false;
    }
    const std::size_t frameSize = static_cast<std::size_t>(channels)
        * static_cast<std::size_t>(bits / 8u);
    if (frameSize == 0) {
        error = name + ": empty WAV frame";
        return false;
    }
    const std::size_t frames = dataSize / frameSize;
    out.name = name;
    out.sampleRate = static_cast<int>(sampleRate);
    out.channels = static_cast<int>(channels);
    out.samples.clear();
    out.samples.reserve(frames * static_cast<std::size_t>(channels));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const std::size_t sample = frame * frameSize + channel * (bits / 8u);
            if (bits == 8) {
                out.samples.push_back((static_cast<float>(data[sample]) - 128.0f) / 128.0f);
            } else {
                const std::int16_t value = static_cast<std::int16_t>(
                    static_cast<std::uint16_t>(data[sample])
                    | (static_cast<std::uint16_t>(data[sample + 1]) << 8));
                out.samples.push_back(static_cast<float>(value) / 32768.0f);
            }
        }
    }
    return !out.samples.empty();
}

bool decodeMp3(const std::vector<std::uint8_t>& bytes, const std::string& name,
               SoundData& out, std::string& error)
{
    drmp3 mp3{};
    if (!drmp3_init_memory(&mp3, bytes.data(), bytes.size(), nullptr)) {
        error = name + ": MP3 init failed";
        return false;
    }
    const drmp3_uint64 frames = drmp3_get_pcm_frame_count(&mp3);
    const std::uint32_t channels = std::max(1u, mp3.channels);
    if (frames == 0) {
        drmp3_uninit(&mp3);
        error = name + ": MP3 has no frames";
        return false;
    }
    std::vector<float> pcm(static_cast<std::size_t>(frames) * channels);
    const drmp3_uint64 read = drmp3_read_pcm_frames_f32(&mp3, frames, pcm.data());
    out.name = name;
    out.sampleRate = static_cast<int>(mp3.sampleRate);
    out.channels = static_cast<int>(channels);
    out.samples.assign(pcm.begin(),
                       pcm.begin() + static_cast<std::ptrdiff_t>(read * channels));
    drmp3_uninit(&mp3);
    if (out.samples.empty() || out.sampleRate <= 0) {
        error = name + ": MP3 decode produced no samples";
        return false;
    }
    return true;
}

bool decodeAudio(const std::vector<std::uint8_t>& bytes, const std::string& name,
                 SoundData& out, std::string& error)
{
    // Shipped streammusic/streamsounds files may carry the verified 0x1d6-byte
    // KotOR prefix in front of a normal RIFF/WAVE payload. Locate the first
    // RIFF/WAVE header rather than trusting the extension or the first bytes.
    std::size_t riff = std::string::npos;
    const std::size_t searchLimit = std::min<std::size_t>(bytes.size(), 4096);
    for (std::size_t offset = 0; offset + 12 <= searchLimit; ++offset) {
        if (std::memcmp(bytes.data() + offset, "RIFF", 4) == 0
            && std::memcmp(bytes.data() + offset + 8, "WAVE", 4) == 0) {
            riff = offset;
            break;
        }
    }
    if (riff != std::string::npos) {
        std::vector<std::uint8_t> wav(bytes.begin() + static_cast<std::ptrdiff_t>(riff),
                                      bytes.end());
        // Some shipped music files carry a zero-length `data` chunk followed by
        // an ID3 tag and MP3 frames. The RIFF fmt block is a stale PCM header.
        std::size_t dataHeader = std::string::npos;
        std::uint32_t dataSize = 0;
        for (std::size_t offset = 12; offset + 8 <= wav.size();) {
            const std::uint32_t size = readU32(wav.data(), wav.size(), offset + 4);
            if (std::memcmp(wav.data() + offset, "data", 4) == 0) {
                dataHeader = offset;
                dataSize = size;
                break;
            }
            const std::size_t next = offset + 8u + size + (size & 1u);
            if (next <= offset) {
                break;
            }
            offset = next;
        }
        if (dataHeader != std::string::npos && dataSize == 0) {
            std::size_t audioStart = dataHeader + 8u;
            if (audioStart + 10 <= wav.size()
                && std::memcmp(wav.data() + audioStart, "ID3", 3) == 0) {
                const std::size_t tagSize = (static_cast<std::size_t>(wav[audioStart + 6]) << 21)
                    | (static_cast<std::size_t>(wav[audioStart + 7]) << 14)
                    | (static_cast<std::size_t>(wav[audioStart + 8]) << 7)
                    | static_cast<std::size_t>(wav[audioStart + 9]);
                audioStart += 10u + tagSize;
            }
            if (audioStart + 2 <= wav.size() && wav[audioStart] == 0xFFu
                && (wav[audioStart + 1] & 0xE0u) == 0xE0u) {
                std::vector<std::uint8_t> mp3(
                    wav.begin() + static_cast<std::ptrdiff_t>(audioStart), wav.end());
                return decodeMp3(mp3, name, out, error);
            }
        }
        return decodeWav(wav, name, out, error);
    }
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "BMU ", 4) == 0) {
        std::vector<std::uint8_t> mp3(bytes.begin() + 4, bytes.end());
        return decodeMp3(mp3, name, out, error);
    }
    const bool frameSync = bytes.size() >= 2 && bytes[0] == 0xFFu
        && (bytes[1] & 0xE0u) == 0xE0u;
    if (frameSync || (bytes.size() >= 3 && std::memcmp(bytes.data(), "ID3", 3) == 0)) {
        return decodeMp3(bytes, name, out, error);
    }
    error = name + ": unknown audio signature";
    return false;
}

bool resample(SoundData& data, int sampleRate, int channels)
{
    if (data.sampleRate == sampleRate && data.channels == channels) {
        return true;
    }
    if (data.sampleRate <= 0 || data.channels <= 0 || channels <= 0) {
        return false;
    }
    const std::size_t inFrames = data.frameCount();
    if (inFrames == 0) {
        return false;
    }
    const double ratio = static_cast<double>(sampleRate) / static_cast<double>(data.sampleRate);
    const std::size_t outFrames = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(static_cast<double>(inFrames) * ratio)));
    std::vector<float> output(outFrames * static_cast<std::size_t>(channels));
    for (std::size_t frame = 0; frame < outFrames; ++frame) {
        const double source = static_cast<double>(frame) / ratio;
        const std::size_t lo = static_cast<std::size_t>(std::floor(source));
        const std::size_t hi = std::min(lo + 1, inFrames - 1);
        const float t = static_cast<float>(source - static_cast<double>(lo));
        for (int channel = 0; channel < channels; ++channel) {
            const int sourceChannel = std::min(channel, data.channels - 1);
            const float a = data.samples[lo * static_cast<std::size_t>(data.channels)
                                         + static_cast<std::size_t>(sourceChannel)];
            const float b = data.samples[hi * static_cast<std::size_t>(data.channels)
                                         + static_cast<std::size_t>(sourceChannel)];
            output[frame * static_cast<std::size_t>(channels)
                   + static_cast<std::size_t>(channel)] = a + (b - a) * t;
        }
    }
    data.samples = std::move(output);
    data.sampleRate = sampleRate;
    data.channels = channels;
    return true;
}

const std::vector<std::string>& reverbLabels()
{
    static const std::vector<std::string> labels = [] {
        std::vector<std::string> names;
        for (int id = 0; id < kEaxEnvironmentCount; ++id) {
            names.emplace_back(eaxEnvironment(id).label);
        }
        return names;
    }();
    return labels;
}

ReverbPreset reverbPreset(const std::string& label)
{
    const int id = eaxEnvironmentId(label.c_str());
    if (id < 0) {
        return {"GENERIC", 0};
    }
    return {label, id};
}

struct Mixer::Impl {
    struct Voice {
        int id = 0;
        std::shared_ptr<const SoundData> data;
        Bus bus = Bus::Effects;
        double cursor = 0.0;
        float volume = 1.0f;
        float pan = 0.0f;
        float pitch = 1.0f;
        bool loop = false;
        bool positional = false;
        bool active = true;
        kotor::Vec3 position;
        float minDistance = 1.0f;
        float maxDistance = 10.0f;
    };

    int sampleRate = kMixRate;
    float master = 0.85f;
    float busVolume[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    kotor::Vec3 listener;
    float yaw = 0.0f;
    ReverbPreset preset = reverbPreset("GENERIC");
    std::vector<Voice> voices;
    int nextId = 1;
    std::vector<float> buses[5];
    float lastRms = 0.0f;

    // Environment reverb (see audio/Reverb.h). Two engines exist so a room change
    // can crossfade: `active` carries the current room and, while `fading`,
    // `incoming` ramps in. Everything here is allocated once.
    static constexpr int kNoRequest = -1000;
    static constexpr int kChunk = 2048;
    Reverb engines[2];
    int active = 0;
    int environment = -1;          // applied environment, -1 = off
    int incomingEnvironment = -1;
    bool fading = false;
    std::size_t fadePos = 0;
    std::size_t fadeTotal = 1;
    float crossfadeSeconds = 0.15f;
    float send = 0.6f;
    std::atomic<int> requestedEnvironment{kNoRequest};
    std::atomic<bool> requestedCrossfade{false};
    int queuedEnvironment = kNoRequest;
    bool queuedCrossfade = false;
    std::vector<float> sendMono;
    std::vector<float> wet;  // interleaved stereo
    std::array<float, kChunk> chunkLeft[2] = {};
    std::array<float, kChunk> chunkRight[2] = {};

    Impl()
    {
        for (std::vector<float>& bus : buses) {
            bus.reserve(16384u * 2u);
        }
        sendMono.reserve(16384);
        wet.reserve(16384u * 2u);
    }

    void applyEnvironment(int next, bool crossfade)
    {
        if (next == environment && !fading) {
            return;
        }
        if (fading) {
            queuedEnvironment = next;
            queuedCrossfade = crossfade;
            return;
        }
        const bool wasOff = environment < 0;
        if (!crossfade || wasOff) {
            if (next >= 0) {
                engines[active].configure(eaxEnvironment(next), sampleRate);
            }
            environment = next;
            return;
        }
        incomingEnvironment = next;
        if (next >= 0) {
            engines[1 - active].configure(eaxEnvironment(next), sampleRate);
        }
        fadePos = 0;
        fadeTotal = std::max<std::size_t>(
            1, static_cast<std::size_t>(crossfadeSeconds * static_cast<float>(sampleRate)));
        fading = true;
    }

    void takeRequests()
    {
        if (!fading && queuedEnvironment != kNoRequest) {
            const int next = queuedEnvironment;
            queuedEnvironment = kNoRequest;
            applyEnvironment(next, queuedCrossfade);
        }
        const int next = requestedEnvironment.exchange(kNoRequest);
        if (next != kNoRequest) {
            applyEnvironment(next, requestedCrossfade.load());
        }
    }

    // Runs the reverb over `frames` mono send samples into `wet` (interleaved).
    void renderReverb(int frames)
    {
        wet.assign(static_cast<std::size_t>(frames) * 2u, 0.0f);
        if (environment < 0 && !fading) {
            return;
        }
        for (int offset = 0; offset < frames; offset += kChunk) {
            const int count = std::min(kChunk, frames - offset);
            const float* in = sendMono.data() + offset;
            const std::size_t cn = static_cast<std::size_t>(count);
            const bool newOn = fading && incomingEnvironment >= 0;
            std::fill_n(chunkLeft[0].begin(), cn, 0.0f);
            std::fill_n(chunkRight[0].begin(), cn, 0.0f);
            if (environment >= 0) {
                engines[active].process(in, chunkLeft[0].data(), chunkRight[0].data(), count);
            }
            if (newOn) {
                std::fill_n(chunkLeft[1].begin(), cn, 0.0f);
                std::fill_n(chunkRight[1].begin(), cn, 0.0f);
                engines[1 - active].process(in, chunkLeft[1].data(), chunkRight[1].data(), count);
            }
            for (int i = 0; i < count; ++i) {
                float left = chunkLeft[0][static_cast<std::size_t>(i)];
                float right = chunkRight[0][static_cast<std::size_t>(i)];
                if (fading) {
                    const float position = std::min(
                        1.0f, static_cast<float>(fadePos + static_cast<std::size_t>(offset + i))
                                  / static_cast<float>(fadeTotal));
                    const float angle = position * 1.57079632679f;  // equal-power
                    // Exact end points so the retired reverb contributes nothing once
                    // the fade is complete, whatever the block size.
                    const float outGain = position >= 1.0f ? 0.0f : std::cos(angle);
                    const float inGain = position >= 1.0f ? 1.0f : std::sin(angle);
                    left *= outGain;
                    right *= outGain;
                    if (newOn) {
                        left += inGain * chunkLeft[1][static_cast<std::size_t>(i)];
                        right += inGain * chunkRight[1][static_cast<std::size_t>(i)];
                    }
                }
                wet[static_cast<std::size_t>(offset + i) * 2u] = left;
                wet[static_cast<std::size_t>(offset + i) * 2u + 1u] = right;
            }
        }
        if (fading) {
            fadePos += static_cast<std::size_t>(frames);
            if (fadePos >= fadeTotal) {
                fading = false;
                if (incomingEnvironment >= 0) {
                    active = 1 - active;
                }
                environment = incomingEnvironment;
                incomingEnvironment = -1;
            }
        }
    }

    float voiceGain(const Voice& voice) const
    {
        float gain = voice.volume;
        if (voice.positional) {
            const float dx = voice.position.x - listener.x;
            const float dy = voice.position.y - listener.y;
            const float dz = voice.position.z - listener.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float maxDistance = std::max(voice.maxDistance, voice.minDistance + 0.01f);
            const float attenuation = std::clamp(
                (maxDistance - distance) / (maxDistance - voice.minDistance), 0.0f, 1.0f);
            gain *= attenuation * attenuation;
        }
        return gain;
    }

    float voicePan(const Voice& voice) const
    {
        if (!voice.positional) {
            return std::clamp(voice.pan, -1.0f, 1.0f);
        }
        const float dx = voice.position.x - listener.x;
        const float dy = voice.position.y - listener.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= 0.0001f) {
            return 0.0f;
        }
        const float rightX = std::sin(yaw);
        const float rightY = -std::cos(yaw);
        return std::clamp((dx * rightX + dy * rightY) / distance, -1.0f, 1.0f);
    }

    float sample(const SoundData& data, std::size_t frame, int channel) const
    {
        const std::size_t frames = data.frameCount();
        if (frames == 0) {
            return 0.0f;
        }
        const std::size_t index = std::min(frame, frames - 1);
        const int sourceChannel = std::min(channel, data.channels - 1);
        return data.samples[index * static_cast<std::size_t>(data.channels)
                            + static_cast<std::size_t>(sourceChannel)];
    }
};

Mixer::Mixer()
    : impl_(std::make_unique<Impl>())
{
}

Mixer::~Mixer() = default;

void Mixer::setSampleRate(int sampleRate)
{
    if (sampleRate > 0) {
        impl_->sampleRate = sampleRate;
    }
}

int Mixer::sampleRate() const
{
    return impl_->sampleRate;
}

void Mixer::setListener(kotor::Vec3 position, float yaw)
{
    impl_->listener = position;
    impl_->yaw = yaw;
}

void Mixer::setBusVolume(Bus bus, float volume)
{
    impl_->busVolume[static_cast<std::size_t>(bus)] = std::clamp(volume, 0.0f, 4.0f);
}

float Mixer::busVolume(Bus bus) const
{
    return impl_->busVolume[static_cast<std::size_t>(bus)];
}

void Mixer::setReverb(const std::string& label)
{
    impl_->preset = reverbPreset(label);
    impl_->requestedCrossfade.store(false);
    impl_->requestedEnvironment.store(impl_->preset.environment);
}

void Mixer::setReverbEnvironment(int environment, bool crossfade)
{
    const int id = environment >= kEaxEnvironmentCount ? 0 : std::max(environment, -1);
    impl_->preset = id >= 0 ? ReverbPreset{eaxEnvironment(id).label, id} : ReverbPreset{"OFF", -1};
    impl_->requestedCrossfade.store(crossfade);
    impl_->requestedEnvironment.store(id);
}

const ReverbPreset& Mixer::reverb() const
{
    return impl_->preset;
}

int Mixer::reverbEnvironment() const
{
    return impl_->environment;
}

bool Mixer::reverbFading() const
{
    return impl_->fading;
}

void Mixer::setReverbCrossfadeSeconds(float seconds)
{
    impl_->crossfadeSeconds = std::clamp(seconds, 0.01f, 2.0f);
}

void Mixer::setReverbSend(float level)
{
    impl_->send = std::clamp(level, 0.0f, 1.0f);
}

float Mixer::reverbSend() const
{
    return impl_->send;
}

std::size_t Mixer::reverbDenormalCount() const
{
    return impl_->engines[0].denormalCount() + impl_->engines[1].denormalCount();
}

int Mixer::play(const std::shared_ptr<const SoundData>& data, Bus bus, float volume,
                bool loop, bool positional, kotor::Vec3 position, float minDistance,
                float maxDistance, float pan, float pitch)
{
    if (!data || data->frameCount() == 0) {
        return 0;
    }
    Impl::Voice voice;
    voice.id = impl_->nextId++;
    voice.data = data;
    voice.bus = bus;
    voice.volume = std::clamp(volume, 0.0f, 4.0f);
    voice.loop = loop;
    voice.positional = positional;
    voice.position = position;
    voice.minDistance = std::max(minDistance, 0.0f);
    voice.maxDistance = std::max(maxDistance, voice.minDistance + 0.01f);
    voice.pan = std::clamp(pan, -1.0f, 1.0f);
    voice.pitch = std::clamp(pitch, 0.05f, 8.0f);
    impl_->voices.push_back(std::move(voice));
    return impl_->voices.back().id;
}

int Mixer::playMusic(const std::shared_ptr<const SoundData>& data, float volume, bool loop)
{
    stopBus(Bus::Music);
    return play(data, Bus::Music, volume, loop, false, {}, 1.0f, 10.0f, 0.0f, 1.0f);
}

void Mixer::stopMusic()
{
    stopBus(Bus::Music);
}

void Mixer::stopVoice(int voice)
{
    for (Impl::Voice& entry : impl_->voices) {
        if (entry.id == voice) {
            entry.active = false;
        }
    }
}

void Mixer::stopBus(Bus bus)
{
    for (Impl::Voice& entry : impl_->voices) {
        if (entry.bus == bus) {
            entry.active = false;
        }
    }
}

void Mixer::stopAll()
{
    for (Impl::Voice& entry : impl_->voices) {
        entry.active = false;
    }
}

void Mixer::setVoiceVolume(int voice, float volume)
{
    for (Impl::Voice& entry : impl_->voices) {
        if (entry.id == voice) {
            entry.volume = std::clamp(volume, 0.0f, 4.0f);
        }
    }
}

void Mixer::setVoicePosition(int voice, kotor::Vec3 position)
{
    for (Impl::Voice& entry : impl_->voices) {
        if (entry.id == voice) {
            entry.position = position;
            entry.positional = true;
        }
    }
}

void Mixer::setVoiceCursor(int voice, double seconds)
{
    for (Impl::Voice& entry : impl_->voices) {
        if (entry.id == voice && entry.data) {
            const double frames = static_cast<double>(entry.data->frameCount());
            entry.cursor = std::clamp(seconds, 0.0, frames / impl_->sampleRate)
                * static_cast<double>(impl_->sampleRate);
        }
    }
}

double Mixer::voiceCursor(int voice) const
{
    for (const Impl::Voice& entry : impl_->voices) {
        if (entry.id == voice) {
            return entry.cursor / static_cast<double>(impl_->sampleRate);
        }
    }
    return -1.0;
}

void Mixer::mix(float* interleavedStereo, int frames)
{
    if (interleavedStereo == nullptr || frames <= 0) {
        return;
    }
    const std::size_t samples = static_cast<std::size_t>(frames) * 2u;
    impl_->takeRequests();
    for (std::vector<float>& buffer : impl_->buses) {
        buffer.assign(samples, 0.0f);
    }
    // Mono reverb send, summed over positional voices (retail sends only 3D
    // samples and positional streams to the provider: swkotor.exe 005d71c0 and
    // 005dc560; music and movie streams never get a reverb level).
    const bool reverbOn = impl_->environment >= 0 || impl_->fading;
    impl_->sendMono.assign(static_cast<std::size_t>(frames), 0.0f);

    for (Impl::Voice& voice : impl_->voices) {
        if (!voice.active || !voice.data) {
            continue;
        }
        const SoundData& data = *voice.data;
        const std::size_t sourceFrames = data.frameCount();
        if (sourceFrames == 0) {
            voice.active = false;
            continue;
        }
        const float gain = impl_->voiceGain(voice);
        const float pan = impl_->voicePan(voice);
        const float leftGain = gain * std::sqrt((1.0f - pan) * 0.5f);
        const float rightGain = gain * std::sqrt((1.0f + pan) * 0.5f);
        std::vector<float>& bus = impl_->buses[static_cast<std::size_t>(voice.bus)];
        const bool sends = reverbOn && voice.positional
            && (voice.bus == Bus::Effects || voice.bus == Bus::Ambient || voice.bus == Bus::Voice);
        const float sendGain = sends
            ? impl_->send * impl_->busVolume[static_cast<std::size_t>(voice.bus)] * 0.5f
            : 0.0f;
        for (int frame = 0; frame < frames; ++frame) {
            if (voice.cursor >= static_cast<double>(sourceFrames)) {
                if (voice.loop) {
                    voice.cursor = std::fmod(voice.cursor, static_cast<double>(sourceFrames));
                } else {
                    voice.active = false;
                    break;
                }
            }
            const std::size_t lo = static_cast<std::size_t>(voice.cursor);
            const std::size_t hi = std::min(lo + 1, sourceFrames - 1);
            const float t = static_cast<float>(voice.cursor - static_cast<double>(lo));
            const float left = impl_->sample(data, lo, 0)
                + (impl_->sample(data, hi, 0) - impl_->sample(data, lo, 0)) * t;
            const float right = impl_->sample(data, lo, 1)
                + (impl_->sample(data, hi, 1) - impl_->sample(data, lo, 1)) * t;
            bus[static_cast<std::size_t>(frame) * 2u] += left * leftGain;
            bus[static_cast<std::size_t>(frame) * 2u + 1u] += right * rightGain;
            if (sends) {
                impl_->sendMono[static_cast<std::size_t>(frame)] +=
                    (left * leftGain + right * rightGain) * sendGain;
            }
            voice.cursor += static_cast<double>(voice.pitch);
        }
    }

    // Environment reverb: wet only, added after the dry buses below.
    impl_->renderReverb(frames);

    if (impl_->voices.size() > 64) {
        impl_->voices.erase(
            std::remove_if(impl_->voices.begin(), impl_->voices.end(),
                           [](const Impl::Voice& voice) { return !voice.active; }),
            impl_->voices.end());
    }

    double sumSquares = 0.0;
    for (int frame = 0; frame < frames; ++frame) {
        float left = 0.0f;
        float right = 0.0f;
        for (int bus = 0; bus < 5; ++bus) {
            const float volume = impl_->busVolume[static_cast<std::size_t>(bus)];
            const std::vector<float>& buffer = impl_->buses[static_cast<std::size_t>(bus)];
            left += buffer[static_cast<std::size_t>(frame) * 2u] * volume;
            right += buffer[static_cast<std::size_t>(frame) * 2u + 1u] * volume;
        }
        left += impl_->wet[static_cast<std::size_t>(frame) * 2u];
        right += impl_->wet[static_cast<std::size_t>(frame) * 2u + 1u];
        left = clampSample(left * impl_->master);
        right = clampSample(right * impl_->master);
        interleavedStereo[static_cast<std::size_t>(frame) * 2u] = left;
        interleavedStereo[static_cast<std::size_t>(frame) * 2u + 1u] = right;
        sumSquares += static_cast<double>(left) * left
            + static_cast<double>(right) * right;
    }
    impl_->lastRms = static_cast<float>(std::sqrt(sumSquares
                                                  / static_cast<double>(samples)));
}

std::size_t Mixer::activeVoices() const
{
    return static_cast<std::size_t>(std::count_if(
        impl_->voices.begin(), impl_->voices.end(),
        [](const Impl::Voice& voice) { return voice.active; }));
}

std::size_t Mixer::activeVoices(Bus bus) const
{
    return static_cast<std::size_t>(std::count_if(
        impl_->voices.begin(), impl_->voices.end(),
        [bus](const Impl::Voice& voice) { return voice.active && voice.bus == bus; }));
}

float Mixer::lastRms() const
{
    return impl_->lastRms;
}

float Mixer::voiceGain(int voice) const
{
    for (const Impl::Voice& entry : impl_->voices) {
        if (entry.id == voice) {
            return impl_->voiceGain(entry);
        }
    }
    return 0.0f;
}

float Mixer::voicePan(int voice) const
{
    for (const Impl::Voice& entry : impl_->voices) {
        if (entry.id == voice) {
            return impl_->voicePan(entry);
        }
    }
    return 0.0f;
}

} // namespace ke::audio
