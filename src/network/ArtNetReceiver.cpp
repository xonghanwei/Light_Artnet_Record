// ArtNetReceiver.cpp
#include "ArtNetReceiver.h"
#include "../core/Logger.h"
#include <QHostAddress>
#include <cstring>

#pragma pack(push, 1)
// ArtNet ArtDMX 包结构
struct ArtNetDmxPacket {
    char     id[8];           // "Art-Net\0"
    quint16  opCode;          // 操作码 (小端序)
    quint16  protocolVersion; // 协议版本 (小端序)
    quint8   sequence;
    quint8   physical;
    quint16  universe;        // Universe (小端序, NetSubUni)
    quint16  length;          // DMX数据长度 (小端序)
    // 后面紧跟 DMX 数据
};
#pragma pack(pop)

ArtNetReceiver::ArtNetReceiver(QObject *parent)
    : QObject(parent) {
}

ArtNetReceiver::~ArtNetReceiver() {
    stop();
}

bool ArtNetReceiver::start(quint16 port, const QString &bindAddress) {
    m_lastError.clear();
    
    if (m_running) {
        m_lastError = "ArtNet接收器已在运行";
        LOG_WARN("ArtNet接收器已在运行");
        return false;
    }
    
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, 
            this, &ArtNetReceiver::onReadyRead);
    
    QHostAddress address(bindAddress);
    quint16 bindPort = port;
    
    if (!m_socket->bind(address, bindPort)) {
        m_lastError = QString("ArtNet接收器绑定失败: %1:%2 - %3")
            .arg(bindAddress)
            .arg(port)
            .arg(m_socket->errorString());
        LOG_ERROR(m_lastError);
        delete m_socket;
        m_socket = nullptr;
        return false;
    }
    
    m_running = true;
    m_totalPackets = 0;
    m_dmxPackets = 0;
    m_rdmPackets = 0;
    
    LOG_INFO(QString("ArtNet接收器已启动: 监听端口 %1").arg(port));
    emit statusChanged(QString("Listening on port %1").arg(port));
    
    return true;
}

void ArtNetReceiver::stop() {
    if (!m_running) return;
    
    m_running = false;
    
    if (m_socket) {
        m_socket->close();
        delete m_socket;
        m_socket = nullptr;
    }
    
    LOG_INFO("ArtNet接收器已停止");
    emit statusChanged("Stopped");
}

void ArtNetReceiver::setFilterUniverses(const QList<quint16> &universes) {
    m_filterUniverses = universes;
    m_filterEnabled = !universes.isEmpty();
}

void ArtNetReceiver::addFilterUniverse(quint16 universe) {
    if (!m_filterUniverses.contains(universe)) {
        m_filterUniverses.append(universe);
    }
    m_filterEnabled = true;
}

void ArtNetReceiver::clearFilter() {
    m_filterUniverses.clear();
    m_filterEnabled = false;
}

bool ArtNetReceiver::isUniverseFiltered(quint16 universe) const {
    if (!m_filterEnabled) return false; // 不过滤
    return !m_filterUniverses.contains(universe);
}

void ArtNetReceiver::onReadyRead() {
    if (!m_socket) return;
    
    while (m_socket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_socket->receiveDatagram();
        processPacket(datagram);
    }
}

void ArtNetReceiver::processPacket(const QNetworkDatagram &datagram) {
    QByteArray packetData = datagram.data();
    
    m_totalPackets++;
    
    // 最小 ArtNet 包: 8(id) + 2(opcode) + 2(version) + ... = 至少 10 字节
    if (packetData.size() < 10) return;
    
    if (!validateArtNetPacket(packetData)) return;
    
    // 解析 OpCode (小端序)
    quint16 opCode = quint16(quint8(packetData[9]) << 8) | quint8(packetData[8]);
    
    emit rawPacketReceived(opCode, packetData);
    
    switch (opCode) {
        case ArtNet::OP_DMX: { // ArtDMX
            m_dmxPackets++;
            
            if (packetData.size() < qsizetype(sizeof(ArtNetDmxPacket))) break;
            
            const char *raw = packetData.constData();
            
            ArtNetDmxFrame frame;
            frame.sequence = quint8(raw[12]);
            frame.physical = quint8(raw[13]);
            frame.universe = quint16(quint8(raw[15]) << 8) | quint8(raw[14]);
            frame.length = quint16(quint8(raw[17]) << 8) | quint8(raw[16]);
            frame.receiveTimeUs = QDateTime::currentMSecsSinceEpoch() * 1000;
            
            // 检查 Universe 过滤
            if (isUniverseFiltered(frame.universe)) break;
            
            // 读取 DMX 数据
            int dmxOffset = 18;
            int dmxLen = qMin(int(frame.length), int(ArtNet::DMX_CHANNEL_COUNT));
            dmxLen = qMin(dmxLen, packetData.size() - dmxOffset);
            
            if (dmxLen > 0) {
                std::memcpy(frame.dmxData, raw + dmxOffset, dmxLen);
            }
            
            emit dmxFrameReceived(frame);
            break;
        }
        
        case ArtNet::OP_RDM:   // ArtRDM
        case ArtNet::OP_RDMSUB: // ArtRDMSub
        {
            m_rdmPackets++;
            emit rdmFrameReceived(packetData);
            break;
        }
        
        default:
            // 忽略其他 OpCode
            break;
    }
}

bool ArtNetReceiver::validateArtNetPacket(const QByteArray &packet) {
    if (packet.size() < 8) return false;
    
    // 验证 "Art-Net\0" 标识
    return std::memcmp(packet.constData(), ArtNet::ID_STRING, 8) == 0;
}
