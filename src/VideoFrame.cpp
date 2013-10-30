/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2013 Wang Bin <wbsecg1@gmail.com>

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

#include "QtAV/VideoFrame.h"
#include "private/Frame_p.h"

namespace QtAV {

class VideoFramePrivate : public FramePrivate
{
public:
    VideoFramePrivate(int w, int h, VideoFrame::PixelFormat fmt)
        : FramePrivate()
        , width(w)
        , height(h)
        , pixel_format(fmt)
    {}
    virtual ~VideoFramePrivate() {}

    int width, height;
    VideoFrame::PixelFormat pixel_format;
    QVector<int> textures;
};


VideoFrame::VideoFrame(int width, int height, PixelFormat pixFormat):
    Frame(*new VideoFramePrivate(width, height, pixFormat))
{
}

VideoFrame::~VideoFrame()
{
}

int VideoFrame::bytesPerLine(int plane) const
{
    DPTR_D(const VideoFrame);
    switch (d.pixel_format) {
    case Format_YUV420P:
        return plane == 0 ? d.width * bytesPerPixel() : d.width * bytesPerPixel()/2;
    case Format_ARGB32:
    default:
        return d.width*bytesPerLine(plane);
        break;
    }
    return 0;
}

int VideoFrame::bytesPerPixel() const
{
    return d_func().pixel_format&kMaxBPP;
}

QSize VideoFrame::size() const
{
    DPTR_D(const VideoFrame);
    return QSize(d.width, d.height);
}

int VideoFrame::width() const
{
    return d_func().width;
}

int VideoFrame::height() const
{
    return d_func().height;
}

bool VideoFrame::convertTo(PixelFormat fmt)
{
    return false;
}

bool VideoFrame::mapToDevice()
{
    return false;
}

bool VideoFrame::mapToHost()
{
    return false;
}

int VideoFrame::texture(int plane) const
{
    DPTR_D(const VideoFrame);
    if (d.textures.size() <= plane)
        return -1;
    return d.textures[plane];
}

} //namespace QtAV
