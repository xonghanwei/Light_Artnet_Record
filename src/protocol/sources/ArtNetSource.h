// ArtNetSource.h - ArtNet 协议接收实现 (包装现有 ArtNetReceiver)
#ifndef ARTNET_SOURCE_H
#define ARTNET_SOURCE_H

#include "../protocol/IProtocolSource.h"
#include "../network/ArtNetReceiver.h"
#include <memory>

/**
 * @brief ArtNet 协议接收
 * 
 * 实现策略: 内部持有 ArtNetReceiver 实例, 
 *           收到 ArtNetDmxFrame 时转换为 LightFrame 再发出。
 */
class ArtNetSource : public IProtocolSource {
    Q_OBJECT
public:
    explicit ArtNetSource(QObject *parent = nullptr);
    ~ArtNetSource() override;
    
    ProtocolType protocolType() const override { return ProtocolType::ArtNet; }
    QString protocolName() const override { return "ArtNet"; }
    
    bool start(const ConnectionParams &params) override;
    void stop() override;
    bool isRunning() const override { return m_receiver && m_receiver->isRunning(); }
    
    void setFilterUniverses(const QList<quint16> &universes) override;
    
    quint64 framesReceived() const override { return m_receiver ? m_receiver->dmxPacketsReceived() : 0; }
    
private slots:
    void onArtNetDmxReceived(const ArtNetDmxFrame &frame);
    void onArtNetStatusChanged(const QString &status);
    
private:
    ArtNetReceiver *m_receiver = nullptr;
};

#endif // ARTNET_SOURCE_H
