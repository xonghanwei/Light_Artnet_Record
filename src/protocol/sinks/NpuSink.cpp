// NpuSink.cpp - MA NPU (Ma-Net2) 协议输出
#include "NpuSink.h"
#include "../../core/Logger.h"
#include <cstring>

NpuSink::NpuSink(QObject *parent)
    : IProtocolSink(parent) {
}

NpuSink::~NpuSink() {
    stop();
}

bool NpuSink::start(const ConnectionParams &params) {
    if (m_running) return true;
    
    m_socket = new QUdpSocket();
    
    quint16 bindPort = params.localPort;
    if (!m_socket->bind(QHostAddress::AnyIPv4, bindPort ? bindPort : 0,
                        QUdpSocket::ShareAddress)) {
        LOG_ERROR(QString("[NpuSink] 绑定失败: %1").arg(m_socket->errorString()));
        emit sendError(QString("NpuSink 绑定失败: %1").arg(m_socket->errorString()));
        delete m_socket;
        m_socket = nullptr;
        return false;
    }
    
    // 如果 start 时指定了 remote, 直接加一个目标
    if (!params.remoteAddress.isEmpty() && params.remotePort > 0) {
        addTarget(params.remoteAddress, params.remotePort);
    }
    
    m_running = true;
    m_sequence = 0;
    
    QString info = QString("[NpuSink] 已启动 (本地端口 %1, %2 个目标)")
        .arg(m_socket->localPort()).arg(m_targets.size());
    LOG_INFO(info);
    emit started(info);
    
    return true;
}

void NpuSink::stop() {
    if (!m_running) return;
    m_running = false;
    
    if (m_socket) {
        m_socket->close();
        delete m_socket;
        m_socket = nullptr;
    }
    
    m_targets.clear();
    LOG_INFO("[NpuSink] 已停止");
    emit stopped();
}

int NpuSink::addTarget(const QString &ip, quint16 port) {
    Target t;
    t.address = QHostAddress(ip);
    t.port = port;
    m_targets.append(t);
    emit statusChanged(QString("NpuSink 目标已添加: %1:%2").arg(ip).arg(port));
    return m_targets.size() - 1;
}

void NpuSink::clearTargets() {
    m_targets.clear();
    emit statusChanged("NpuSink 所有目标已清除");
}

bool NpuSink::sendFrame(const LightFrame &frame) {
    if (!m_running || !m_socket) return false;
    if (m_targets.isEmpty()) return false;
    
    // 构造 NPU DMX 包
    quint8 universe = quint8(frame.universeRaw & 0xFF);
    if (universe == 0 && frame.universe > 0) {
        // 如果 LightFrame 只有 ArtNet 编码值, 取低 4 bit
        universe = quint8(frame.universe & 0x0F);
    }
    
    QByteArray packet;
    packet.reserve(NpuProtocol::DMX_PACKET_SIZE);
    
    quint16 length = 514; // cmd + universe + 512
    packet.append(char(length & 0xFF));
    packet.append(char((length >> 8) & 0xFF));
    packet.append(char(NpuProtocol::OP_DMX));
    packet.append(char(universe));
    
    QByteArray dmx = frame.paddedDmx();
    packet.append(dmx);
    
    bool anySent = false;
    for (auto &t : m_targets) {
        qint64 sent = m_socket->writeDatagram(packet, t.address, t.port);
        if (sent > 0) {
            t.packetsSent++;
            anySent = true;
        }
    }
    
    if (anySent) {
        m_sentFrames++;
        emit frameSent(frame.universe, quint64(m_targets.size()));
    } else {
        emit sendError("NpuSink 发送失败 (全部目标)");
    }
    return anySent;
}
