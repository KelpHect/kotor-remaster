#include "kotor/BinkDecoder.h"

#include "kotor/Formats.h"

#include <fstream>

#if defined(KOTOR_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace ke::kotor {

namespace {

constexpr const char* kDecoderUnavailable =
    "Bink pixel decoder unavailable: rebuild with KOTOR_FFMPEG_ROOT";

bool readPrefix(const std::string& path, BikMovie& movie, std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = path + ": cannot open Bink stream";
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0x3c) {
        error = path + ": shorter than a Bink header";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(0x3c);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), 0x3c)) {
        error = path + ": short Bink header";
        return false;
    }
    const std::uint32_t frames = readU32(bytes.data(), bytes.size(), 8, "BIK frames");
    const std::size_t tableEnd = 0x38u + (static_cast<std::size_t>(frames) + 1u) * 4u;
    if (tableEnd > static_cast<std::size_t>(size)) {
        error = path + ": Bink frame table overruns the file";
        return false;
    }
    bytes.resize(tableEnd);
    file.clear();
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
        error = path + ": short Bink frame table";
        return false;
    }
    try {
        movie = parseBik(bytes, path, static_cast<std::uint64_t>(size));
    } catch (const FormatError& errorText) {
        error = errorText.what();
        return false;
    }
    return true;
}

} // namespace

struct BinkPlayer::Impl {
#if defined(KOTOR_HAVE_FFMPEG)
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int videoStream = -1;
#endif
    BikMovie movie;
    std::string path;
    int decoded = 0;

    ~Impl() { close(); }

    void close()
    {
#if defined(KOTOR_HAVE_FFMPEG)
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (codec != nullptr) {
            avcodec_free_context(&codec);
        }
        if (format != nullptr) {
            avformat_close_input(&format);
        }
#endif
        decoded = 0;
        path.clear();
    }

    bool open(const std::string& file, std::string& error)
    {
        close();
#if !defined(KOTOR_HAVE_FFMPEG)
        error = kDecoderUnavailable;
        (void)file;
        return false;
#else
        if (!readPrefix(file, movie, error)) {
            return false;
        }
        if (avformat_open_input(&format, file.c_str(), nullptr, nullptr) < 0) {
            error = file + ": FFmpeg could not open the Bink container";
            close();
            return false;
        }
        if (avformat_find_stream_info(format, nullptr) < 0) {
            error = file + ": FFmpeg could not read Bink stream info";
            close();
            return false;
        }
        const AVCodec* decoder = nullptr;
        videoStream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                          &decoder, 0);
        if (videoStream < 0 || decoder == nullptr) {
            error = file + ": no Bink video stream";
            close();
            return false;
        }
        codec = avcodec_alloc_context3(decoder);
        if (codec == nullptr
            || avcodec_parameters_to_context(codec, format->streams[videoStream]->codecpar) < 0
            || avcodec_open2(codec, decoder, nullptr) < 0) {
            error = file + ": Bink decoder init failed";
            close();
            return false;
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr) {
            error = file + ": FFmpeg frame allocation failed";
            close();
            return false;
        }
        path = file;
        decoded = 0;
        return true;
#endif
    }

#if defined(KOTOR_HAVE_FFMPEG)
    bool convert(const AVFrame* source, BinkVideoFrame& out, std::string& error)
    {
        SwsContext* sws = sws_getContext(
            source->width, source->height,
            static_cast<AVPixelFormat>(source->format),
            source->width, source->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws == nullptr) {
            error = "FFmpeg swscale context failed";
            return false;
        }
        out.width = source->width;
        out.height = source->height;
        out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4u);
        std::uint8_t* destination[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
        const int lines[4] = {out.width * 4, 0, 0, 0};
        const int scaled = sws_scale(sws, source->data, source->linesize, 0,
                                     source->height, destination, lines);
        sws_freeContext(sws);
        if (scaled <= 0) {
            error = "FFmpeg swscale produced no rows";
            return false;
        }
        return true;
    }

    bool emit(const AVFrame* source, int wanted, BinkVideoFrame& out,
              bool& matched, std::string& error)
    {
        BinkVideoFrame converted;
        if (!convert(source, converted, error)) {
            return false;
        }
        if (decoded == wanted) {
            out = std::move(converted);
            matched = true;
        }
        ++decoded;
        return true;
    }

    bool decode(int wanted, BinkVideoFrame& out, std::string& error)
    {
        if (format == nullptr || codec == nullptr) {
            error = "Bink stream is not open";
            return false;
        }
        if (wanted < 0 || wanted >= static_cast<int>(movie.frames)) {
            error = "Bink frame index " + std::to_string(wanted) + " out of range";
            return false;
        }
        if (wanted < decoded) {
            const std::string file = path;
            if (!open(file, error)) {
                return false;
            }
        }
        bool matched = false;
        while (!matched) {
            const int read = av_read_frame(format, packet);
            if (read < 0) {
                // Flush delayed frames before declaring EOF.
                avcodec_send_packet(codec, nullptr);
                while (true) {
                    const int receive = avcodec_receive_frame(codec, frame);
                    if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) {
                        break;
                    }
                    if (receive < 0) {
                        error = "FFmpeg Bink decode failed while draining";
                        return false;
                    }
                    if (!emit(frame, wanted, out, matched, error)) {
                        return false;
                    }
                    av_frame_unref(frame);
                }
                if (!matched) {
                    error = "Bink stream ended before frame "
                        + std::to_string(wanted);
                }
                return matched;
            }
            if (packet->stream_index != videoStream) {
                av_packet_unref(packet);
                continue;
            }
            const int sent = avcodec_send_packet(codec, packet);
            av_packet_unref(packet);
            if (sent < 0) {
                error = "FFmpeg Bink send_packet failed";
                return false;
            }
            while (true) {
                const int receive = avcodec_receive_frame(codec, frame);
                if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) {
                    break;
                }
                if (receive < 0) {
                    error = "FFmpeg Bink decode failed";
                    return false;
                }
                if (!emit(frame, wanted, out, matched, error)) {
                    return false;
                }
                av_frame_unref(frame);
                if (matched) {
                    break;
                }
            }
        }
        return true;
    }
#endif
};

BinkPlayer::BinkPlayer()
    : impl_(std::make_unique<Impl>())
{
}

BinkPlayer::~BinkPlayer() = default;

bool BinkPlayer::available()
{
#if defined(KOTOR_HAVE_FFMPEG)
    return true;
#else
    return false;
#endif
}

bool BinkPlayer::open(const std::string& path, std::string& error)
{
    return impl_->open(path, error);
}

void BinkPlayer::close()
{
    impl_->close();
}

bool BinkPlayer::isOpen() const
{
    return !impl_->path.empty();
}

int BinkPlayer::width() const
{
    return static_cast<int>(impl_->movie.width);
}

int BinkPlayer::height() const
{
    return static_cast<int>(impl_->movie.height);
}

int BinkPlayer::frameCount() const
{
    return static_cast<int>(impl_->movie.frames);
}

double BinkPlayer::framesPerSecond() const
{
    return impl_->movie.framesPerSecond > 0.0f
        ? static_cast<double>(impl_->movie.framesPerSecond) : 30.0;
}

bool BinkPlayer::decodeFrame(int index, BinkVideoFrame& out, std::string& error)
{
#if defined(KOTOR_HAVE_FFMPEG)
    return impl_->decode(index, out, error);
#else
    (void)index;
    (void)out;
    error = kDecoderUnavailable;
    return false;
#endif
}

int BinkPlayer::decodedFrames() const
{
    return impl_->decoded;
}

} // namespace ke::kotor
