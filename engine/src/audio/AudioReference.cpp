#include "audio/AudioReference.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>

#if defined(KOTOR_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}
#endif

namespace ke::audio {

namespace {

std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t o)
{
    if (o + 4 > b.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
        | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

bool isRiffWave(const std::vector<std::uint8_t>& b, std::size_t o)
{
    return o + 12 <= b.size() && std::memcmp(b.data() + o, "RIFF", 4) == 0
        && std::memcmp(b.data() + o + 8, "WAVE", 4) == 0;
}

bool isMp3Sync(const std::vector<std::uint8_t>& b, std::size_t o)
{
    return o + 2 <= b.size() && b[o] == 0xFF && (b[o + 1] & 0xE0) == 0xE0;
}

std::size_t skipId3(const std::vector<std::uint8_t>& b, std::size_t o)
{
    if (o + 10 <= b.size() && std::memcmp(b.data() + o, "ID3", 3) == 0) {
        const std::size_t size = (static_cast<std::size_t>(b[o + 6] & 0x7F) << 21)
            | (static_cast<std::size_t>(b[o + 7] & 0x7F) << 14)
            | (static_cast<std::size_t>(b[o + 8] & 0x7F) << 7) | static_cast<std::size_t>(b[o + 9] & 0x7F);
        return o + 10 + size;
    }
    return o;
}

} // namespace

AudioLayout locateAudio(const std::vector<std::uint8_t>& bytes)
{
    AudioLayout layout;
    if (isRiffWave(bytes, 0)) {
        // Walk the chunks: an empty `data` chunk means MP3 frames follow the header.
        for (std::size_t o = 12; o + 8 <= bytes.size();) {
            const std::uint32_t size = le32(bytes, o + 4);
            if (std::memcmp(bytes.data() + o, "data", 4) == 0) {
                if (size == 0) {
                    const std::size_t start = skipId3(bytes, o + 8);
                    if (isMp3Sync(bytes, start)) {
                        layout = {true, start, true, "riff-empty-data+mp3"};
                        return layout;
                    }
                }
                break;
            }
            o += 8 + static_cast<std::size_t>(size) + (size & 1u);
        }
        layout = {true, 0, false, "riff@0"};
        return layout;
    }
    if (bytes.size() > 482 && isRiffWave(bytes, 470)) {
        layout = {true, 470, false, "riff@470"};
        return layout;
    }
    if (bytes.size() > 4 && std::memcmp(bytes.data(), "BMU ", 4) == 0) {
        layout = {true, 4, true, "bmu"};
        return layout;
    }
    const std::size_t start = skipId3(bytes, 0);
    if (isMp3Sync(bytes, start)) {
        layout = {true, start, true, "mp3"};
    }
    return layout;
}

bool referenceAvailable()
{
#if defined(KOTOR_HAVE_FFMPEG)
    return true;
#else
    return false;
#endif
}

#if defined(KOTOR_HAVE_FFMPEG)
namespace {

struct MemoryCursor {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t position = 0;
};

int readPacket(void* opaque, std::uint8_t* buffer, int bufferSize)
{
    MemoryCursor* cursor = static_cast<MemoryCursor*>(opaque);
    const std::size_t left = cursor->size - cursor->position;
    if (left == 0) {
        return AVERROR_EOF;
    }
    const std::size_t count = std::min<std::size_t>(left, static_cast<std::size_t>(bufferSize));
    std::memcpy(buffer, cursor->data + cursor->position, count);
    cursor->position += count;
    return static_cast<int>(count);
}

std::int64_t seekPacket(void* opaque, std::int64_t offset, int whence)
{
    MemoryCursor* cursor = static_cast<MemoryCursor*>(opaque);
    if (whence == AVSEEK_SIZE) {
        return static_cast<std::int64_t>(cursor->size);
    }
    std::int64_t target = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: target = offset; break;
    case SEEK_CUR: target = static_cast<std::int64_t>(cursor->position) + offset; break;
    case SEEK_END: target = static_cast<std::int64_t>(cursor->size) + offset; break;
    default: return -1;
    }
    if (target < 0 || target > static_cast<std::int64_t>(cursor->size)) {
        return -1;
    }
    cursor->position = static_cast<std::size_t>(target);
    return target;
}

float frameSample(const AVFrame* frame, int sample, int channel, int channels)
{
    switch (frame->format) {
    case AV_SAMPLE_FMT_FLTP:
        return reinterpret_cast<const float*>(frame->extended_data[channel])[sample];
    case AV_SAMPLE_FMT_FLT:
        return reinterpret_cast<const float*>(frame->data[0])[sample * channels + channel];
    case AV_SAMPLE_FMT_S16P:
        return reinterpret_cast<const std::int16_t*>(frame->extended_data[channel])[sample] / 32768.0f;
    case AV_SAMPLE_FMT_S16:
        return reinterpret_cast<const std::int16_t*>(frame->data[0])[sample * channels + channel] / 32768.0f;
    case AV_SAMPLE_FMT_U8:
        return (static_cast<float>(frame->data[0][sample * channels + channel]) - 128.0f) / 128.0f;
    case AV_SAMPLE_FMT_U8P:
        return (static_cast<float>(frame->extended_data[channel][sample]) - 128.0f) / 128.0f;
    case AV_SAMPLE_FMT_S32:
        return static_cast<float>(reinterpret_cast<const std::int32_t*>(frame->data[0])[sample * channels + channel])
            / 2147483648.0f;
    case AV_SAMPLE_FMT_S32P:
        return static_cast<float>(reinterpret_cast<const std::int32_t*>(frame->extended_data[channel])[sample])
            / 2147483648.0f;
    default:
        return 0.0f;
    }
}

} // namespace
#endif

bool referenceDecode(const std::vector<std::uint8_t>& bytes, SoundData& out, std::string& error,
                     AudioLayout* layoutOut)
{
#if !defined(KOTOR_HAVE_FFMPEG)
    (void)bytes;
    (void)out;
    (void)layoutOut;
    error = "FFmpeg is not built in";
    return false;
#else
    const AudioLayout layout = locateAudio(bytes);
    if (layoutOut != nullptr) {
        *layoutOut = layout;
    }
    if (!layout.valid) {
        error = "unrecognised audio layout";
        return false;
    }
    MemoryCursor cursor{bytes.data() + layout.offset, bytes.size() - layout.offset, 0};
    constexpr int kIoBuffer = 32768;
    unsigned char* ioBuffer = static_cast<unsigned char*>(av_malloc(kIoBuffer));
    AVIOContext* io = avio_alloc_context(ioBuffer, kIoBuffer, 0, &cursor, &readPacket, nullptr, &seekPacket);
    AVFormatContext* format = avformat_alloc_context();
    if (ioBuffer == nullptr || io == nullptr || format == nullptr) {
        error = "FFmpeg allocation failed";
        if (io != nullptr) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        } else if (ioBuffer != nullptr) {
            av_free(ioBuffer);
        }
        if (format != nullptr) {
            avformat_free_context(format);
        }
        return false;
    }
    format->pb = io;
    const AVInputFormat* input = av_find_input_format(layout.mp3 ? "mp3" : "wav");
    bool ok = false;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> samples;
    int sampleRate = 0;
    int channels = 0;
    do {
        if (avformat_open_input(&format, nullptr, input, nullptr) < 0) {
            error = "FFmpeg could not open the stream";
            break;
        }
        if (avformat_find_stream_info(format, nullptr) < 0) {
            error = "FFmpeg found no stream info";
            break;
        }
        const AVCodec* decoder = nullptr;
        const int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
        if (stream < 0 || decoder == nullptr) {
            error = "FFmpeg found no audio stream";
            break;
        }
        codec = avcodec_alloc_context3(decoder);
        if (codec == nullptr || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0
            || avcodec_open2(codec, decoder, nullptr) < 0) {
            error = "FFmpeg could not open the decoder";
            break;
        }
        const auto drain = [&]() {
            while (avcodec_receive_frame(codec, frame) >= 0) {
                const int frameChannels = frame->ch_layout.nb_channels;
                if (sampleRate == 0) {
                    sampleRate = frame->sample_rate;
                    channels = frameChannels;
                }
                if (frameChannels == channels && frame->sample_rate == sampleRate) {
                    const std::size_t base = samples.size();
                    samples.resize(base + static_cast<std::size_t>(frame->nb_samples) * static_cast<std::size_t>(channels));
                    for (int i = 0; i < frame->nb_samples; ++i) {
                        for (int c = 0; c < channels; ++c) {
                            samples[base + static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)
                                    + static_cast<std::size_t>(c)] = frameSample(frame, i, c, channels);
                        }
                    }
                }
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(format, packet) >= 0) {
            if (packet->stream_index == stream && avcodec_send_packet(codec, packet) == 0) {
                drain();
            }
            av_packet_unref(packet);
        }
        avcodec_send_packet(codec, nullptr);
        drain();
        ok = !samples.empty() && sampleRate > 0 && channels > 0;
        if (!ok) {
            error = "FFmpeg produced no samples";
        }
    } while (false);
    av_frame_free(&frame);
    av_packet_free(&packet);
    if (codec != nullptr) {
        avcodec_free_context(&codec);
    }
    if (format != nullptr) {
        avformat_close_input(&format);
    }
    if (io != nullptr) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
    if (!ok) {
        return false;
    }
    out.name = "reference";
    out.sampleRate = sampleRate;
    out.channels = channels;
    out.samples = std::move(samples);
    return true;
#endif
}

std::vector<float> mono(const SoundData& data)
{
    std::vector<float> out;
    if (data.channels <= 0) {
        return out;
    }
    const std::size_t frames = data.frameCount();
    out.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (int c = 0; c < data.channels; ++c) {
            sum += data.samples[i * static_cast<std::size_t>(data.channels) + static_cast<std::size_t>(c)];
        }
        out[i] = sum / static_cast<float>(data.channels);
    }
    return out;
}

namespace {

double dotAt(const std::vector<float>& a, const std::vector<float>& b, long lag, std::size_t start,
             std::size_t count, double& energyA, double& energyB)
{
    double dot = 0.0;
    energyA = 0.0;
    energyB = 0.0;
    for (std::size_t i = start; i < start + count; ++i) {
        const long j = static_cast<long>(i) + lag;
        if (i >= a.size() || j < 0 || j >= static_cast<long>(b.size())) {
            continue;
        }
        const double x = a[i];
        const double y = b[static_cast<std::size_t>(j)];
        dot += x * y;
        energyA += x * x;
        energyB += y * y;
    }
    return dot;
}

} // namespace

Correlation correlate(const std::vector<float>& a, const std::vector<float>& b, int maxLag,
                      std::size_t limit)
{
    Correlation result;
    if (a.empty() || b.empty()) {
        return result;
    }
    // Loudest 8192-sample window of `a` (by 1/16-window hop) picks where to search for the lag.
    constexpr std::size_t kWindow = 8192;
    const std::size_t window = std::min(kWindow, a.size());
    std::size_t bestStart = 0;
    double bestEnergy = -1.0;
    for (std::size_t start = 0; start + window <= a.size(); start += window / 4) {
        double energy = 0.0;
        for (std::size_t i = start; i < start + window; ++i) {
            energy += static_cast<double>(a[i]) * a[i];
        }
        if (energy > bestEnergy) {
            bestEnergy = energy;
            bestStart = start;
        }
    }
    long bestLag = 0;
    double bestScore = -2.0;
    for (long lag = -maxLag; lag <= maxLag; ++lag) {
        double ea = 0.0;
        double eb = 0.0;
        const double dot = dotAt(a, b, lag, bestStart, window, ea, eb);
        const double score = ea > 0.0 && eb > 0.0 ? dot / std::sqrt(ea * eb) : 0.0;
        if (score > bestScore) {
            bestScore = score;
            bestLag = lag;
        }
    }
    double ea = 0.0;
    double eb = 0.0;
    const std::size_t count = std::min(a.size(), limit);
    const double dot = dotAt(a, b, bestLag, 0, count, ea, eb);
    result.lag = static_cast<int>(bestLag);
    result.compared = count;
    result.value = ea > 0.0 && eb > 0.0 ? dot / std::sqrt(ea * eb) : 0.0;
    return result;
}

namespace {

void fft(std::vector<std::complex<double>>& x)
{
    const std::size_t n = x.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(x[i], x[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = x[i + k];
                const std::complex<double> v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

} // namespace

double spectralFlatness(const std::vector<float>& signal, int sampleRate, double lowHz, double highHz)
{
    constexpr std::size_t kWindow = 2048;
    if (signal.size() < kWindow || sampleRate <= 0) {
        return 0.0;
    }
    const double binHz = static_cast<double>(sampleRate) / static_cast<double>(kWindow);
    const std::size_t lo = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(lowHz / binHz)));
    const std::size_t hi = std::min<std::size_t>(kWindow / 2 - 1, static_cast<std::size_t>(highHz / binHz));
    if (hi <= lo) {
        return 0.0;
    }
    double total = 0.0;
    int windows = 0;
    // At most 64 windows spread across the signal keep the cost bounded for long music.
    const std::size_t available = (signal.size() - kWindow) / kWindow + 1;
    const std::size_t stride = std::max<std::size_t>(1, available / 64);
    std::vector<std::complex<double>> buffer(kWindow);
    for (std::size_t w = 0; w < available; w += stride) {
        const std::size_t start = w * kWindow;
        double energy = 0.0;
        for (std::size_t i = 0; i < kWindow; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i)
                                                     / static_cast<double>(kWindow - 1));
            const double v = static_cast<double>(signal[start + i]) * hann;
            buffer[i] = {v, 0.0};
            energy += v * v;
        }
        if (energy < 1.0e-9) {
            continue; // silence has no defined flatness
        }
        fft(buffer);
        double logSum = 0.0;
        double sum = 0.0;
        for (std::size_t k = lo; k <= hi; ++k) {
            const double power = std::norm(buffer[k]) + 1.0e-18;
            logSum += std::log(power);
            sum += power;
        }
        const double count = static_cast<double>(hi - lo + 1);
        total += std::exp(logSum / count) / (sum / count);
        ++windows;
    }
    return windows > 0 ? total / windows : 0.0;
}

StepStats stepStats(const std::vector<float>& stereo, float threshold)
{
    StepStats stats;
    stats.samples = stereo.size();
    for (std::size_t i = 0; i < stereo.size(); ++i) {
        const float v = stereo[i];
        if (!std::isfinite(v)) {
            ++stats.nonFinite;
            continue;
        }
        if (std::fabs(v) >= 1.0f) {
            ++stats.clipped;
        }
        if (i >= 2 && std::isfinite(stereo[i - 2])) {
            const float step = std::fabs(v - stereo[i - 2]);
            stats.maxStep = std::max(stats.maxStep, step);
            if (step > threshold) {
                ++stats.over;
            }
        }
    }
    return stats;
}

bool writeWav16(const std::string& path, const std::vector<float>& interleaved, int channels,
                int sampleRate)
{
    std::ofstream file(path, std::ios::binary);
    if (!file || channels <= 0 || sampleRate <= 0) {
        return false;
    }
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(interleaved.size() * 2u);
    const auto put32 = [&](std::uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [&](std::uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
    file.write("RIFF", 4);
    put32(36u + dataBytes);
    file.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(static_cast<std::uint16_t>(channels));
    put32(static_cast<std::uint32_t>(sampleRate));
    put32(static_cast<std::uint32_t>(sampleRate * channels * 2));
    put16(static_cast<std::uint16_t>(channels * 2));
    put16(16);
    file.write("data", 4);
    put32(dataBytes);
    std::vector<std::int16_t> pcm(interleaved.size());
    for (std::size_t i = 0; i < interleaved.size(); ++i) {
        const float v = std::isfinite(interleaved[i]) ? std::clamp(interleaved[i], -1.0f, 1.0f) : 0.0f;
        pcm[i] = static_cast<std::int16_t>(std::lround(v * 32767.0f));
    }
    file.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    return file.good();
}

} // namespace ke::audio
