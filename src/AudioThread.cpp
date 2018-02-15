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

#include "AudioThread.h"
#include "AVThread_p.h"
#include "QtAV/AudioDecoder.h"
#include "QtAV/Packet.h"
#include "QtAV/AudioFormat.h"
#include "QtAV/AudioOutput.h"
#include "QtAV/AudioResampler.h"
#include "QtAV/AVClock.h"
#include "QtAV/Filter.h"
#include "output/OutputSet.h"
#include "QtAV/private/AVCompat.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include "utils/Logger.h"

namespace QtAV {

class AudioThreadPrivate : public AVThreadPrivate
{
public:
    void init() {
        resample = false;
        last_pts = 0;
    }

    bool resample;
    qreal last_pts; //used when audio output is not available, to calculate the aproximate sleeping time
};

AudioThread::AudioThread(QObject *parent)
    :AVThread(*new AudioThreadPrivate(), parent)
{
}

void AudioThread::applyFilters(AudioFrame &frame)
{
    DPTR_D(AudioThread);
    //QMutexLocker locker(&d.mutex);
    //Q_UNUSED(locker);
    if (!d.filters.isEmpty()) {
        //sort filters by format. vo->defaultFormat() is the last
        foreach (Filter *filter, d.filters) {
            AudioFilter *af = static_cast<AudioFilter*>(filter);
            if (!af->isEnabled())
                continue;
            af->apply(d.statistics, &frame);
        }
    }
}
/*
 *TODO:
 * if output is null or dummy, the use duration to wait
 */
void AudioThread::run()
{
    DPTR_D(AudioThread);
    //No decoder or output. No audio output is ok, just display picture
    if (!d.dec || !d.dec->isAvailable() || !d.outputSet)
        return;
    resetState();
    Q_ASSERT(d.clock != 0);
    d.init();
    Packet pkt;
    qint64 fake_duration = 0LL;
    qint64 fake_pts = 0LL;
    int sync_id = 0;
    while (!d.stop) {
        processNextTask();
        pkt = d.packets.take();
        if(!pkt.isValid())
        {
            if (pkt.pts >= 0) { // check seek first
                qDebug("Invalid packet! flush audio codec context!!!!!!!! audio queue size=%d", d.packets.size());
                QMutexLocker locker(&d.mutex);
                Q_UNUSED(locker);
                if (d.dec) //maybe set to null in setDecoder()
                    d.dec->flush();
            }
        }
        AudioDecoder *dec = static_cast<AudioDecoder*>(d.dec);
        if (!dec) {
            pkt = Packet(); //mark invalid to take next
            continue;
        }
        dec->decode(pkt);

        AudioFrame frame = dec->frame();
        if(!frame.isValid()) {
            qDebug("FRAME NOT VALID");
        }
        if (d.clock->initialValue() == 0) {
            qDebug("Update initial clock value from audio thread\n");
            d.clock->setInitialValue(frame.timestamp());
        }
        d.clock->updateValue(frame.timestamp()*1000.0*0.02);

        QByteArray decoded(frame.data());
         int decodedSize = decoded.size();
         int decodedPos = 0;
         qreal delay = 0;
         const qreal byte_rate = frame.format().bytesPerSecond();
         qreal pts = frame.timestamp();
         AudioOutput *ao = 0;
         // first() is not null even if list empty
         if (!d.outputSet->outputs().isEmpty())
             ao = static_cast<AudioOutput*>(d.outputSet->outputs().first());

         if (ao && !ao->isAvailable()) {
             AVCodecContext *avctx = (AVCodecContext*)dec->codecContext();
             AudioFormat af;
             af.setSampleRate(avctx->sample_rate);
             af.setSampleFormatFFmpeg(avctx->sample_fmt);
             af.setChannelLayoutFFmpeg(avctx->channel_layout);
             if (af.isValid()) {
                 ao->setAudioFormat(af); /// set before close to workaround OpenAL context lost
                 ao->close();
                 qDebug() << "AudioOutput format: " << ao->audioFormat() << "; requested: " << ao->requestedFormat();
                 if (!ao->open()) {
                     qWarning("Audio: Failed to open ao\n");
                 } else {
                     qDebug("Audio: Succesfully opened ao\n");
                 }
                 dec->resampler()->setOutAudioFormat(ao->audioFormat());
             }
         }
         //DO NOT decode and convert if ao is not available or mute!
         bool has_ao = ao && ao->isAvailable();
         if (has_ao) {
             applyFilters(frame);
             frame.setAudioResampler(dec->resampler()); //!!!
             // FIXME: resample ONCE is required for audio frames from ffmpeg
             //if (ao->audioFormat() != frame.format()) {
                 frame = frame.to(ao->audioFormat());
             //}
         }
         if (has_ao && dec->resampler()) {
             if (dec->resampler()->speed() != ao->speed()
                     || dec->resampler()->outAudioFormat() != ao->audioFormat()) {
                 //resample later to ensure thread safe. TODO: test
                 if (d.resample) {
                     qDebug() << "ao.format " << ao->audioFormat();
                     qDebug() << "swr.format " << dec->resampler()->outAudioFormat();
                     qDebug("decoder set speed: %.2f", ao->speed());
                     dec->resampler()->setOutAudioFormat(ao->audioFormat());
                     dec->resampler()->setSpeed(ao->speed());
                     dec->resampler()->prepare();
                     d.resample = false;
                 } else {
                     d.resample = true;
                 }
             }
         }
         //qDebug("frame samples: %d @%.3f+%lld", frame.samplesPerChannel()*frame.channelCount(), frame.timestamp(), frame.duration()/1000LL);
         while (decodedSize > 0) {
             if (d.stop) {
                 qDebug("audio thread stop after decode()");
                 break;
             }
             const int chunk = qMin(decodedSize, has_ao ? ao->bufferSize() : 512*frame.format().bytesPerFrame());//int(max_len*byte_rate));
             //AudioFormat.bytesForDuration
             const qreal chunk_delay = (qreal)chunk/(qreal)byte_rate;
             if (has_ao && ao->isOpen()) {
                 QByteArray decodedChunk = QByteArray::fromRawData(decoded.constData() + decodedPos, chunk);
                 //qDebug("ao.timestamp: %.3f, pts: %.3f, pktpts: %.3f", ao->timestamp(), pts, pkt.pts);
                 ao->play(decodedChunk, pts);
             } /*else {
                 d.clock->updateDelay(delay += chunk_delay);
                 //TODO: avoid acummulative error. External clock?
                 msleep((unsigned long)(chunk_delay * 1000.0));
             }*/
             decodedPos += chunk;
             decodedSize -= chunk;
             if (pts != 0) {
                pts += chunk_delay;
//                pkt.pts += chunk_delay; // packet not fully decoded, use new pts in the next decoding
//                pkt.dts += chunk_delay;
             }
         }
    }
    d.packets.clear();
    qDebug("Audio thread stops running...");
}

} //namespace QtAV
