// Dmx512Sender.h - DMX512 串口发送器 (USB-DMX / RS-485 输出)
#ifndef DMX512_SENDER_H
#define DMX512_SENDER_H

#include "../core/Common.h"
#include <QObject>
#include <QSerialPort>
#include <QList>
#include <QMutex>

/**
 * @brief DMX512 串口发送器
 * 
 * 协议格式 (RS-485 / USB-DMX):
 *   BREAK (90μs) + MAB (12μs) + Start Code (0x00) + 512 channels (8bit each)
 *   串口参数: 250000 baud, 8 data bits, 2 stop bits, No parity
 * 
 * 典型设备:
 *   - DMXKing ultraDMX Micro (FT232)
 *   - Enttec Open DMX USB (FTDI)
 *   - Velleman VM116
 *   - 任何 USB 转 RS-485 适配器
 */
class Dmx512Sender : public QObject {
    Q_OBJECT
public:
    explicit Dmx512Sender(QObject *parent = nullptr);
    ~Dmx512Sender();
    
    /**
     * @brief 打开串口
     * @param portName 串口名 (Windows: "COM3", Linux: "/dev/ttyUSB0")
     * @param baudRate 波特率 (默认 250000, DMX512 标准)
     * @return 成功返回true
     */
    bool open(const QString &portName, qint32 baudRate = 250000);
    
    void close();
    bool isOpen() const { return m_serial && m_serial->isOpen(); }
    
    QString portName() const { return m_portName; }
    quint64 totalSentFrames() const { return m_framesSent; }
    
    /**
     * @brief 发送完整的 DMX512 帧
     * @param channels 512 通道数据 (多余截断, 不足补0)
     * @return 成功返回true
     */
    bool sendFrame(const QByteArray &channels);
    
    /**
     * @brief 发送 DMX512 帧 (原始字节数组, 必须恰好 513 字节含 start code)
     */
    bool sendRaw(const QByteArray &data);
    
    /**
     * @brief 列出系统可用的串口
     */
    static QStringList availablePorts();
    
    /**
     * @brief 常用波特率
     */
    static qint32 defaultBaudRate() { return 250000; }
    
signals:
    void portOpened(const QString &name);
    void portClosed();
    void portError(const QString &error);
    
private:
    // 构建 DMX512 帧: BREAK + MAB + 0x00 Start Code + 512 channels
    QByteArray buildDmxFrame(const QByteArray &channels);

    // 关闭并释放串口; 调用方必须已持有 m_mutex, 且不发信号 (避免锁内重入死锁)
    void closeLocked();
    
    QSerialPort *m_serial = nullptr;
    QString m_portName;
    QMutex m_mutex;
    quint64 m_framesSent = 0;
    bool m_hwBreakFailed = false;  // 硬件 BREAK 探测失败后置位, 后续直接走软件回退 (open 成功时重置)
};

#endif // DMX512_SENDER_H
