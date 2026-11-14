// ArtNetSink.h - ArtNet 协议输出实现 (包装现有 ArtNetSender)
#ifndef ARTNET_SINK_H
#define ARTNET_SINK_H

#include "../protocol/IProtocolSink.h"
#include "../../output/ArtNetSender.h"

/**
 * @brief ArtNet 协议输出
 * 
 * 包装 ArtNetSender, 把 LightFrame 转成 ArtDMX 包发送。
 * 支持多目标 fan-out, 每个目标独立 Universe 过滤。
 * 
 * UI 信号: frameSent / started / stopped / statusChanged / sendError
 */
class ArtNetSink : public IProtocolSink {
    Q_OBJECT
public:
    explicit ArtNetSink(QObject *parent = nullptr);
    ~ArtNetSink() override;
    
    ProtocolType protocolType() const override { return ProtocolType::ArtNet; }
    QString protocolName() const override { return "ArtNet"; }
    
    bool start(const ConnectionParams &params) override;
    void stop() override;
    bool isRunning() const override;
    
    bool sendFrame(const LightFrame &frame) override;
    quint64 framesSent() const override { return m_sentFrames; }
    
    // 多目标管理 (ArtNet 特有, 在 start 之后调用)
    int addTarget(const QString &ip, quint16 port = ArtNet::DEFAULT_PORT,
                  const QList<quint16> &universeFilter = {});
    void clearTargets();
    int targetCount() const { return m_sender.targetCount(); }

private slots:
    void onTargetAdded(int id, const QString &ip, quint16 port);
    void onTargetRemoved(int id);
    
private:
    ArtNetSender m_sender;
    quint64 m_sentFrames = 0;
};

#endif // ARTNET_SINK_H
