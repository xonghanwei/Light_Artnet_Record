// ArtNetReceiver.h - ArtNet协议接收模块
#ifndef ARTNET_RECEIVER_H
#define ARTNET_RECEIVER_H

#include "../core/Common.h"
#include <QObject>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QList>
#include <QTimer>

class ArtNetReceiver : public QObject {
    Q_OBJECT
public:
    explicit ArtNetReceiver(QObject *parent = nullptr);
    ~ArtNetReceiver();
    
    /**
     * @brief 启动监听
     * @param port ArtNet端口 (默认6454)
     * @param bindAddress 绑定地址 ("0.0.0.0"表示所有接口)
     * @return 成功返回true
     */
    bool start(quint16 port = ArtNet::DEFAULT_PORT, 
               const QString &bindAddress = QStringLiteral("0.0.0.0"));
    
    /**
     * @brief 停止监听
     */
    void stop();
    
    /**
     * @brief 是否正在监听
     */
    bool isRunning() const { return m_running; }
    
    /**
     * @brief 设置 Universe 过滤 (空列表 = 不过滤, 接收所有)
     */
    void setFilterUniverses(const QList<quint16> &universes);
    
    /**
     * @brief 添加过滤 Universe
     */
    void addFilterUniverse(quint16 universe);
    
    /**
     * @brief 清除过滤 (接收所有 Universe)
     */
    void clearFilter();
    
    /**
     * @brief 获取已接收数据包统计
     */
    quint64 totalPacketsReceived() const { return m_totalPackets; }
    quint64 dmxPacketsReceived() const { return m_dmxPackets; }
    quint64 rdmPacketsReceived() const { return m_rdmPackets; }
    
    /**
     * @brief 获取最近一次错误的详细描述
     */
    QString lastError() const { return m_lastError; }
    
signals:
    /**
     * @brief 收到 ArtDMX 帧
     */
    void dmxFrameReceived(const ArtNetDmxFrame &frame);
    
    /**
     * @brief 收到 RDM 数据
     */
    void rdmFrameReceived(const QByteArray &rdmData);
    
    /**
     * @brief 收到 ArtNet 原始包 (所有类型)
     */
    void rawPacketReceived(quint16 opCode, const QByteArray &packet);
    
    /**
     * @brief 状态变化
     */
    void statusChanged(const QString &status);
    
private slots:
    void onReadyRead();
    
private:
    void processPacket(const QNetworkDatagram &datagram);
    bool validateArtNetPacket(const QByteArray &packet);
    bool isUniverseFiltered(quint16 universe) const;
    
    QUdpSocket *m_socket = nullptr;
    QList<quint16> m_filterUniverses;
    bool m_filterEnabled = false;
    bool m_running = false;
    
    quint64 m_totalPackets = 0;
    quint64 m_dmxPackets = 0;
    quint64 m_rdmPackets = 0;
    
    QString m_lastError;
};

#endif // ARTNET_RECEIVER_H
