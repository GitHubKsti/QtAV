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

    VideoDecoder *dec = static_cast<VideoDecoder*>(d.dec);
    Packet pkt;

    d.filter_context = VideoFilterContext::create(VideoFilterContext::QtPainter);

    while (!d.stop) {
        processNextTask();
        pkt = d.packets.take();
        dec->decode(pkt);

        VideoFrame frame = dec->frame();
        if(!frame.isValid()) {
            qDebug("FRAME NOT VALID");
        }

        qDebug() << "Frame TS is" << frame.timestamp();
        qDebug() << "Clock TS is" << d.clock->value();

        if(d.clock->value() != 0) {
            qreal diff = frame.timestamp() - d.clock->value(); //+ v_a;
            waitAndCheck(diff, frame.timestamp());
        }
        d.clock->updateVideoTime(frame.timestamp());

        //if(video_clock)


        processNextTask();
        deliverVideoFrame(frame);
    }
    d.packets.clear();

}

} //namespace QtAV
