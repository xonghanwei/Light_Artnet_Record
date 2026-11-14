// ArtNetSender.h - ArtNet ArtDMX 网络发送器 (用于转发到其他 ArtNet 节点)
#ifndef ARTNET_SENDER_H
#define ARTNET_SENDER_H

#include "../core/Common.h"
#include <QObject>
#include <QUdpSocket>
#include <QHostAddress>
#include <QList>
#include <cstdint>

#pragma pack(push, 1)
// ArtNet ArtDMX 包结构 (用于发包)
struct ArtNetDmxTxPacket {
    char     id[8] = {'A','r','t','-','N','e','t','\0'};
    uint16_t opCode = 0x5000;           // ArtDMX
    uint16_t protocolVersion = 0x000E;  // 14
    uint8_t  sequence = 0;
    uint8_t  physical = 0;
    uint16_t universe = 0;              // NetSubUni (小端序)
    uint16_t length = ArtNet::DMX_CHANNEL_COUNT;
    uint8_t  dmxData[512] = {0};
    
    void setUniverse(uint16_t u) {
        // ArtNet 里 universe 是小端序存的
        universe = (uint16_t((u >> 8) & 0x7F) << 8) | uint16_t(u & 0xFF);
    }
    
    void setDmx(const uint8_t *data, int len = ArtNet::DMX_CHANNEL_COUNT) {
        if (len > ArtNet::DMX_CHANNEL_COUNT) len = ArtNet::DMX_CHANNEL_COUNT;
        std::memcpy(dmxData, data, len);
    }
};
#pragma pack(pop)

class ArtNetSender : public QObject {
    Q_OBJECT
public:
    explicit ArtNetSender(QObject *parent = nullptr);
    ~ArtNetSender();
    
    /**
     * @brief 启动
     * @param localPort 本地绑定端口 (0=随机)
     * @return 成功返回true
     */
    bool start(quint16 localPort = 0);
    
    void stop();
    bool isRunning() const { return m_running; }
    
    /**
     * @brief 添加一个转发目标
     * @param ip 目标 IP ("255.255.255.255" 为广播)
     * @param port 目标 ArtNet 端口 (默认 6454)
     * @param universes 要转发的 Universe 编码列表 (空=全部)
     * @return 目标 ID (用于后续移除)
     */
    int addTarget(const QString &ip, quint16 port = ArtNet::DEFAULT_PORT,
                  const QList<quint16> &universes = {});
    
    bool removeTarget(int targetId);
    void clearTargets();
    int targetCount() const { return m_targets.size(); }
    
    /**
     * @brief 发送单个 Universe 的 DMX 数据到所有匹配目标
     * @param universe 编码后的 Universe
     * @param dmxData 512 字节 DMX 数据
     */
    void sendDmxFrame(quint16 universe, const QByteArray &dmxData);
    
    /**
     * @brief 发送多个 Universe 到所有目标 (fan-out)
     * @param universeData key=encoded universe
     */
    void sendMultipleUniverses(const QMap<quint16, QByteArray> &universeData);
    
signals:
    void targetAdded(int id, const QString &ip, quint16 port);
    void targetRemoved(int id);
    void statusChanged(const QString &status);
    
private:
    struct Target {
        int id;
        QHostAddress address;
        quint16 port;
        QList<quint16> filterUniverses; // 空=全部
        quint64 packetsSent = 0;
    };
    
    bool universeMatches(const Target &t, quint16 universe) const;
    
    QUdpSocket *m_socket = nullptr;
    QList<Target> m_targets;
    bool m_running = false;
    int m_nextTargetId = 1;
    uint8_t m_sequenceCounter = 0;
};

#endif // ARTNET_SENDER_H
