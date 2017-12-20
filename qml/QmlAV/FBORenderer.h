#include "QmlAV/QuickFBORenderer.h"

#include <QtQuick/QQuickFramebufferObject>
#include <QElapsedTimer>

namespace QtAV {

class FBORenderer : public QObject, public QQuickFramebufferObject::Renderer
{
    Q_OBJECT

public:
    FBORenderer(QuickFBORenderer* item);
    QOpenGLFramebufferObject* createFramebufferObject(const QSize &size) Q_DECL_OVERRIDE;

    void render() Q_DECL_OVERRIDE;
    void synchronize(QQuickFramebufferObject *item) Q_DECL_OVERRIDE;

public slots:
    void onNewFrameAvailable();

private:
    QuickFBORenderer *m_item;
};

} //namespace QtAV
