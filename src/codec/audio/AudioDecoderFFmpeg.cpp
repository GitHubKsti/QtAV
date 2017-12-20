/******************************************************************************
    QtAV:  Multimedia framework based on Qt and FFmpeg
    Copyright (C) 2012-2016 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "QtAV/AudioDecoder.h"
#include "QtAV/AudioResampler.h"
#include "QtAV/Packet.h"
#include "QtAV/private/AVDecoder_p.h"
#include "QtAV/private/AVCompat.h"
#include "QtAV/private/mkid.h"
#include "QtAV/private/factory.h"
#include "QtAV/version.h"
#include "utils/Logger.h"
#include "AudioThread.h"
#include "codec/AVDecoderFFmpeg.h"

namespace QtAV {

class AudioDecoderFFmpegPrivate;
class AudioDecoderFFmpeg : public AudioDecoder, public AVDecoderFFmpeg
{
    Q_OBJECT
    Q_DISABLE_COPY(AudioDecoderFFmpeg)
    DPTR_DECLARE_PRIVATE(AudioDecoderFFmpeg)
    Q_PROPERTY(QString codecName READ codecName WRITE setCodecName NOTIFY codecNameChanged)
public:
    AudioDecoderFFmpeg();
    AudioDecoderId id() const Q_DECL_OVERRIDE Q_DECL_FINAL;
    virtual QString description() const Q_DECL_OVERRIDE Q_DECL_FINAL {
        const int patch = QTAV_VERSION_PATCH(avcodec_version());
        return QStringLiteral("%1 avcodec %2.%3.%4")
                .arg(patch>=100?QStringLiteral("FFmpeg"):QStringLiteral("Libav"))
                .arg(QTAV_VERSION_MAJOR(avcodec_version())).arg(QTAV_VERSION_MINOR(avcodec_version())).arg(patch);
    }
    bool decode(const Packet& packet) Q_DECL_OVERRIDE Q_DECL_FINAL;
    AudioFrame frame() Q_DECL_OVERRIDE Q_DECL_FINAL;
    void processFrame(AVFrame* frame, int currentDecodeReturnValue, AVPacket* packet) Q_DECL_OVERRIDE;
    AudioFrame frame(AVFrame* frame, AudioResampler* audioSampler);
Q_SIGNALS:
    void codecNameChanged() Q_DECL_OVERRIDE Q_DECL_FINAL;
    void newFrameDecoded(AudioFrame frame, AVPacket& packet);
};

AudioDecoderId AudioDecoderId_FFmpeg = mkid::id32base36_6<'F','F','m','p','e','g'>::value;
FACTORY_REGISTER(AudioDecoder, FFmpeg, "FFmpeg")

class AudioDecoderFFmpegPrivate Q_DECL_FINAL: public AudioDecoderPrivate
{
public:
    AudioDecoderFFmpegPrivate()
        : AudioDecoderPrivate()
        , frame(av_frame_alloc())
    {
        avcodec_register_all();
    }
    ~AudioDecoderFFmpegPrivate() {
        if (frame) {
            av_frame_free(&frame);
            frame = 0;
        }
    }

    AVFrame *frame; //set once and not change
};

AudioDecoderId AudioDecoderFFmpeg::id() const
{
    return AudioDecoderId_FFmpeg;
}

AudioDecoderFFmpeg::AudioDecoderFFmpeg()
    : AudioDecoder(*new AudioDecoderFFmpegPrivate()), AVDecoderFFmpeg()
{
}

//returns the number of decoded bytes
//int AudioDecoderFFmpeg::decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
//{
//    DPTR_D(AudioDecoderFFmpeg);

//    //do this to buffer the last frames
//    int ret;

//    *got_frame = 0;
//    bool performSendPacket = true;
//    AVFrame* internalFrame = av_frame_alloc();

//    for( int ret = 0; ret == 0; )
//    {
//        if (pkt && performSendPacket) {
//            ret = avcodec_send_packet(avctx, pkt);
//            if( ret != 0 && ret != AVERROR(EAGAIN) )
//            {
//                if (ret == AVERROR(ENOMEM) || ret == AVERROR(EINVAL))
//                {
//                    qInfo("avcodec_send_packet critical error");
//                }
//                return 0;
//            }
//            else
//            {
//                performSendPacket = false;
//            }
//        }
//        ret = avcodec_receive_frame(avctx, frame);
//        if( ret != 0 && ret != AVERROR(EAGAIN) )
//        {
//            if (ret == AVERROR(ENOMEM) || ret == AVERROR(EINVAL))
//            {
//                qInfo("avcodec_receive_frame critical error");
//                *got_frame = 0;
//            }
//            //av_frame_free(&frame);
//            /* After draining, we need to reset decoder with a flush */
//            if( ret == AVERROR_EOF )
//                avcodec_flush_buffers( avctx );
//            return 0;
//        }
//        else if(ret != AVERROR(EAGAIN))
//        {
//            qInfo("unhandled decoding error");

//        }
//    }
//    *got_frame = 1;
//    return pkt->size;
//}

void AudioDecoderFFmpeg::processFrame(AVFrame* fmt, int currentDecodeReturnVal, AVPacket* packet)
{
    Q_UNUSED(currentDecodeReturnVal);
    DPTR_D(AudioDecoderFFmpeg);
    if(_listener)
    {
        AudioFrame audioFrame = frame(fmt, d.resampler);
        _listener->onNewFrameAvailable(audioFrame,*packet);
    }
}

bool AudioDecoderFFmpeg::decode(const Packet &packet)
{
    if (!isAvailable())
        return false;
    DPTR_D(AudioDecoderFFmpeg);
    d.decoded.clear();
    int ret = 0;
    if (packet.isEOF()) {
        AVPacket eofpkt;
        av_init_packet(&eofpkt);
        eofpkt.data = NULL;
        eofpkt.size = 0;
        ret = AVDecoderFFmpeg::decode(d.codec_ctx, &eofpkt);
    } else {
    // const AVPacket*: ffmpeg >= 1.0. no libav
        ret = AVDecoderFFmpeg::decode(d.codec_ctx, (AVPacket*)packet.asAVPacket());
    }
    if(ret < 0)
    {
        qInfo("wrong audio decoding");
        return true;
    }
    return true;
}

AudioFrame AudioDecoderFFmpeg::frame(AVFrame* frame, AudioResampler* audioSampler)
{
    DPTR_D(AudioDecoderFFmpeg);
    AudioFormat fmt;
    fmt.setSampleFormatFFmpeg(frame->format);
    fmt.setChannelLayoutFFmpeg(frame->channel_layout);
    fmt.setSampleRate(frame->sample_rate);

    if (!fmt.isValid()) {// need more data to decode to get a frame
        return AudioFrame();
    }
    AudioFrame f(fmt);
    //av_frame_get_pkt_duration ffmpeg
    f.setBits(frame->extended_data); // TODO: ref
    f.setBytesPerLine(frame->linesize[0], 0); // for correct alignment
    f.setSamplesPerChannel(frame->nb_samples);
    // TODO: ffplay check AVFrame.pts, pkt_pts, last_pts+nb_samples. move to AudioFrame::from(AVFrame*)
    f.setTimestamp((double)frame->pkt_pts/1000.0);
    f.setAudioResampler(audioSampler); // TODO: remove. it's not safe if frame is shared. use a pool or detach if ref >1
    return f;
}
AudioFrame AudioDecoderFFmpeg::frame()
{
    DPTR_D(AudioDecoderFFmpeg);

    return frame(d.frame, d.resampler);
}

} //namespace QtAV
#include "AudioDecoderFFmpeg.moc"
