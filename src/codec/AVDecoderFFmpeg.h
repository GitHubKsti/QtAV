#ifndef AVDECODERFFMPEG_H
#define AVDECODERFFMPEG_H

struct AVFrame;
struct AVPacket;
struct AVCodecContext;

namespace QtAV {
class Frame;
class AVDecoderFFmpeg
{
public:
    class AVDecoderFFmpegListener
    {
    public:
        virtual void onNewFrameAvailable(Frame& frame, AVPacket& packet) = 0;
    };

public:
    AVDecoderFFmpeg();
    virtual int decode(AVCodecContext *avctx, AVPacket *pkt);
    void setListener(AVDecoderFFmpeg::AVDecoderFFmpegListener* listener);
    virtual void processFrame(AVFrame* fmt, int currentDecodeReturnVal, AVPacket* packet) = 0;


protected:
    AVDecoderFFmpegListener* _listener;
};
}

#endif // AVDECODERFFMPEG_H
