// The Bink shim's decoders on FFmpeg's libavcodec (binkvideo, binkaudio_dct and
// binkaudio_rdft). Only libavcodec and libavutil are used: the shim reads the
// file layout itself (bink_file.cpp), so libavformat is not needed.
//
// The decoders are set up the way FFmpeg's own Bink demuxer sets them up
// (libavformat/bink.c): the video decoder takes the file's four-character code
// as its codec tag and the 32-bit video flags as extradata; an audio decoder
// takes the same code as extradata, the track's rate, and one or two channels.
#include "bink_decoder.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace kisak::cinematic
{
namespace
{
std::string AvError(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

void QuietFfmpegLog()
{
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

void WriteLe32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

bool SetExtradata(AVCodecContext *context, uint32_t value)
{
    context->extradata = (uint8_t *)av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!context->extradata)
        return false;
    WriteLe32(context->extradata, value);
    context->extradata_size = 4;
    return true;
}

// Owns a codec context, a reusable packet and frame, and a padded copy of the
// current packet (libavcodec reads up to AV_INPUT_BUFFER_PADDING_SIZE past it).
struct CodecState
{
    AVCodecContext *context = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    std::vector<uint8_t> padded;

    ~CodecState()
    {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
    }

    bool Allocate(AVCodecID id, std::string *error)
    {
        QuietFfmpegLog();
        const AVCodec *codec = avcodec_find_decoder(id);
        if (!codec)
        {
            *error = std::string("libavcodec has no ") + avcodec_get_name(id) + " decoder";
            return false;
        }
        context = avcodec_alloc_context3(codec);
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!context || !packet || !frame)
        {
            *error = "out of memory";
            return false;
        }
        context->thread_count = 1;
        return true;
    }

    bool Open(std::string *error)
    {
        const int result = avcodec_open2(context, context->codec, nullptr);
        if (result < 0)
        {
            *error = std::string("avcodec_open2(") + avcodec_get_name(context->codec_id) + "): " + AvError(result);
            return false;
        }
        return true;
    }

    void Point(const uint8_t *data, size_t size, bool keyframe)
    {
        padded.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (size)
            std::memcpy(padded.data(), data, size);
        std::memset(padded.data() + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        av_packet_unref(packet);
        packet->data = padded.data();
        packet->size = (int)size;
        packet->flags = keyframe ? AV_PKT_FLAG_KEY : 0;
    }
};

class FfmpegVideoDecoder final : public BinkVideoDecoder
{
  public:
    bool Init(const BinkFileInfo &info, std::string *error)
    {
        if (!state.Allocate(AV_CODEC_ID_BINKVIDEO, error))
            return false;
        state.context->codec_tag = info.fourcc;
        state.context->width = (int)info.width;
        state.context->height = (int)info.height;
        if (!SetExtradata(state.context, info.videoFlags))
        {
            *error = "out of memory";
            return false;
        }
        return state.Open(error);
    }

    bool Decode(const uint8_t *packet, size_t size, bool keyframe, BinkPicture *picture, std::string *error) override
    {
        state.Point(packet, size, keyframe);
        int result = avcodec_send_packet(state.context, state.packet);
        if (result < 0)
        {
            *error = "video packet refused: " + AvError(result);
            return false;
        }
        av_frame_unref(state.frame);
        result = avcodec_receive_frame(state.context, state.frame);
        if (result < 0)
        {
            *error = "no picture: " + AvError(result);
            return false;
        }
        const AVFrame *frame = state.frame;
        const bool alpha = frame->format == AV_PIX_FMT_YUVA420P;
        if (frame->format != AV_PIX_FMT_YUV420P && !alpha)
        {
            *error = std::string("unexpected pixel format ") +
                     std::to_string(frame->format);
            return false;
        }
        // libavcodec's plane 1 is U (Cb) and plane 2 is V (Cr).
        picture->y = frame->data[0];
        picture->cb = frame->data[1];
        picture->cr = frame->data[2];
        picture->a = alpha ? frame->data[3] : nullptr;
        picture->yPitch = frame->linesize[0];
        picture->cbPitch = frame->linesize[1];
        picture->crPitch = frame->linesize[2];
        picture->aPitch = alpha ? frame->linesize[3] : 0;
        picture->width = frame->width;
        picture->height = frame->height;
        return true;
    }

    void Reset() override { avcodec_flush_buffers(state.context); }

  private:
    CodecState state;
};

int16_t FloatToS16(float value)
{
    const float scaled = std::nearbyint(value * 32767.0f);
    return (int16_t)std::clamp(scaled, -32768.0f, 32767.0f);
}

class FfmpegAudioDecoder final : public BinkAudioDecoder
{
  public:
    bool Init(const BinkFileInfo &info, const BinkAudioTrack &track, std::string *error)
    {
        if (!state.Allocate(track.UsesDct() ? AV_CODEC_ID_BINKAUDIO_DCT : AV_CODEC_ID_BINKAUDIO_RDFT, error))
            return false;
        channels = track.Channels();
        state.context->sample_rate = (int)track.sampleRate;
        av_channel_layout_default(&state.context->ch_layout, channels);
        if (!SetExtradata(state.context, info.fourcc))
        {
            *error = "out of memory";
            return false;
        }
        if (!state.Open(error))
            return false;
        sampleRate = state.context->sample_rate;
        return true;
    }

    bool Decode(const uint8_t *packet, size_t size, std::vector<int16_t> *samples, std::string *error) override
    {
        state.Point(packet, size, true);
        for (;;)
        {
            const int result = avcodec_send_packet(state.context, state.packet);
            if (result == AVERROR(EAGAIN))
            {
                // The decoder still holds part of the last packet.
                if (!Drain(samples, error))
                    return false;
                continue;
            }
            if (result < 0)
            {
                *error = "audio packet refused: " + AvError(result);
                return false;
            }
            break;
        }
        return Drain(samples, error);
    }

    void Reset() override { avcodec_flush_buffers(state.context); }
    int Channels() const override { return channels; }
    int SampleRate() const override { return sampleRate; }

  private:
    bool Drain(std::vector<int16_t> *samples, std::string *error)
    {
        for (;;)
        {
            av_frame_unref(state.frame);
            const int result = avcodec_receive_frame(state.context, state.frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return true;
            if (result < 0)
            {
                *error = "audio decode: " + AvError(result);
                return false;
            }
            if (!Append(state.frame, samples, error))
                return false;
        }
    }

    bool Append(const AVFrame *frame, std::vector<int16_t> *samples, std::string *error) const
    {
        const int frameChannels = frame->ch_layout.nb_channels;
        if (frameChannels != channels)
        {
            *error = "audio channel count changed";
            return false;
        }
        const size_t count = (size_t)frame->nb_samples;
        const size_t base = samples->size();
        samples->resize(base + count * (size_t)channels);
        int16_t *out = samples->data() + base;
        switch (frame->format)
        {
        case AV_SAMPLE_FMT_FLTP:
            for (size_t i = 0; i < count; ++i)
                for (int c = 0; c < channels; ++c)
                    out[i * channels + c] = FloatToS16(((const float *)frame->extended_data[c])[i]);
            return true;
        case AV_SAMPLE_FMT_FLT:
            for (size_t i = 0; i < count * (size_t)channels; ++i)
                out[i] = FloatToS16(((const float *)frame->extended_data[0])[i]);
            return true;
        case AV_SAMPLE_FMT_S16P:
            for (size_t i = 0; i < count; ++i)
                for (int c = 0; c < channels; ++c)
                    out[i * channels + c] = ((const int16_t *)frame->extended_data[c])[i];
            return true;
        case AV_SAMPLE_FMT_S16:
            std::memcpy(out, frame->extended_data[0], count * (size_t)channels * sizeof(int16_t));
            return true;
        default:
            samples->resize(base);
            *error = std::string("unexpected sample format ") +
                     av_get_sample_fmt_name((AVSampleFormat)frame->format);
            return false;
        }
    }

    CodecState state;
    int channels = 0;
    int sampleRate = 0;
};

std::unique_ptr<BinkVideoDecoder> CreateVideo(const BinkFileInfo &info, std::string *error)
{
    auto decoder = std::make_unique<FfmpegVideoDecoder>();
    if (!decoder->Init(info, error))
        return nullptr;
    return decoder;
}

std::unique_ptr<BinkAudioDecoder> CreateAudio(const BinkFileInfo &info, const BinkAudioTrack &track,
                                              std::string *error)
{
    auto decoder = std::make_unique<FfmpegAudioDecoder>();
    if (!decoder->Init(info, track, error))
        return nullptr;
    return decoder;
}

const BinkDecoderBackend kFfmpegBackend = {"FFmpeg libavcodec", CreateVideo, CreateAudio};
} // namespace

const BinkDecoderBackend &BinkDefaultDecoderBackend()
{
    return kFfmpegBackend;
}
} // namespace kisak::cinematic
