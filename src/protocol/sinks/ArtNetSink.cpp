// ArtNetSink.cpp
#include "ArtNetSink.h"
#include "../../core/Logger.h"

ArtNetSink::ArtNetSink(QObject *parent)
    : IProtocolSink(parent) {
    // 连接底层 ArtNetSender 的信号, 转换为 IProtocolSink 统一信号
    connect(&m_sender, &ArtNetSender::statusChanged,
            this, &IProtocolSink::statusChanged);
    connect(&m_sender, &ArtNetSender::targetAdded,
            this, &ArtNetSink::onTargetAdded);
    connect(&m_sender, &ArtNetSender::targetRemoved,
            this, &ArtNetSink::onTargetRemoved);
}

ArtNetSink::~ArtNetSink() {
    stop();
}

bool ArtNetSink::start(const ConnectionParams &params) {
    quint16 port = params.localPort; // 本地绑定端口, 0=随机
    bool ok = m_sender.start(port);
    if (ok) {
        emit started(QString("ArtNetSink 启动 (%1 个预置目标)")
            .arg(m_sender.targetCount()));
    } else {
        emit sendError("ArtNetSink 启动失败");
    }
    return ok;
}

void ArtNetSink::stop() {
    m_sender.stop();
    emit stopped();
}

bool ArtNetSink::isRunning() const {
    return m_sender.isRunning();
}

bool ArtNetSink::sendFrame(const LightFrame &frame) {
    if (!m_sender.isRunning()) return false;
    if (m_sender.targetCount() == 0) return false;
    
    QByteArray dmx = frame.paddedDmx();
    m_sender.sendDmxFrame(frame.universe, dmx);
    m_sentFrames++;
    
    emit frameSent(frame.universe, quint64(m_sender.targetCount()));
    return true;
}

int ArtNetSink::addTarget(const QString &ip, quint16 port,
                          const QList<quint16> &universeFilter) {
    return m_sender.addTarget(ip, port, universeFilter);
}

void ArtNetSink::clearTargets() {
    m_sender.clearTargets();
}

void ArtNetSink::onTargetAdded(int id, const QString &ip, quint16 port) {
    emit statusChanged(QString("ArtNetSink 目标已添加 #%1: %2:%3")
        .arg(id).arg(ip).arg(port));
}

void ArtNetSink::onTargetRemoved(int id) {
    emit statusChanged(QString("ArtNetSink 目标已移除 #%1").arg(id));
}
