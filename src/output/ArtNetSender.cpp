// ArtNetSender.cpp
#include "ArtNetSender.h"
#include "../core/Logger.h"
#include <QNetworkDatagram>
#include <cstring>

ArtNetSender::ArtNetSender(QObject *parent)
    : QObject(parent) {
}

ArtNetSender::~ArtNetSender() {
    stop();
}

bool ArtNetSender::start(quint16 localPort) {
    if (m_running) return true;
    
    m_socket = new QUdpSocket(this);
    
    if (!m_socket->bind(QHostAddress::AnyIPv4, localPort, 
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        LOG_ERROR(QString("ArtNetSender 绑定失败: port=%1 - %2")
            .arg(localPort).arg(m_socket->errorString()));
        delete m_socket;
        m_socket = nullptr;
        return false;
    }
    
    m_running = true;
    m_sequenceCounter = 0;
    
    LOG_INFO(QString("ArtNetSender 已启动 (本地端口 %1)").arg(m_socket->localPort()));
    return true;
}

void ArtNetSender::stop() {
    if (!m_running) return;
    m_running = false;
    
    if (m_socket) {
        m_socket->close();
        delete m_socket;
        m_socket = nullptr;
    }
    
    LOG_INFO("ArtNetSender 已停止");
}

int ArtNetSender::addTarget(const QString &ip, quint16 port, 
                            const QList<quint16> &universes) {
    // 注意: 必须先 start(), 再 addTarget. 不再隐式启动 (与 NpuSink 行为一致)
    
    Target t;
    t.id = m_nextTargetId++;
    t.address = QHostAddress(ip);
    t.port = port;
    t.filterUniverses = universes;
    
    m_targets.append(t);
    
    LOG_INFO(QString("ArtNet 转发目标已添加: %1:%2 (Universe: %3)")
        .arg(ip)
        .arg(port)
        .arg(universes.isEmpty() ? "全部" : 
             [&]{ QStringList l; 
                   for(auto u:universes) l.append(QString("0x%1").arg(u,0,16)); 
                   return l.join(","); }()));
    
    emit targetAdded(t.id, ip, port);
    return t.id;
}

bool ArtNetSender::removeTarget(int targetId) {
    for (int i = 0; i < m_targets.size(); ++i) {
        if (m_targets[i].id == targetId) {
            m_targets.removeAt(i);
            emit targetRemoved(targetId);
            return true;
        }
    }
    return false;
}

void ArtNetSender::clearTargets() {
    m_targets.clear();
}

bool ArtNetSender::universeMatches(const Target &t, quint16 universe) const {
    if (t.filterUniverses.isEmpty()) return true;
    return t.filterUniverses.contains(universe);
}

void ArtNetSender::sendDmxFrame(quint16 universe, const QByteArray &dmxData) {
    if (!m_running || !m_socket || dmxData.isEmpty()) return;
    
    // 构造 ArtDMX 包
    ArtNetDmxTxPacket packet;
    packet.sequence = m_sequenceCounter++;
    packet.setUniverse(universe);
    
    QByteArray dmx = dmxData.left(ArtNet::DMX_CHANNEL_COUNT);
    if (dmx.size() < ArtNet::DMX_CHANNEL_COUNT) {
        dmx.append(QByteArray(ArtNet::DMX_CHANNEL_COUNT - dmx.size(), 0));
    }
    packet.setDmx(reinterpret_cast<const uint8_t*>(dmx.constData()));
    
    QByteArray raw = QByteArray::fromRawData(reinterpret_cast<const char*>(&packet), sizeof(packet));
    
    // 发送给匹配的所有目标
    for (auto &t : m_targets) {
        if (!universeMatches(t, universe)) continue;
        
        qint64 sent = m_socket->writeDatagram(raw, t.address, t.port);
        if (sent > 0) {
            t.packetsSent++;
        }
    }
}

void ArtNetSender::sendMultipleUniverses(const QMap<quint16, QByteArray> &universeData) {
    for (auto it = universeData.constBegin(); it != universeData.constEnd(); ++it) {
        sendDmxFrame(it.key(), it.value());
    }
}
