/******************************************************************************
    QtAV:  Multimedia framework based on Qt and FFmpeg
    Copyright (C) 2012-2016 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV (from 2014)

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

#include "QtAV/MediaIO.h"
#include "QtAV/private/MediaIO_p.h"
#include "QtAV/private/mkid.h"
#include "QtAV/private/factory.h"
#include <QtCore/QFile>
#include "utils/Logger.h"
#include <QWaitCondition>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QDataStream>
#include <QTextStream>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtDebug>
#include <QThread>
#include <QQueue>
#include <QTime>
#include <QUrl>

#define RTSP_RECEIVE_BUFFER 2048
#define KEEPALIVE_INTERVAL 60
#define KEEPALIVE_MARGIN 5

#define UINT16_MAX 65535

namespace QtAV {
enum RTSPResult {
	RTSP_RESULT_OK = 200,
};

class SatIPIOPrivate;
class SatIPIO : public MediaIO
{
	Q_OBJECT
	DPTR_DECLARE_PRIVATE(SatIPIO)
public:
	SatIPIO();
	~SatIPIO();
	virtual QString name() const Q_DECL_OVERRIDE;
	virtual const QStringList& protocols() const Q_DECL_OVERRIDE;

	virtual bool isSeekable() const Q_DECL_OVERRIDE;
	virtual bool isWritable() const Q_DECL_OVERRIDE;
	virtual qint64 read(char *data, qint64 maxSize) Q_DECL_OVERRIDE;
	virtual qint64 write(const char *data, qint64 maxSize) Q_DECL_OVERRIDE;
	virtual bool seek(qint64 offset, int from) Q_DECL_OVERRIDE;
	virtual qint64 position() const Q_DECL_OVERRIDE;
	virtual qint64 size() const Q_DECL_OVERRIDE;
	virtual QString formatForced() const Q_DECL_OVERRIDE;

protected slots:
	virtual void rtspSocketError(QAbstractSocket::SocketError socketError);
	virtual void rtspSocketConnected();
	virtual void rtspSocketRead();
	virtual void udpSocketRead();
	virtual void keepalivePing();
    virtual void rtspTeardown();

protected:
	SatIPIO(SatIPIOPrivate &d);
	void onUrlChanged() Q_DECL_OVERRIDE;
	virtual RTSPResult rtspHandle();
	virtual void parseSession(const QByteArray &requestLine);
	virtual int parseTransport(const QByteArray &requestLine);
    void connectRtspSocket(const QTcpSocket* rtspSocket);

signals:
    void sendRtspTeardown();
    void sendKillSockets();
};

typedef SatIPIO MediaIOSatIP;
static const MediaIOId MediaIOId_SatIP = mkid::id32base36_5<'S','a','t','I','P'>::value;
static const char kSatIPName[] = "SatIP";
FACTORY_REGISTER(MediaIO, SatIP, kSatIPName)

class SatIPIOPrivate : public MediaIOPrivate
{
public:
	SatIPIOPrivate()
		: MediaIOPrivate(),
		rtspSocketMutex(QMutex::Recursive),
		rtspSocket(new QTcpSocket()),
		rtcpSocket(new QUdpSocket()),
		udpSocket(NULL),
		udpThread(NULL),
		keepalive(KEEPALIVE_INTERVAL - KEEPALIVE_MARGIN),
		cseq(1)
	{
	}
	~SatIPIOPrivate() { 
		rtspSocket->abort();
		rtcpSocket->abort();
		delete rtspSocket;
		delete rtcpSocket;
	}

	QMutex rtspSocketMutex;
	QTcpSocket *rtspSocket;
	QUdpSocket *rtcpSocket;
	QUdpSocket *udpSocket;
	QThread *udpThread;
	QQueue<QByteArray*> fifo;
	QWaitCondition fifoWait;
	QMutex fifoMutex;

	/* RTSP state */
	QElapsedTimer keepaliveTimer;
	QByteArray sessionID;
	QByteArray contentBase;
	QByteArray udpAddress;
	QByteArray control;
	uint16_t udpPort;
	int keepalive;
	int streamID;
	int cseq;
};

SatIPIO::SatIPIO() : MediaIO(*new SatIPIOPrivate())
{
	DPTR_D(SatIPIO);

    connectRtspSocket(d.rtspSocket);
    connect(this, &SatIPIO::sendRtspTeardown,
            this, &SatIPIO::rtspTeardown);
}

SatIPIO::SatIPIO(SatIPIOPrivate &d) : MediaIO(d)
{
    connectRtspSocket(d.rtspSocket);
}

SatIPIO::~SatIPIO()
{
    rtspTeardown();
    //emit sendRtspTeardown();
}

void SatIPIO::connectRtspSocket(const QTcpSocket* rtspSocket)
{
    if(rtspSocket)
    {
        connect(rtspSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
                this, &SatIPIO::rtspSocketError);
        connect(rtspSocket, &QAbstractSocket::connected, this,
                &SatIPIO::rtspSocketConnected);
        connect(rtspSocket, &QIODevice::readyRead, this,
                &SatIPIO::rtspSocketRead);
    }
    else
    {
        qWarning() << "unexpected: rtspSocket is null.";
    }
}

QString SatIPIO::name() const
{
	return QLatin1String(kSatIPName);
}

const QStringList& SatIPIO::protocols() const
{
	static QStringList p = QStringList() << QStringLiteral("satip");
	return p;
}

void SatIPIO::rtspSocketError(QAbstractSocket::SocketError socketError)
{
	DPTR_D(SatIPIO);
	QMutexLocker(&d.rtspSocketMutex);

	switch (socketError) {
	case QAbstractSocket::RemoteHostClosedError:
		qDebug() << "Remote closed connection.";
		break;
	case QAbstractSocket::HostNotFoundError:
		qDebug() << "Host not found.";
		break;
	case QAbstractSocket::ConnectionRefusedError:
		qDebug() << "Connection Refused.";
		break;
	default:
		qDebug() << "Unknown error: " << d.rtspSocket->errorString();
	}
}

void SatIPIO::rtspSocketConnected()
{
	qDebug() << "RTSP socket connection established.";
}

void SatIPIO::rtspSocketRead()
{
	qDebug() << "RTSP socket ready for reading.";
}

void SatIPIO::keepalivePing()
{
	DPTR_D(SatIPIO);
	QMutexLocker(&d.rtspSocketMutex);

	QString msg = "";
	QTextStream msg_ts(&msg);

	if (!d.rtspSocket->isValid())
		return;

	msg_ts << "OPTIONS" << d.control << " RTSP/1.0\r\n";
	msg_ts << "CSeq: " << d.cseq++ << "\r\n";
	msg_ts << "Session: " << d.sessionID << "\r\n\r\n";
	d.rtspSocket->write(msg.toLatin1());

	if (rtspHandle() != RTSP_RESULT_OK)
		qWarning() << "Failed to ping RTSP session.";
}

void SatIPIO::parseSession(const QByteArray &requestLine)
{
	DPTR_D(SatIPIO);

	QList<QByteArray> tokens = requestLine.split(';');

	if (tokens.count() < 1)
		return;

	d.sessionID = tokens[0];
	for (int i = 1; i < tokens.count(); i++) {
		if (tokens[i].startsWith("timeout=")) {
			d.keepalive = qMax(tokens[i].mid(8).toInt() -
					KEEPALIVE_MARGIN, 1);
		}
	}
}

int SatIPIO::parseTransport(const QByteArray &requestLine)
{
	DPTR_D(SatIPIO);

	QList<QByteArray> tokens = requestLine.split(';');
	if (tokens.count() < 1 || !tokens[0].startsWith("RTP/AVP"))
		return -1;

	if (tokens.count() < 2 || !tokens[1].startsWith("multicast"))
		return 0;

	for (int i = 2; i < tokens.count(); i++) {
		if (tokens[i].startsWith("destination=")) {
			d.udpAddress = tokens[i].mid(12);
		} else if (tokens[i].startsWith("port=")) {
			int port = tokens[i].split('-')[0].toInt();
			if (port >= 0 && port < 65535)
				d.udpPort = port;
			else
				return -1;
		}
	}

	return 0;
}

RTSPResult SatIPIO::rtspHandle()
{
	DPTR_D(SatIPIO);
	QMutexLocker(&d.rtspSocketMutex);

	int contentLength = 0;
	int rtspResult = 0;
	QByteArray in;

	for (;;) {
		if (!d.rtspSocket->canReadLine() &&
				!d.rtspSocket->waitForReadyRead()) {
			qCritical() << "Timeout waiting for data.";
			break;
		}
		in = d.rtspSocket->readLine();

		if (in.startsWith("RTSP/1.0 ")) {
			rtspResult = in.mid(9, 3).toInt();
		} else if (in.startsWith("Content-Base:")) {
			d.contentBase = in.mid(13).trimmed();
		} else if (in.startsWith("Content-Length:")) {
			contentLength = in.mid(15).trimmed().toInt();
		} else if (in.startsWith("Session:")) {
			parseSession(in.mid(8).trimmed());
		} else if (in.startsWith("Transport")) {
			if (parseTransport(in.mid(10).trimmed()) < 0) {
				rtspResult = -1;
				break;
			}
		} else if (in.startsWith("com.ses.streamID:")) {
			d.streamID = in.mid(17).trimmed().toInt();
		} else if (in.startsWith("\r\n")) {
			/* End of header */
			break;
		}
	}

	/* Discard further content */
	while (contentLength > 0)
		contentLength -= d.rtspSocket->readAll().count();

	return (RTSPResult)rtspResult;
}

void SatIPIO::rtspTeardown()
{
	DPTR_D(SatIPIO);
	QMutexLocker(&d.rtspSocketMutex);

	QString msg = "";
	QTextStream msg_ts(&msg);

	if (!d.rtspSocket->isValid() || !d.udpSocket)
		return;

	msg_ts << "TEARDOWN " << d.control << " RTSP/1.0\r\n";
	msg_ts << "CSeq: " << d.cseq++ << "\r\n";
	msg_ts << "Session: " << d.sessionID << "\r\n\r\n";
	d.rtspSocket->write(msg.toLatin1());

	if (rtspHandle() != RTSP_RESULT_OK) {
		qWarning() << "Failed to teardown RTSP session.";
		return;
	}

	d.rtspSocket->abort();
	d.rtcpSocket->abort();
	d.udpThread->requestInterruption();
	d.fifoWait.wakeAll();
	d.udpThread->quit();
	d.udpThread->wait();
	delete d.udpThread;
	d.udpThread = NULL;
	d.udpSocket = NULL;
}

void SatIPIO::onUrlChanged()
{
	DPTR_D(SatIPIO);
	QMutexLocker(&d.rtspSocketMutex);

	QUrl url(d.url);
	QString msg = "";
	QTextStream msg_ts(&msg);

	if (!url.isValid()) {
		qWarning() << "Specified URL is invalid.";
		return;
	}

    emit sendRtspTeardown();
	d.rtspSocket->connectToHost(url.host(), url.port());
	if (!d.rtspSocket->waitForConnected()) {
		qCritical() << "Could not connect to remote host.";
		return;
	}

	url.setScheme("rtsp");
	d.contentBase = url.toEncoded(QUrl::RemovePath | QUrl::RemoveQuery).append("/");

	qsrand(static_cast<uint>(QTime::currentTime().msec()));

	d.udpThread = new QThread();
	d.udpThread->start();
	d.udpSocket = new QUdpSocket();
	connect(d.udpSocket, &QIODevice::readyRead, this,
			&SatIPIO::udpSocketRead, Qt::DirectConnection);

	// find an unused port pair
	do {
		d.rtcpSocket->abort();
		d.udpSocket->abort();
		d.udpPort = 5534 + (qrand() * 2 % 60000);
	} while (!d.udpSocket->bind(d.udpPort) || !d.rtcpSocket->bind(d.udpPort + 1));
	qDebug() << "Bound to port " << d.udpPort;

	msg_ts << "SETUP " << url.url() << " RTSP/1.0\r\n";
	msg_ts << "CSeq: " << d.cseq++ << "\r\n";
	msg_ts << "Transport: RTP/AVP;unicast;client_port=" << d.udpPort <<
		"-" << (d.udpPort + 1) << "\r\n\r\n";
	qDebug() << "Send: '" << msg << "'";
	d.rtspSocket->write(msg.toLatin1());

	if (rtspHandle() != RTSP_RESULT_OK) {
		qCritical() << "Failed to setup RTSP session.";
		return;
	}

	d.control = QString("%1stream=%2").arg(QString(d.contentBase))
		.arg(d.streamID).toLatin1();

	// Make sure udp port matches
	if (d.udpSocket->localPort() != d.udpPort) {
		d.rtcpSocket->abort();
		d.udpSocket->abort();
		qDebug() << "Rebind to port " << d.udpPort;
		if (!d.rtcpSocket->bind(d.udpPort) ||
				!d.udpSocket->bind(d.udpPort)) {
            emit sendRtspTeardown();
			return;
		}
	}
	d.udpSocket->moveToThread(d.udpThread);
	connect(d.udpThread, &QThread::finished, d.udpSocket,
			&QObject::deleteLater);

	msg = "";
	msg_ts << "PLAY " << d.control << " RTSP/1.0\r\n";
	msg_ts << "CSeq: " << d.cseq++ << "\r\n";
	msg_ts << "Session: " << d.sessionID << "\r\n\r\n";
	d.rtspSocket->write(msg.toLatin1());

	if (rtspHandle() != RTSP_RESULT_OK) {
		qCritical() << "Failed to play RTSP session.";
		return;
	}

	d.keepaliveTimer.start();

	// FIXME: Add rtcpSocket
}

bool SatIPIO::isSeekable() const
{
	/* Unsupported, Live Streams only */
	return false;
}

bool SatIPIO::isWritable() const
{
	/* Unsupported, Live Streams only */
	return false;
}

void SatIPIO::udpSocketRead()
{
	DPTR_D(SatIPIO);
	int maxSize = RTSP_RECEIVE_BUFFER;
	char buf[RTSP_RECEIVE_BUFFER];

	if (d.keepaliveTimer.elapsed() > d.keepalive * 1000) {
		keepalivePing();
		d.keepaliveTimer.restart();
	}

	d.fifoMutex.lock();
	while (d.udpSocket->hasPendingDatagrams()) {
		while (d.fifo.size() > 50 &&
				!d.udpThread->isInterruptionRequested())
			d.fifoWait.wait(&d.fifoMutex);

		if (d.udpThread->isInterruptionRequested())
			break;

		qint64 size = d.udpSocket->readDatagram(buf, RTSP_RECEIVE_BUFFER);
		if (size <= 12)
			continue;
		size -= 12;
		QByteArray *data = new QByteArray(buf + 12, size);
		maxSize -= size;
		d.fifo.enqueue(data);
		d.fifoWait.wakeAll();
	}
	d.fifoMutex.unlock();
}

qint64 SatIPIO::read(char *data, qint64 maxSize)
{
	DPTR_D(SatIPIO);

	d.fifoMutex.lock();
	int read = 0;

	if (!d.udpThread)
		return -1;

	while (maxSize > 0) {
		while (d.fifo.size() == 0 && d.udpThread->isRunning())
        {
            d.fifoWait.wait(&d.fifoMutex,500);
            if(d.udpSocket && !d.udpSocket->hasPendingDatagrams())
            {
                d.fifoWait.wakeAll();
                d.fifoMutex.unlock();
                return read;
            }
        }
		if (!d.udpThread->isRunning())
			break;

		if (d.fifo.head()->size() > maxSize)
			break;

		QByteArray *buf = d.fifo.dequeue();
		memcpy(data + read, buf->constData(), buf->size());
		read += buf->size();
		maxSize -= buf->size();
		delete buf;
	}

	d.fifoWait.wakeAll();
	d.fifoMutex.unlock();
	return read;
}

qint64 SatIPIO::write(const char *data, qint64 maxSize)
{
	Q_UNUSED(data);
	Q_UNUSED(maxSize);
	return 0;
}

bool SatIPIO::seek(qint64 offset, int from)
{
	/* Unsupported, Live Streams only */
	Q_UNUSED(offset);
	Q_UNUSED(from);
	return false;
}

qint64 SatIPIO::position() const
{
	/* Unsupported, Live Streams only */
	return 0;
}

qint64 SatIPIO::size() const
{
	/* Unsupported, Live Streams only */
	return 0;
}

QString SatIPIO::formatForced() const
{
	static QString format = "mpegts";
	return format;
}

} //namespace QtAV
#include "SatIPIO.moc"
