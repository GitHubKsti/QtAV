#include "AVDecoderFFmpeg.h"

#include "QtAV/private/AVCompat.h"

#include "QtAV/Frame.h"

namespace QtAV
{
AVDecoderFFmpeg::AVDecoderFFmpeg() : _listener(nullptr)
{
}

int AVDecoderFFmpeg::decode(AVCodecContext *avctx, AVPacket *pkt)
{
    AVFrame *frame = av_frame_alloc();
    int ret;
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO ||
     avctx->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        ret = avcodec_send_packet(avctx, pkt);
        if( ret != 0 && ret != AVERROR(EAGAIN) )
        {
            if (ret == AVERROR(ENOMEM) || ret == AVERROR(EINVAL))
            {
                qInfo("avcodec_send_packet critical error");
            }
            avcodec_flush_buffers( avctx );
            qInfo("avcodec_send_packet error goto out");

            goto out;
        }
        // Again EAGAIN is not expected
    //    if (ret < 0)
    //        goto out;
        int count = 0;
        while (!ret) {
            ret = avcodec_receive_frame(avctx, frame);
            if (!ret)
            {
                count++;
                processFrame(frame, ret, pkt);
            }
            else
            {
                if(count == 0)
                {
                    qInfo("avcodec_receive_frame error: no packet decoded");
                    break;
                }
            }
            if(count>1)
            {
                qInfo("Two frames generated at once.");
            }
        }
    }
    else
    {
        av_frame_free(&frame);
        return -1;
    }

out:
    av_frame_free(&frame);
    if (ret == AVERROR(EAGAIN))
        return 0;
    return ret;
}

void AVDecoderFFmpeg::setListener(AVDecoderFFmpeg::AVDecoderFFmpegListener *listener)
{
    _listener = listener;
}
}
