// NpuSource.cpp - MA NPU (Ma-Net2) 协议接收
#include "NpuSource.h"
#include "../../core/Logger.h"
#include <QHostAddress>
#include <QDateTime>
#include <cstring>

NpuSource::NpuSource(QObject *parent)
    : IProtocolSource(parent) {
}

NpuSource::~NpuSource() {
    stop();
}

bool NpuSource::start(const ConnectionParams &params) {
    if (m_running) return false;
    
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead,
            this, &NpuSource::onReadyRead);
    
    quint16 port = params.localPort > 0 ? params.localPort : NpuProtocol::DEFAULT_PORT;
    QString bindAddr = params.localBindAddress.isEmpty() ? "0.0.0.0" : params.localBindAddress;
    
    m_filterUniverses = params.filterUniverses;
    m_filterEnabled = !params.filterUniverses.isEmpty();
    
    if (!m_socket->bind(QHostAddress(bindAddr), port,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        LOG_ERROR(QString("[NpuSource] 绑定失败 %1:%2 - %3")
            .arg(bindAddr).arg(port).arg(m_socket->errorString()));
        delete m_socket;
        m_socket = nullptr;
        return false;
    }
    
    m_running = true;
    m_totalPackets = 0;
    m_dmxPackets = 0;
    
    LOG_INFO(QString("[NpuSource] 已启动监听 %1:%2")
        .arg(bindAddr).arg(port));
    emit statusChanged(QString("NPU listening on port %1").arg(port));
    
    return true;
}

void NpuSource::stop() {
    if (!m_running) return;
    
    m_running = false;
    if (m_socket) {
        m_socket->close();
        delete m_socket;
        m_socket = nullptr;
    }
    
    LOG_INFO("[NpuSource] 已停止");
    emit statusChanged("NPU stopped");
}

void NpuSource::setFilterUniverses(const QList<quint16> &universes) {
    m_filterUniverses = universes;
    m_filterEnabled = !universes.isEmpty();
}

void NpuSource::onReadyRead() {
    if (!m_socket) return;
    
    while (m_socket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_socket->receiveDatagram();
        processDatagram(datagram);
    }
}

void NpuSource::processDatagram(const QNetworkDatagram &datagram) {
    QByteArray data = datagram.data();
    m_totalPackets++;
    
    // NPU 最小包: [2B length] + [1B cmd] + ...
    if (data.size() < 3) return;
    
    quint16 length = quint16(quint8(data[0]) | (quint16(quint8(data[1])) << 8));
    quint8 cmd = quint8(data[2]);
    
    switch (cmd) {
        case NpuProtocol::OP_DMX: {
            // DMX 数据包: 期望长度 = 2(长度头) + 1(命令) + 1(域) + 512(数据) = 516
            if (data.size() >= NpuProtocol::DMX_PACKET_SIZE) {
                LightFrame lf;
                if (parseDmxPacket(data, lf)) {
                    m_dmxPackets++;
                    emit frameReceived(lf);
                }
            }
            break;
        }
        default:
            m_otherPackets++;
            break;
    }
}

bool NpuSource::parseDmxPacket(const QByteArray &packet, LightFrame &outFrame) {
    const char *raw = packet.constData();
    
    quint8 cmd = quint8(raw[2]);
    if (cmd != NpuProtocol::OP_DMX) return false;
    
    quint8 universe = quint8(raw[3]);
    
    // Universe 过滤
    if (m_filterEnabled && !m_filterUniverses.contains(universe)) {
        return false;
    }
    
    outFrame.sourceProtocol = ProtocolType::Npu;
    outFrame.universeRaw = universe;
    // 编码成 ArtNet 兼容格式 (Net=0, SubNet=0, Universe=universe低4bit)
    outFrame.universe = quint16(universe & 0x0F);
    outFrame.dmxData = QByteArray(raw + 4, 512);
    outFrame.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
    outFrame.sequence = quint32(m_dmxPackets);
    
    return true;
}
