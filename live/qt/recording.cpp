#include "recording.h"
#include <windows.h>
#include <chrono>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
namespace live {
static void check(int result, const char *operation) {
    if (result >= 0)
        return;
    char detail[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, detail, sizeof(detail));
    throw std::runtime_error(std::string(operation) + ": " + detail);
}
struct Track {
    AVStream *stream = nullptr;
    AVCodecContext *codec = nullptr;
    AVFrame *frame = nullptr;
    ~Track() {
        av_frame_free(&frame);
        avcodec_free_context(&codec);
    }
};
class Muxer {
  public:
    AVFormatContext *ctx = nullptr;
    SwsContext *convert = nullptr;
    AVPacket *packet = nullptr;
    Track video, audio;
    qint64 expiry = 0;
    ~Muxer() {
        if (ctx && ctx->pb && !(ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ctx->pb);
        avformat_free_context(ctx);
        sws_freeContext(convert);
        av_packet_free(&packet);
    }
    static int interrupt(void *arg) {
        auto self = static_cast<Muxer *>(arg);
        return self->expiry && clockUs() > self->expiry;
    }
    void open(const QString &dst, bool rtsp, int w, int h, int fps) {
        auto url = dst.toUtf8();
        check(avformat_alloc_output_context2(&ctx, nullptr, rtsp ? "rtsp" : "mp4", url.constData()), "output");
        ctx->interrupt_callback = {interrupt, this};
        for (auto *t : {&video, &audio}) {
            bool pic = t == &video;
            auto encoder =
                pic ? avcodec_find_encoder_by_name("libx264") : avcodec_find_encoder(AV_CODEC_ID_AAC);
            if (!encoder)
                throw std::runtime_error(pic ? "Missing H.264 encoder" : "Missing AAC encoder");
            t->stream = avformat_new_stream(ctx, nullptr);
            t->codec = avcodec_alloc_context3(encoder);
            t->frame = av_frame_alloc();
            if (!t->stream || !t->codec || !t->frame)
                throw std::bad_alloc();
            auto c = t->codec;
            auto f = t->frame;
            if (pic) {
                c->width = w;
                c->height = h;
                c->pix_fmt = AV_PIX_FMT_YUV420P;
                c->time_base = {1, fps};
                c->framerate = {fps, 1};
                c->gop_size = fps * 2;
                c->max_b_frames = 0;
                c->thread_count = 2;
                av_opt_set(c->priv_data, "preset", "veryfast", 0);
                av_opt_set(c->priv_data, "tune", "zerolatency", 0);
                av_opt_set(c->priv_data, "crf", "18", 0);
                f->width = w;
                f->height = h;
                f->format = c->pix_fmt;
            } else {
                c->sample_rate = 48000;
                c->sample_fmt = AV_SAMPLE_FMT_FLTP;
                c->time_base = {1, 48000};
                c->bit_rate = 160000;
                av_channel_layout_default(&c->ch_layout, 2);
            }
            if (ctx->oformat->flags & AVFMT_GLOBALHEADER)
                c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            check(avcodec_open2(c, encoder, nullptr), "encoder");
            t->stream->time_base = c->time_base;
            check(avcodec_parameters_from_context(t->stream->codecpar, c), "stream parameters");
            if (!pic) {
                f->format = c->sample_fmt;
                f->nb_samples = c->frame_size;
                f->sample_rate = c->sample_rate;
                check(av_channel_layout_copy(&f->ch_layout, &c->ch_layout), "audio layout");
            }
            check(av_frame_get_buffer(f, 32), "frame buffer");
        }
        expiry = clockUs() + 4000000;
        if (!(ctx->oformat->flags & AVFMT_NOFILE))
            check(avio_open2(&ctx->pb, url, AVIO_FLAG_WRITE, &ctx->interrupt_callback, nullptr), "open file");
        AVDictionary *options = nullptr;
        if (rtsp)
            av_dict_set(&options, "rtsp_transport", "tcp", 0);
        else
            av_dict_set(&options, "movflags", "+faststart", 0);
        int ret = avformat_write_header(ctx, &options);
        av_dict_free(&options);
        check(ret, "header");
        expiry = 0;
        convert = sws_getContext(w, h, AV_PIX_FMT_RGB24, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                                 nullptr, nullptr);
        packet = av_packet_alloc();
        if (!convert || !packet)
            throw std::bad_alloc();
    }
    void encode(Track &track, AVFrame *frame) {
        check(avcodec_send_frame(track.codec, frame), "send frame");
        while (true) {
            int ret = avcodec_receive_packet(track.codec, packet);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                return;
            check(ret, "encode");
            if (!packet->duration)
                packet->duration = &track == &video ? 1 : audio.codec->frame_size;
            av_packet_rescale_ts(packet, track.codec->time_base, track.stream->time_base);
            packet->stream_index = track.stream->index;
            expiry = clockUs() + 4000000;
            ret = av_interleaved_write_frame(ctx, packet);
            expiry = 0;
            av_packet_unref(packet);
            check(ret, "write packet");
        }
    }
    void addVideo(const QImage &im, qint64 index) {
        auto f = video.frame;
        check(av_frame_make_writable(f), "writable video");
        const uint8_t *data[] = {im.constBits()};
        int strides[] = {int(im.bytesPerLine())};
        if (sws_scale(convert, data, strides, 0, im.height(), f->data, f->linesize) != im.height())
            throw std::runtime_error("RGB conversion failed");
        f->pts = index;
        encode(video, f);
    }
    void addAudio(const QByteArray &pcm, qint64 index) {
        auto f = audio.frame;
        check(av_frame_make_writable(f), "writable audio");
        auto in = reinterpret_cast<const qint16 *>(pcm.constData());
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < f->nb_samples; ++i)
                reinterpret_cast<float *>(f->data[c])[i] = in[i * 2 + c] / 32768.f;
        f->pts = index;
        encode(audio, f);
    }
    void finish() {
        encode(video, nullptr);
        encode(audio, nullptr);
        expiry = clockUs() + 4000000;
        check(av_write_trailer(ctx), "finalize");
        if (ctx->pb && !(ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_flush(ctx->pb);
            check(ctx->pb->error, "flush output");
            check(avio_closep(&ctx->pb), "close output");
        }
    }
};
Recording::Recording(Timeline &t, QString p, int s, int hz, bool replace)
    : timeline(t), path(std::move(p)), scale(s), fps(hz), allowReplace(replace) {
    worker = std::thread(&Recording::run, this);
}
Recording::~Recording() {
    stop();
    worker.join();
}
void Recording::stop() {
    qint64 expected = 0;
    stopTime.compare_exchange_strong(expected, clockUs());
}
QJsonObject Recording::stats() const {
    std::lock_guard lock(mutex);
    QString value = stopTime && state == "recording" ? "saving" : state;
    auto start = startTime.load(), end = stopTime.load();
    return {{"state", value},
            {"file", path},
            {"error", error},
            {"seconds", start ? std::max<qint64>(0, (end ? end : clockUs()) - start) / 1000000 : 0}};
}
void Recording::run() {
    bool rtsp = path.startsWith("rtsp://", Qt::CaseInsensitive);
    QString staging;
    try {
        if (!rtsp) {
            QTemporaryFile tmp(QFileInfo(path).absoluteDir().filePath(".3ds-live-XXXXXX.mp4"));
            if (!tmp.open())
                throw std::runtime_error(tmp.errorString().toStdString());
            tmp.setAutoRemove(false);
            staging = tmp.fileName();
        }
        Muxer out;
        out.open(rtsp ? path : staging, rtsp, 400 * scale, 480 * scale, fps);
        {
            std::lock_guard lock(mutex);
            state = "recording";
        }
        qint64 origin = clockUs(), v = 0, a = 0;
        startTime = origin;
        QImage black(400 * scale, 480 * scale, QImage::Format_RGB888);
        black.fill(Qt::black);
        while (true) {
            const auto end = stopTime.load();
            const qint64 frameStart = origin + v * 1000000 / fps;
            const qint64 at = origin + a * 1000000 / 48000;
            if (end && frameStart >= end && at >= end)
                break;
            // A CFR image represents an entire output interval. Sample its midpoint so that
            // a game transition is quantized to the nearest MP4 boundary on the audio clock.
            qint64 vt = origin + (2 * v + 1) * 500000 / fps;
            if (end && frameStart < end)
                vt = std::min(vt, end - 1);
            const qint64 when = std::min(vt, at);
            qint64 remaining = when - clockUs();
            if (remaining > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(std::min<qint64>(remaining, 5000)));
                continue;
            }
            if (remaining < -1000000)
                throw std::runtime_error("Encoding fell more than one second behind capture");
            if (vt <= at) {
                auto frame = timeline.recorded(vt);
                auto input = frame.image.isNull() ? black : frame.image;
                if (input.size() != black.size())
                    input = input.scaled(black.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                out.addVideo(input, v++);
                timeline.diagnostics.add({rtsp ? "publish" : "encode", frame.source, clockUs(), vt, frame.id,
                                          frame.image.cacheKey(), frame.quality});
            } else {
                int n = out.audio.frame->nb_samples;
                out.addAudio(timeline.pcm(at, n), a);
                a += n;
            }
        }
        {
            std::lock_guard lock(mutex);
            state = "saving";
        }
        out.finish();
        if (!rtsp &&
            !MoveFileExW(reinterpret_cast<LPCWSTR>(staging.utf16()), reinterpret_cast<LPCWSTR>(path.utf16()),
                         (allowReplace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error(
                QString("Could not replace destination (Windows error %1)").arg(GetLastError()).toStdString());
        std::lock_guard lock(mutex);
        state = "finished";
    } catch (const std::exception &e) {
        if (!staging.isEmpty())
            QFile::remove(staging);
        std::lock_guard lock(mutex);
        state = "failed";
        error = QString::fromUtf8(e.what());
    }
}
} // namespace live
