// Dmx512Sink.h - DMX512 串口输出 (包装现有 Dmx512Sender)
#ifndef DMX512_SINK_H
#define DMX512_SINK_H

#include "../protocol/IProtocolSink.h"
#include "../../output/Dmx512Sender.h"

/**
 * @brief DMX512 串口输出
 * 
 * 简单包装 Dmx512Sender, LightFrame → BREAK+MAB+0x00+512ch 直接输出到 COM 口。
 */
class Dmx512Sink : public IProtocolSink {
    Q_OBJECT
public:
    explicit Dmx512Sink(QObject *parent = nullptr);
    ~Dmx512Sink() override;
    
    ProtocolType protocolType() const override { return ProtocolType::Dmx512; }
    QString protocolName() const override { return "DMX512"; }
    
    bool start(const ConnectionParams &params) override;
    void stop() override;
    bool isRunning() const override;
    
    bool sendFrame(const LightFrame &frame) override;
    quint64 framesSent() const override { return m_sender.totalSentFrames(); }
    
    QString portName() const { return m_sender.portName(); }
    
private:
    Dmx512Sender m_sender;
};

#endif // DMX512_SINK_H
