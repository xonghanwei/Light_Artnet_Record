// Dmx512Sender.h - DMX512 串口发送器 (USB-DMX / RS-485 输出)
#ifndef DMX512_SENDER_H
#define DMX512_SENDER_H

#include "../core/Common.h"
#include <QObject>
#include <QSerialPort>
#include <QElapsedTimer>
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
     * @brief 非阻塞发送完整 DMX512 帧
     *
     * 与 sendFrame 的区别: write 后不 flush, 字节进入驱动 TX 队列由硬件异步发出,
     * 调用立即返回 (~0.1ms)。多端口可在一个节拍内连续触发, BREAK 起始偏差 ~ms 级。
     * 上一帧仍在传输 (距上次发送 < 一帧时长) 时直接拒绝, 避免 BREAK 插进帧中间。
     *
     * @return 成功入队返回 true; 忙 (上一帧未发完) 或失败返回 false
     */
    bool sendFrameAsync(const QByteArray &channels);

    /**
     * @brief 以读+写模式打开串口 (RDM 半双工: 同一端口发请求、收应答)
     */
    bool openRdm(const QString &portName, qint32 baudRate = 250000);

    /**
     * @brief 发送 E1.20 RDM 线上帧 (含起始码/长度/校验和的完整 DMX 包)
     *
     * BREAK + MAB + 原始字节 + flush (RDM 为一次性请求, 必须确保字节上线
     * 后再进入应答接收窗口)。
     * @param packet  完整线上帧
     * @param breakUs BREAK 时长: 常规 RDM (0xCC) 用 100, 发现 (0xFF) 规范要求 ≥176
     */
    bool sendRdmFrame(const QByteArray &packet, int breakUs = 100);

    /**
     * @brief 读取接收缓冲区当前可用的全部字节 (RDM 应答)
     */
    QByteArray readAvailable();

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
    void rdmDataReady();   // 串口收到数据 (仅 ReadWrite 模式有意义)

private:
    // 构建 DMX512 帧: BREAK + MAB + 0x00 Start Code + 512 channels
    QByteArray buildDmxFrame(const QByteArray &channels);

    // open/openRdm 公共实现: mode = WriteOnly (纯发送) / ReadWrite (RDM 半双工)
    bool openInternal(const QString &portName, qint32 baudRate, QIODevice::OpenMode mode);

    // 关闭并释放串口; 调用方必须已持有 m_mutex, 且不发信号 (避免锁内重入死锁)
    void closeLocked();

    // 发送 BREAK + MAB (硬件优先, 失败降级软件模拟); 调用方必须已持有 m_mutex
    // 返回 false 仅当软件回退路径写入失败
    bool sendBreakLocked(int breakUs = 100);

    // 一帧传输时长 (µs): BREAK 100 + MAB 15 + 513 字节 × 40µs/字节 ≈ 20635
    static constexpr qint64 kFrameUs = 20700;

    QSerialPort *m_serial = nullptr;
    QString m_portName;
    QMutex m_mutex;
    quint64 m_framesSent = 0;
    bool m_hwBreakFailed = false;  // 硬件 BREAK 探测失败后置位, 后续直接走软件回退 (open 成功时重置)
    QElapsedTimer m_txClock;       // 单调时钟, open 成功时启动 (sendFrameAsync 忙判定用)
    qint64 m_lastFrameStartUs = -1; // 最近一次异步帧入队时刻 (µs); -1 = 无
};

// ============================================================
// DmxSyncGroup — 多端口 BREAK 绝对时刻同步发送组
//
// 方案 1+2 (GUI 线程 23ms 节拍批量 sendFrameAsync) 的端口偏差约 1ms,
// 源于同 tick 内逐口执行 BREAK(100μs)+MAB(15μs) 忙等串行累加。
// 本类为每个端口配一个独立 worker 线程 (线程内打开并独占 QSerialPort,
// 规避跨线程串口亲和性问题), 协调者一次性发布 (各口帧数据, 统一绝对时刻),
// 各线程自旋等待到同一 QPC 时刻后同时触发 BREAK, 端口间偏差降至数十微秒级。
//
// 典型用法 (23ms 节拍):
//   targetUs = DmxSyncGroup::nowUs() + 1500;   // 预留唤醒+自旋余量
//   group.commit(frames, targetUs);            // 立即返回, 各线程到点齐发
// ============================================================
class DmxSyncGroup {
    Q_DISABLE_COPY(DmxSyncGroup)
public:
    DmxSyncGroup() = default;
    ~DmxSyncGroup() { stop(); }

    /**
     * @brief 为每个端口名启动一个 worker 线程 (空名 = 空闲槽位, 线程直接退出)
     * @return 打开失败的端口名列表 (空 = 全部成功); 已有运行中的组会先 stop
     */
    QStringList start(const QStringList &portNames, qint32 baudRate = 250000);

    /// 停止全部 worker 并关闭串口 (可重复调用)
    void stop();

    bool isRunning() const { return m_running; }
    int count() const { return int(m_workers.size()); }
    bool isOpen(int i) const;
    quint64 framesSent(int i) const;

    /**
     * @brief 发布本节拍: 槽 i 发送 frames[i] (不足 512 自动补零),
     *        全部端口在绝对时刻 targetUs (nowUs() 时基) 同时发出 BREAK
     */
    void commit(const QVector<QByteArray> &frames, quint64 targetUs);

    /// 单调时钟绝对时刻 (μs), 与 commit 的 targetUs 同时基
    static quint64 nowUs();

private:
    class Impl;   // 单端口 worker 线程 (定义在 .cpp)
    QVector<Impl*> m_workers;
    bool m_running = false;
};

#endif // DMX512_SENDER_H
