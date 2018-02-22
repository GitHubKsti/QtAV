/******************************************************************************
    QtAV:  Multimedia framework based on Qt and FFmpeg
    Copyright (C) 2012-2017 Wang Bin <wbsecg1@gmail.com>

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

#include "VideoThread.h"
#include "AVThread_p.h"
#include "QtAV/Packet.h"
#include "QtAV/AVClock.h"
#include "QtAV/VideoCapture.h"
#include "QtAV/VideoDecoder.h"
#include "QtAV/VideoRenderer.h"
#include "QtAV/Statistics.h"
#include "QtAV/Filter.h"
#include "QtAV/FilterContext.h"
#include "output/OutputSet.h"
#include "QtAV/private/AVCompat.h"
#include <QtCore/QFileInfo>
#include "utils/Logger.h"

namespace QtAV {

class VideoThreadPrivate : public AVThreadPrivate
{
public:
    VideoThreadPrivate():
        AVThreadPrivate()
      , force_fps(0)
      , force_dt(0)
      , capture(0)
      , filter_context(0)
    {
    }
    ~VideoThreadPrivate() {
        //not neccesary context is managed by filters.
        if (filter_context) {
            delete filter_context;
            filter_context = 0;
        }
    }

    VideoFrameConverter conv;
    qreal force_fps; // <=0: try to use pts. if no pts in stream(guessed by 5 packets), use |force_fps|
    // not const.
    int force_dt; //unit: ms. force_fps = 1/force_dt.

    double pts; //current decoded pts. for capture. TODO: remove
    VideoCapture *capture;
    VideoFilterContext *filter_context;//TODO: use own smart ptr. QSharedPointer "=" is ugly
    VideoFrame displayed_frame;
};

VideoThread::VideoThread(QObject *parent) :
    AVThread(*new VideoThreadPrivate(), parent)
{
}

//it is called in main thread usually, but is being used in video thread,
VideoCapture* VideoThread::setVideoCapture(VideoCapture *cap)
{
    qDebug("setCapture %p", cap);
    DPTR_D(VideoThread);
    QMutexLocker locker(&d.mutex);
    VideoCapture *old = d.capture;
    d.capture = cap;
    if (old)
        disconnect(old, SIGNAL(requested()), this, SLOT(addCaptureTask()));
    if (cap)
        connect(cap, SIGNAL(requested()), this, SLOT(addCaptureTask()));
    if (cap->autoSave() && cap->name.isEmpty()) {
        // statistics is already set by AVPlayer
        cap->setCaptureName(QFileInfo(d.statistics->url).completeBaseName());
    }
    return old;
}

VideoCapture* VideoThread::videoCapture() const
{
    return d_func().capture;
}

void VideoThread::addCaptureTask()
{
    if (!isRunning())
        return;
    class CaptureTask : public QRunnable {
    public:
        CaptureTask(VideoThread *vt) : vthread(vt) {}
        void run() {
            VideoCapture *vc = vthread->videoCapture();
            if (!vc)
                return;
            VideoFrame frame(vthread->displayedFrame());
            //vthread->applyFilters(frame);
            vc->setVideoFrame(frame);
            vc->start();
        }
    private:
        VideoThread *vthread;
    };
    scheduleTask(new CaptureTask(this));
}

void VideoThread::clearRenderers()
{
    d_func().outputSet->sendVideoFrame(VideoFrame());
}

VideoFrame VideoThread::displayedFrame() const
{
    return d_func().displayed_frame;
}

void VideoThread::setFrameRate(qreal value)
{
    DPTR_D(VideoThread);
    d.force_fps = value;
    if (d.force_fps != 0.0) {
        d.force_dt = int(1000.0/d.force_fps);
    } else {
        d.force_dt = 0;
    }
}

void VideoThread::setBrightness(int val)
{
    setEQ(val, 101, 101);
}

void VideoThread::setContrast(int val)
{
    setEQ(101, val, 101);
}

void VideoThread::setSaturation(int val)
{
    setEQ(101, 101, val);
}

void VideoThread::setEQ(int b, int c, int s)
{
    class EQTask : public QRunnable {
    public:
        EQTask(VideoFrameConverter *c)
            : brightness(0)
            , contrast(0)
            , saturation(0)
            , conv(c)
        {
            //qDebug("EQTask tid=%p", QThread::currentThread());
        }
        void run() {
            conv->setEq(brightness, contrast, saturation);
        }
        int brightness, contrast, saturation;
    private:
        VideoFrameConverter *conv;
    };
    DPTR_D(VideoThread);
    EQTask *task = new EQTask(&d.conv);
    task->brightness = b;
    task->contrast = c;
    task->saturation = s;
    if (isRunning()) {
        scheduleTask(task);
    } else {
        task->run();
        delete task;
    }
}

void VideoThread::applyFilters(VideoFrame &frame)
{
    DPTR_D(VideoThread);
    QMutexLocker locker(&d.mutex);
    Q_UNUSED(locker);
    if (!d.filters.isEmpty()) {
        //sort filters by format. vo->defaultFormat() is the last
        foreach (Filter *filter, d.filters) {
            VideoFilter *vf = static_cast<VideoFilter*>(filter);
            if (!vf->isEnabled())
                continue;
            if (vf->prepareContext(d.filter_context, d.statistics, &frame))
                vf->apply(d.statistics, &frame);
        }
    }
}

// filters on vo will not change video frame, so it's safe to protect frame only in every individual vo
bool VideoThread::deliverVideoFrame(VideoFrame &frame)
{
    DPTR_D(VideoThread);
    /*
     * TODO: video renderers sorted by preferredPixelFormat() and convert in AVOutputSet.
     * Convert only once for the renderers has the same preferredPixelFormat().
     */
    d.outputSet->lock();
    QList<AVOutput *> outputs = d.outputSet->outputs();
    VideoRenderer *vo = 0;
    if (!outputs.isEmpty())
        vo = static_cast<VideoRenderer*>(outputs.first());
    if (vo && (!vo->isSupported(frame.pixelFormat())
            || (vo->isPreferredPixelFormatForced() && vo->preferredPixelFormat() != frame.pixelFormat())
            )) {
        VideoFormat fmt(frame.format());
        if (fmt.hasPalette() || fmt.isRGB())
            fmt = VideoFormat::Format_RGB32;
        else
            fmt = vo->preferredPixelFormat();
        VideoFrame outFrame(d.conv.convert(frame, fmt));
        if (!outFrame.isValid()) {
            d.outputSet->unlock();
            return false;
        }
        frame = outFrame;
    }
    d.outputSet->sendVideoFrame(frame); //TODO: group by format, convert group by group
    d.outputSet->unlock();

    Q_EMIT frameDelivered();
    return true;
}

void VideoThread::run()
{
    DPTR_D(VideoThread);
    if (!d.dec || !d.dec->isAvailable() || !d.outputSet)
        return;
    resetState();
    if (d.capture->autoSave()) {
        d.capture->setCaptureName(QFileInfo(d.statistics->url).completeBaseName());
    }
    //not neccesary context is managed by filters.
    d.filter_context = VideoFilterContext::create(VideoFilterContext::QtPainter);
    VideoDecoder *dec = static_cast<VideoDecoder*>(d.dec);
    Packet pkt;
    QVariantHash *dec_opt = &d.dec_opt_normal; //TODO: restore old framedrop option after seek
    /*!
     * if we skip some frames(e.g. seek, drop frames to speed up), then then first frame to decode must
     * be a key frame for hardware decoding. otherwise may crash
     */
    bool wait_key_frame = false;
    int nb_dec_slow = 0;

    qint32 seek_count = 0; // wm4 says: 1st seek can not use frame drop for decoder
    // TODO: kNbSlowSkip depends on video fps, ensure slow time <= 2s
    /* kNbSlowSkip: if video frame slow count >= kNbSlowSkip, skip decoding all frames until next keyframe reaches.
     * if slow count > kNbSlowSkip/2, skip rendering every 3 or 6 frames
     */
    const int kNbSlowSkip = 20;
    // kNbSlowFrameDrop: if video frame slow count > kNbSlowFrameDrop, skip decoding nonref frames. only some of ffmpeg based decoders support it.
    const int kNbSlowFrameDrop = 10;
    bool sync_audio = d.clock->clockType() == AVClock::AudioClock;
    bool sync_video = d.clock->clockType() == AVClock::VideoClock; // no frame drop
    const qint64 start_time = QDateTime::currentMSecsSinceEpoch();
    int nb_no_pts = 0;
    //bool wait_audio_drain
    const char* pkt_data = NULL; // workaround for libav9 decode fail but error code >= 0
    qint64 last_deliver_time = 0;
    int sync_id = 0;
    while (!d.stop) {
        processNextTask();
        //TODO: why put it at the end of loop then stepForward() not work?
        //processNextTask tryPause(timeout) and  and continue outter loop
        if (d.render_pts0 < 0) { // no pause when seeking
            if (tryPause()) { //DO NOT continue, or stepForward() will fail

            } else {
                if (isPaused())
                    continue; //timeout. process pending tasks
            }
        }
        if (d.seek_requested) {
            d.seek_requested = false;
            qDebug("request seek video thread");
            pkt = Packet(); // last decode failed and pkt is valid, reset pkt to force take the next packet if seek is requested
            msleep(1);
        } else {
            // d.render_pts0 < 0 means seek finished here
            if (d.clock->syncId() > 0) {
                qDebug("video thread wait to sync end for sync id: %d", d.clock->syncId());
                if (d.render_pts0 < 0 && sync_id > 0) {
                    msleep(10);
                    continue;
                }
            } else {
                sync_id = 0;
            }
        }
        if(!pkt.isValid() && !pkt.isEOF()) { // can't seek back if eof packet is read
            pkt = d.packets.take(); //wait to dequeue
           // TODO: push pts history here and reorder
        }
        if (pkt.isEOF()) {
            wait_key_frame = false;
            qDebug("video thread gets an eof packet.");
        } else {
            //qDebug() << pkt.position << " pts:" <<pkt.pts;
            //Compare to the clock
            if (!pkt.isValid()) {
                // may be we should check other information. invalid packet can come from
                wait_key_frame = true;
                qDebug("Invalid packet! flush video codec context!!!!!!!!!! video packet queue size: %d", d.packets.size());
                d.dec->flush(); //d.dec instead of dec because d.dec maybe changed in processNextTask() but dec is not
                d.render_pts0 = pkt.pts;
                sync_id = pkt.position;
                if (pkt.pts >= 0)
                    qDebug("video seek: %.3f, id: %d", d.render_pts0, sync_id);
                d.pts_history = ring<qreal>(d.pts_history.capacity());
                continue;
            }
        }
        if (pkt.pts <= 0 && !pkt.isEOF() && pkt.data.size() > 0) {
            nb_no_pts++;
        } else {
            nb_no_pts = 0;
        }
        if (nb_no_pts > 5) {
            qDebug("the stream may have no pts. force fps to: %f/%f", d.force_fps < 0 ? -d.force_fps : 24, d.force_fps);
            d.clock->setClockAuto(false);
            d.clock->setClockType(AVClock::VideoClock);
            if (d.force_fps < 0)
                setFrameRate(-d.force_fps);
            else if (d.force_fps == 0)
                setFrameRate(24);
        }

        if (d.clock->clockType() == AVClock::AudioClock) {
            sync_audio = true;
            sync_video = false;
        } else if (d.clock->clockType() == AVClock::VideoClock) {
            sync_audio = false;
            sync_video = true;
        } else {
            sync_audio = false;
            sync_video = false;
        }
        bool seeking = d.render_pts0 >= 0.0;
        if (seeking) {
            nb_dec_slow = 0;
        }

        // can not change d.delay after! we need it to comapre to next loop
        /*
         *after seeking forward, a packet may be the old, v packet may be
         *the new packet, then the d.delay is very large, omit it.
        */

        bool skip_render = false;

        if (wait_key_frame) {
            if (!pkt.hasKeyFrame) {
                qDebug("waiting for key frame. queue size: %d. pkt.size: %d", d.packets.size(), pkt.data.size());
                pkt = Packet();
                continue;
            }
            wait_key_frame = false;
        }
        QVariantHash *dec_opt_old = dec_opt;
        if (!seeking || pkt.pts - d.render_pts0 >= -0.05) { // MAYBE not seeking. We should not drop the frames near the seek target. FIXME: use packet pts distance instead of -0.05 (20fps)
            if (seeking)
                qDebug("seeking... pkt.pts - d.render_pts0: %.3f", pkt.pts - d.render_pts0);
            if (nb_dec_slow < kNbSlowFrameDrop) {
                if (dec_opt == &d.dec_opt_framedrop) {
                    qDebug("frame drop=>normal. nb_dec_slow: %d", nb_dec_slow);
                    dec_opt = &d.dec_opt_normal;
                }
            } else {
                if (dec_opt == &d.dec_opt_normal) {
                    qDebug("frame drop=>noref. nb_dec_slow: %d too slow", nb_dec_slow);
                    dec_opt = &d.dec_opt_framedrop;
                }
            }
        } else { // seeking
            if (seek_count > 0 && d.drop_frame_seek) {
                if (dec_opt == &d.dec_opt_normal) {
                    qDebug("seeking... pkt.pts - d.render_pts0: %.3f, frame drop=>noref. nb_dec_slow: %d", pkt.pts - d.render_pts0, nb_dec_slow);
                    dec_opt = &d.dec_opt_framedrop;
                }
            } else {
                seek_count = -1;
            }
        }

        // decoder maybe changed in processNextTask(). code above MUST use d.dec but not dec
        if (dec != static_cast<VideoDecoder*>(d.dec)) {
            dec = static_cast<VideoDecoder*>(d.dec);
            if (!pkt.hasKeyFrame) {
                wait_key_frame = true;
                continue;
            }
            qDebug("decoder changed. decoding key frame");
        }
        if (dec_opt != dec_opt_old)
            dec->setOptions(*dec_opt);

        qreal clock = d.clock->value();// * time_base;
        qreal ts = pkt.pts*0.02*1000.0;// * time_base*0.02;
        qreal diff = ts - clock;
        if (diff<0)
        {
            // ensure video will not later than 2s
            if (diff < -2 || (nb_dec_slow > kNbSlowSkip && diff < -1.0 && !pkt.hasKeyFrame)) {
                qDebug("video is too slow. skip decoding until next key frame.");
                qDebug() << "skip decoding. Clock:" << clock << " ts:" << ts << " diff:" << diff;
                // TODO: when to reset so frame drop flag can reset?
                nb_dec_slow = 0;
                wait_key_frame = true;
                pkt = Packet();
                // TODO: use discard flag
                continue;
            } else {
                nb_dec_slow++;
                qDebug("frame slow count: %d. v-a: %.3f", nb_dec_slow, diff);
            }
            continue;
        }
        if (!dec->decode(pkt)) {
            d.pts_history.push_back(d.pts_history.back());
            //qWarning("Decode video failed. undecoded: %d/%d", dec->undecodedSize(), pkt.data.size());
            if (pkt.isEOF()) {
                Q_EMIT eofDecoded();
                qDebug("video decode eof done. d.render_pts0: %.3f", d.render_pts0);
                if (d.render_pts0 >= 0) {
                    qDebug("video seek done at eof pts: %.3f. id: %d", d.pts_history.back(), sync_id);
                    d.render_pts0 = -1;
                    d.clock->syncEndOnce(sync_id);
                    Q_EMIT seekFinished(qint64(d.pts_history.back()*1000.0));
                    if (seek_count == -1)
                        seek_count = 1;
                    else if (seek_count > 0)
                        seek_count++;
                }
                if (!pkt.position)
                    break;
            }
            pkt = Packet();
            continue;
        }
        // reduce here to ensure to decode the rest data in the next loop
        if (!pkt.isEOF())
            pkt.skip(pkt.data.size() - dec->undecodedSize());
        VideoFrame frame = dec->frame();

        if (!frame.isValid()) {
            qWarning("invalid video frame from decoder. undecoded data size: %d", pkt.data.size());
            if (pkt_data == pkt.data.constData()) //FIXME: for libav9. what about other versions?
                pkt = Packet();
            else
                pkt_data = pkt.data.constData();
            continue;
        }
        pkt_data = pkt.data.constData();
        if (frame.timestamp() <= 0)
            frame.setTimestamp(pkt.pts*0.02); // pkt.pts is wrong. >= real timestamp
        const qreal pts = frame.timestamp();
        d.pts_history.push_back(pts);
        // seek finished because we can ensure no packet before seek decoded when render_pts0 is set
        //qDebug("pts0: %f, pts: %f, clock: %d", d.render_pts0, pts, d.clock->clockType());
        if (d.render_pts0 >= 0.0) {
            if (pts < d.render_pts0) {
                if (!pkt.isEOF())
                    pkt = Packet();
                continue;
            }
            d.render_pts0 = -1;
            qDebug("video seek finished @%f. id: %d", pts, sync_id);
            d.clock->syncEndOnce(sync_id);
            Q_EMIT seekFinished(qint64(pts*1000.0));
            if (seek_count == -1)
                seek_count = 1;
            else if (seek_count > 0)
                seek_count++;
        }
        if(d.clock->value() != 0) {
            qreal clock = d.clock->value();// * time_base;
            qreal ts = frame.timestamp();// * time_base*0.02;
            qreal diff = ts - clock;
            if(diff < 0)
            {
                qDebug() << "video too slow";
                if (nb_dec_slow > kNbSlowSkip) {
                    skip_render = !pkt.hasKeyFrame && (nb_dec_slow %2);
                }
            }
            else if (diff > 0)
            {
                waitAndCheck(diff, ts);
            }
        }

        if (skip_render) {
            qDebug("skip rendering @%.3f", pts);
            pkt = Packet();
            continue;
        }

        d.clock->updateVideoTime(frame.timestamp());

        Q_ASSERT(d.statistics);
        d.statistics->video.current_time = QTime(0, 0, 0).addMSecs(int(pts * 1000.0)); //TODO: is it expensive?
        applyFilters(frame);

        //while can pause, processNextTask, not call outset.puase which is deperecated
        while (d.outputSet->canPauseThread()) {
            d.outputSet->pauseThread(100);
            //tryPause(100);
            processNextTask();
        }

        // no return even if d.stop is true. ensure frame is displayed. otherwise playing an image may be failed to display
        if (!deliverVideoFrame(frame))
            continue;
        //qDebug("clock.diff: %.3f", d.clock->diff());
        if (d.force_dt > 0)
            last_deliver_time = QDateTime::currentMSecsSinceEpoch();
        // TODO: store original frame. now the frame is filtered and maybe converted to renderer perferred format
        d.displayed_frame = frame;
    }
    d.packets.clear();
    qDebug("Video thread stops running...");

}

} //namespace QtAV
