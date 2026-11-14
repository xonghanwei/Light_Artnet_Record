// NpuSource.h - MA NPU (Ma-Net2) 协议接收
#ifndef NPU_SOURCE_H
#define NPU_SOURCE_H

#include "../protocol/IProtocolSource.h"
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <cstdint>

/**
 * @brief MA NPU (Ma-Net2) 协议接收实现
 * 
 * NPU 是 MA Lighting 的主从架构协议, 默认 UDP 端口 9999。
 * 控制器 (grandMA2/3) 向 NPU 推送 DMX 数据, 或 NPU 请求 Universe 信息。
 * 
 * 常用 OpCode:
 *   0x01 = DMX 数据包: [2B len LE] [1B cmd=0x01] [1B universe] [512B DMX]
 *   0x02 = Universe 信息 / 请求
 *   0x03 = 时间码
 * 
 * 这个 Source 主要用于:
 *   1. 把 MA 控制台的 DMX 实时数据接进来录制
 *   2. 录制同时转发到其他协议 (ArtNet/DMX512) 驱动灯
 *   3. 作为纯 "DMX 采集端", 替换纯 ArtNet 接收场景
 */
class NpuSource : public IProtocolSource {
    Q_OBJECT
public:
    explicit NpuSource(QObject *parent = nullptr);
    ~NpuSource() override;
    
    ProtocolType protocolType() const override { return ProtocolType::Npu; }
    QString protocolName() const override { return "MA NPU (Ma-Net2)"; }
    
    bool start(const ConnectionParams &params) override;
    void stop() override;
    bool isRunning() const override { return m_running; }
    
    void setFilterUniverses(const QList<quint16> &universes) override;
    
    quint64 framesReceived() const override { return m_dmxPackets; }
    
private slots:
    void onReadyRead();
    
private:
    void processDatagram(const QNetworkDatagram &datagram);
    bool parseDmxPacket(const QByteArray &packet, LightFrame &outFrame);
    
    QUdpSocket *m_socket = nullptr;
    QList<quint16> m_filterUniverses; // Universe 原始编号 (0-255)
    bool m_filterEnabled = false;
    bool m_running = false;
    
    quint64 m_totalPackets = 0;
    quint64 m_dmxPackets = 0;
    quint64 m_otherPackets = 0;
};

#endif // NPU_SOURCE_H
