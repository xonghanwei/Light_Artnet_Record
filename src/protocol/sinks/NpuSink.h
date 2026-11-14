// NpuSink.h - MA NPU (Ma-Net2) 协议输出
#ifndef NPU_SINK_H
#define NPU_SINK_H

#include "../protocol/IProtocolSink.h"
#include <QUdpSocket>
#include <QHostAddress>
#include <cstdint>

/**
 * @brief MA NPU (Ma-Net2) 协议输出
 * 
 * 向 MA NPU 节点或控制器推送 DMX 数据。
 * 包格式: [2B length LE] [1B cmd=0x01] [1B universe] [512B DMX]
 */
class NpuSink : public IProtocolSink {
    Q_OBJECT
public:
    explicit NpuSink(QObject *parent = nullptr);
    ~NpuSink() override;
    
    ProtocolType protocolType() const override { return ProtocolType::Npu; }
    QString protocolName() const override { return "MA NPU (Ma-Net2)"; }
    
    bool start(const ConnectionParams &params) override;
    void stop() override;
    bool isRunning() const override { return m_running; }
    
    bool sendFrame(const LightFrame &frame) override;
    quint64 framesSent() const override { return m_sentFrames; }
    
    // 多目标
    int addTarget(const QString &ip, quint16 port = NpuProtocol::DEFAULT_PORT);
    void clearTargets();
    int targetCount() const { return m_targets.size(); }
    
private:
    struct Target {
        QHostAddress address;
        quint16 port;
        quint64 packetsSent = 0;
    };
    
    QUdpSocket *m_socket = nullptr;
    QList<Target> m_targets;
    bool m_running = false;
    quint8 m_sequence = 0;
    quint64 m_sentFrames = 0;
};

#endif // NPU_SINK_H
