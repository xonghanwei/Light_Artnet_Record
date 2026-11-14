// Dmx512Source.h - DMX512 串口被动接收
#ifndef DMX512_SOURCE_H
#define DMX512_SOURCE_H

#include "../protocol/IProtocolSource.h"
#include <QSerialPort>
#include <QByteArray>

/**
 * @brief DMX512 串口被动接收实现
 * 
 * 注意: DMX512 是单向协议, 通常是控制台 → 灯具。
 * 要接收 DMX512 线路上的信号, 必须有支持 "Read DMX" 的硬件,
 * 例如 Enttec Pro Mk3 / DMXKing ultraDMX Pro 的只读模式。
 * 
 * 常见替代方案:
 *   1. 绝大多数灯光软件会把 DMX512 转换成 ArtNet 输出 → 用 ArtNetSource 就够了
 *   2. 硬件 DMX 接收器把物理 DMX 线路 → USB/ArtNet 转发 → 还是 ArtNetSource
 *   
 * 这个 Source 主要用于:
 *   - 物理 DMX512 线路上的"窃听"录制
 *   - 有支持读 DMX 的 USB-DMX 硬件时的原生接入
 */
class Dmx512Source : public IProtocolSource {
    Q_OBJECT
public:
    explicit Dmx512Source(QObject *parent = nullptr);
    ~Dmx512Source() override;
    
    ProtocolType protocolType() const override { return ProtocolType::Dmx512; }
    QString protocolName() const override { return "DMX512"; }
    
    bool start(const ConnectionParams &params) override;
    void stop() override;
    bool isRunning() const override { return m_running; }
    
    void setFilterUniverses(const QList<quint16> &universes) override;
    
    quint64 framesReceived() const override { return m_framesReceived; }
    
private slots:
    void onReadyRead();
    
private:
    void processSerialData();
    bool m_running = false;
    QSerialPort *m_serial = nullptr;
    
    // DMX512 没有 Universe 概念, 这里存储的是"线路号"或 0
    QList<quint16> m_filterUniverses; 
    bool m_filterEnabled = false;
    
    quint64 m_framesReceived = 0;
    QByteArray m_readBuffer;
    
    // DMX512 一帧 = BREAK + MAB + Start Code(0x00) + 512 channel bytes
    // 串口层我们读到的已经是 channel bytes
};

#endif // DMX512_SOURCE_H
