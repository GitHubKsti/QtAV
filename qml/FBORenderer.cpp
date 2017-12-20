/******************************************************************************
    QtAV:  Multimedia framework based on Qt and FFmpeg
    Copyright (C) 2012-2017 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV (from 2015)

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

#include "QmlAV/QuickFBORenderer.h"
#include "QmlAV/FBORenderer.h"
#include "QmlAV/QmlAVPlayer.h"
#include "QtAV/AVPlayer.h"
#include "QtAV/OpenGLVideo.h"
#include "QtAV/private/VideoRenderer_p.h"
#include "QtAV/private/mkid.h"
#include "QtAV/private/factory.h"
#include <QtCore/QCoreApplication>
#include <QtGui/QOpenGLFunctions>
#include <QtGui/QOpenGLFramebufferObject>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGSimpleTextureNode>

#include <QDebug>
// for dynamicgl. qglfunctions before qt5.3 does not have portable gl functions
// use qt gl func if possible to avoid linking to opengl directly
#if QT_VERSION >= QT_VERSION_CHECK(5, 3, 0)
#include <QtGui/QOpenGLFunctions>
#define DYGL(glFunc) QOpenGLContext::currentContext()->functions()->glFunc
#else
#define DYGL(glFunc) glFunc
#endif

namespace QtAV {

FBORenderer::FBORenderer(QuickFBORenderer* item) : m_item(item) {

}
QOpenGLFramebufferObject* FBORenderer::createFramebufferObject(const QSize &size) {
    m_item->fboSizeChanged(size);
    return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
}

void FBORenderer::render() {
    Q_ASSERT(m_item);
    static QElapsedTimer timer;
    static long long count = 0;
    static bool first = true;
    if(first)
    {
        timer.start();
        first = false;
    }
    count++;
    qint64 elapsed = timer.elapsed();
    double inSeconds = elapsed/1000.0;
    double frequency = count/inSeconds;

    if(inSeconds > 1.0)
    {
        timer.restart();
        count = 0;
        qInfo() << "Render frequency:" << frequency;
    }
    QElapsedTimer renderDuration;
    renderDuration.start();
    m_item->renderToFbo(framebufferObject());
    qint64 elapsedRender = renderDuration.elapsed();
    if(elapsedRender > 2)
    {
        //qInfo() << "Rendering needs more time than 2 milliseconds: " << elapsedRender;
    }
}
void FBORenderer::synchronize(QQuickFramebufferObject *item) {
    if(!m_item)
    {
        m_item = static_cast<QuickFBORenderer*>(item);
    }
}

void FBORenderer::onNewFrameAvailable()
{
    static QElapsedTimer timer;
    static long long count = 0;
    static bool first = true;
    if(first)
    {
        timer.start();
        first = false;
    }
    count++;
    qint64 elapsed = timer.elapsed();
    double inSeconds = elapsed/1000.0;
    double frequency = count/inSeconds;
    if(inSeconds > 1.0)
    {
        timer.restart();
        count = 0;
        qInfo() << "onNewFrameAvailable frequency:" << frequency;
    }
    render();
}
} //namespace QtAV
