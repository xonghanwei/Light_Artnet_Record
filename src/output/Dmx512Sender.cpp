// Dmx512Sender.cpp
#include "Dmx512Sender.h"
#include "../core/Logger.h"
#include <QSerialPortInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>
#endif

// 微秒级精确延时 (忙等).
// Windows Sleep/usleep 受系统定时器精度 (~15.6ms) 影响, Linux usleep 有调度抖动,
// 均无法满足 DMX BREAK(90±10μs)/MAB(12±2μs) 时序, 必须用高分辨率单调时钟计数.
static void preciseDelayUs(qint64 us) {
#ifdef Q_OS_WIN
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const LONGLONG target = static_cast<LONGLONG>(freq.QuadPart * static_cast<double>(us) / 1000000.0);
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < target);
#else
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const long targetNs = static_cast<long>(us * 1000);
    long elapsedNs = 0;
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsedNs = static_cast<long>((now.tv_sec - start.tv_sec) * 1000000000L
                                      + (now.tv_nsec - start.tv_nsec));
    } while (elapsedNs < targetNs);
#endif
}

Dmx512Sender::Dmx512Sender(QObject *parent)
    : QObject(parent) {
}

Dmx512Sender::~Dmx512Sender() {
    close();
}

bool Dmx512Sender::open(const QString &portName, qint32 baudRate) {
    return openInternal(portName, baudRate, QIODevice::WriteOnly);
}

bool Dmx512Sender::openRdm(const QString &portName, qint32 baudRate) {
    return openInternal(portName, baudRate, QIODevice::ReadWrite);
}

bool Dmx512Sender::openInternal(const QString &portName, qint32 baudRate, QIODevice::OpenMode mode) {
    // 信号统一在释放 m_mutex 后发出: 若槽函数回调本类 open/close/sendFrame,
    // 锁内 emit 会因非递归锁死锁
    QString errText;
    bool opened = false;
    bool closedPrev = false;
    {
        QMutexLocker locker(&m_mutex);

        if (isOpen()) {
            if (m_portName == portName && m_serial->openMode() == mode) return true;
            closeLocked();
            closedPrev = true;
        }

        m_serial = new QSerialPort(this);
        m_serial->setPortName(portName);

        if (!m_serial->open(mode)) {
            errText = m_serial->errorString();
            LOG_ERROR(QString("DMX512 串口打开失败: %1 - %2").arg(portName, errText));
            delete m_serial;
            m_serial = nullptr;
        } else {
            // DMX512 标准串口参数: 250000 8N2
            // 必须在 open 之后应用: 端口未打开时 setBaudRate 只缓存参数并恒返回 true,
            // 驱动是否支持要到真正下发时才校验。250000 为非标准波特率,
            // 部分适配器 (某些 CH340/PL2303 驱动) 不支持, 失败必须中止,
            // 否则会静默按错误波特率发帧 (现象为灯具乱闪/无响应)
            bool paramsOk = m_serial->setBaudRate(baudRate)
                            && m_serial->setDataBits(QSerialPort::Data8)
                            && m_serial->setParity(QSerialPort::NoParity)
                            && m_serial->setStopBits(QSerialPort::TwoStop)
                            && m_serial->setFlowControl(QSerialPort::NoFlowControl);
            if (!paramsOk || m_serial->baudRate() != baudRate) {
                errText = m_serial->errorString();
                LOG_ERROR(QString("DMX512 串口不支持 %1 8N2 参数: %2 - %3")
                              .arg(baudRate).arg(portName, errText));
                m_serial->close();
                delete m_serial;
                m_serial = nullptr;
            } else {
                m_portName = portName;
                m_framesSent = 0;
                m_hwBreakFailed = false;
                m_txClock.start();
                m_lastFrameStartUs = -1;
                // RDM 半双工接收: 仅 ReadWrite 模式实际产生 readyRead
                connect(m_serial, &QSerialPort::readyRead, this, &Dmx512Sender::rdmDataReady);
                LOG_INFO(QString("DMX512 串口已打开: %1 @ %2 baud (%3)")
                             .arg(portName).arg(baudRate)
                             .arg(mode == QIODevice::ReadWrite ? "读写/RDM" : "只写"));
                opened = true;
            }
        }
    }

    if (closedPrev) emit portClosed();
    if (opened) emit portOpened(portName);
    else if (!errText.isEmpty()) emit portError(errText);
    return opened;
}

void Dmx512Sender::close() {
    bool wasOpen = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_serial) {
            wasOpen = m_serial->isOpen();
            closeLocked();
        }
    }
    if (wasOpen) emit portClosed();
}

// 调用方必须已持有 m_mutex
void Dmx512Sender::closeLocked() {
    if (m_serial) {
        if (m_serial->isOpen()) m_serial->close();
        delete m_serial;
        m_serial = nullptr;
    }
    m_portName.clear();
}

QByteArray Dmx512Sender::buildDmxFrame(const QByteArray &channels) {
    // DMX512 帧结构:
    //   BREAK      90μs  (用 0x00 发送足够长时间模拟, 或者用 setBreakEnabled)
    //   MAB        12μs  (空闲)
    //   Start Code 1 byte (0x00)
    //   Channels   512 bytes
    
    QByteArray frame;
    frame.reserve(ArtNet::DMX_CHANNEL_COUNT + 1);
    
    // Start Code (固定 0x00)
    frame.append('\x00');
    
    // 通道数据
    QByteArray chData = channels.left(ArtNet::DMX_CHANNEL_COUNT);
    if (chData.size() < ArtNet::DMX_CHANNEL_COUNT) {
        chData.append(QByteArray(ArtNet::DMX_CHANNEL_COUNT - chData.size(), 0));
    }
    
    frame.append(chData);
    
    return frame;
}

// 发送 BREAK + MAB; 调用方必须已持有 m_mutex
bool Dmx512Sender::sendBreakLocked(int breakUs) {
    // DMX512 要求 BREAK (90±10μs) + MAB (12±2μs)
    // 不同 USB-DMX 设备对 BREAK 的实现支持程度不同
    //
    // 策略: 优先用平台 API 发送硬件 BREAK; 调用失败 (部分驱动不实现 break 控制,
    // 如旧版 ch341) 则记住状态并降级软件模拟, 避免静默发出无 BREAK 的帧

    bool hwBreakSent = false;
#ifdef Q_OS_WIN
    // Windows: 用 EscapeCommFunction 发送硬件 BREAK
    // (延时用 QueryPerformanceCounter 忙等, Sleep/usleep 精度 ~15.6ms 不可用)
    if (!m_hwBreakFailed) {
        HANDLE handle = m_serial->handle();
        if (handle != INVALID_HANDLE_VALUE) {
            const BOOL setOk = EscapeCommFunction(handle, SETBREAK);
            preciseDelayUs(breakUs);   // 保持 BREAK 低电平
            const BOOL clrOk = EscapeCommFunction(handle, CLRBREAK);
            if (setOk && clrOk) {
                hwBreakSent = true;
                preciseDelayUs(15);    // MAB
            } else {
                m_hwBreakFailed = true;
                LOG_WARN("DMX512 硬件 BREAK 不可用 (EscapeCommFunction 失败), 降级软件模拟 BREAK");
            }
        }
    }
#else
    // Linux/macOS: 用 TIOCSBRK/TIOCCBRK ioctl 强制 TX 拉低
    // (等价于 Windows SETBREAK/CLRBREAK; tcsendbreak(fd,0) 按 POSIX 固定约 0.25s,
    //  远超 DMX 90μs 规范, 不可用)
    if (!m_hwBreakFailed) {
        int fd = m_serial->handle();
        if (fd >= 0) {
            const bool setOk = (ioctl(fd, TIOCSBRK) == 0);
            preciseDelayUs(breakUs);   // BREAK
            const bool clrOk = (ioctl(fd, TIOCCBRK) == 0);
            if (setOk && clrOk) {
                hwBreakSent = true;
                preciseDelayUs(15);    // MAB
            } else {
                m_hwBreakFailed = true;
                LOG_WARN("DMX512 硬件 BREAK 不可用 (TIOCSBRK/TIOCCBRK 失败), 降级软件模拟 BREAK");
            }
        }
    }
#endif
    if (!hwBreakSent) {
        // 回退: 软件模拟 BREAK (注意: 0x00 字节含停止位间隙, 并非连续低电平,
        // 仅部分接收器可容忍; 优先保证硬件 BREAK 路径)
        m_serial->write("\x00\x00", 2);
        if (!m_serial->flush()) {
            LOG_WARN(QString("DMX512 BREAK 回退写入失败: %1").arg(m_serial->errorString()));
            return false;
        }
        preciseDelayUs(breakUs);
    }
    return true;
}

bool Dmx512Sender::sendFrame(const QByteArray &channels) {
    QMutexLocker locker(&m_mutex);

    if (!isOpen() || !m_serial) return false;

    QByteArray frame = buildDmxFrame(channels);

    if (!sendBreakLocked()) return false;

    // === 发送 Start Code (0x00) + 512 channels ===
    qint64 written = m_serial->write(frame);
    if (written != frame.size()) {
        LOG_WARN(QString("DMX512 写入不完整: %1/%2").arg(written).arg(frame.size()));
        return false;
    }

    // flush 失败通常意味着适配器已被拔出/写错误, 不能静默计入已发送帧数
    if (!m_serial->flush()) {
        LOG_WARN(QString("DMX512 flush 失败: %1").arg(m_serial->errorString()));
        return false;
    }

    m_framesSent++;
    return true;
}

bool Dmx512Sender::sendFrameAsync(const QByteArray &channels) {
    QMutexLocker locker(&m_mutex);

    if (!isOpen() || !m_serial) return false;

    // 忙判定: 上一帧字节仍在驱动 TX 队列/线路上发出时, SETBREAK 会插进帧中间,
    // 必须拒绝 (调用方保留数据下个节拍重试)
    const qint64 nowUs = m_txClock.isValid() ? m_txClock.nsecsElapsed() / 1000 : 0;
    if (m_lastFrameStartUs >= 0 && nowUs - m_lastFrameStartUs < kFrameUs)
        return false;

    QByteArray frame = buildDmxFrame(channels);

    if (!sendBreakLocked()) return false;

    const qint64 written = m_serial->write(frame);
    if (written != frame.size()) {
        LOG_WARN(QString("DMX512 异步写入不完整: %1/%2").arg(written).arg(frame.size()));
        return false;
    }

    // 不 flush: 字节留在驱动队列由硬件异步发出, 本调用立即返回
    m_lastFrameStartUs = nowUs;
    m_framesSent++;
    return true;
}

bool Dmx512Sender::sendRaw(const QByteArray &data) {
    QMutexLocker locker(&m_mutex);
    if (!isOpen() || !m_serial) return false;

    qint64 written = m_serial->write(data);
    bool flushed = m_serial->flush();
    return written == data.size() && flushed;
}

// 发送 E1.20 RDM 线上帧: BREAK + MAB + 完整包 (起始码/长度/头/数据/校验和) + flush.
// flush 必须: RDM 是一次性请求, 字节全部上线后才能进入应答接收窗口
bool Dmx512Sender::sendRdmFrame(const QByteArray &packet, int breakUs) {
    QMutexLocker locker(&m_mutex);
    if (!isOpen() || !m_serial) return false;

    if (!sendBreakLocked(breakUs)) return false;

    const qint64 written = m_serial->write(packet);
    if (written != packet.size()) {
        LOG_WARN(QString("RDM 帧写入不完整: %1/%2").arg(written).arg(packet.size()));
        return false;
    }
    if (!m_serial->flush()) {
        LOG_WARN(QString("RDM 帧 flush 失败: %1").arg(m_serial->errorString()));
        return false;
    }
    return true;
}

QByteArray Dmx512Sender::readAvailable() {
    QMutexLocker locker(&m_mutex);
    if (!m_serial) return QByteArray();
    return m_serial->readAll();
}

QStringList Dmx512Sender::availablePorts() {
    QStringList result;
    const auto infos = QSerialPortInfo::availablePorts();
    for (const auto &info : infos) {
        QString desc = info.description();
        // 过滤常见的 DMX 设备
        bool likelyDmx = false;
        desc = desc.toLower();
        if (desc.contains("ft232") || desc.contains("ftdi") || 
            desc.contains("dmx") || desc.contains("usb-serial") ||
            desc.contains("ch340") || desc.contains("cp210")) {
            likelyDmx = true;
        }
        result.append(QString("%1 (%2)%3")
            .arg(info.portName())
            .arg(info.description())
            .arg(likelyDmx ? " [DMX?]" : ""));
    }
    return result;
}

// ============================================================
// DmxSyncGroup — 多端口 BREAK 绝对时刻同步发送组
// ============================================================
#include <QThread>
#include <QWaitCondition>
#include <atomic>

// 单端口 worker 线程: 在本线程内创建并独占 Dmx512Sender (QSerialPort 非线程安全,
// 打开/写入均发生在 run() 内), GUI 线程只通过原子标志与互斥暂存区交互
class DmxSyncGroup::Impl : public QThread {
public:
    Impl(const QString &portName, qint32 baudRate)
        : m_portName(portName), m_baudRate(baudRate) {}

    // 协调者调用: 暂存帧与绝对目标时刻, 唤醒线程
    void stage(const QByteArray &frame, quint64 targetUs) {
        QMutexLocker locker(&m_mtx);
        m_frame = frame;
        m_targetUs = targetUs;
        m_gen++;
        m_cond.wakeAll();
    }
    void requestQuit() {
        QMutexLocker locker(&m_mtx);
        m_quit = true;
        m_cond.wakeAll();
    }
    bool ready() const { return m_ready.load(); }        // 打开流程已结束 (成功或失败)
    bool opened() const { return m_opened.load(); }
    quint64 sent() const { return m_sent.load(); }

protected:
    void run() override {
#ifdef Q_OS_WIN
        // 提高优先级: 降低调度抢占抖动, 保证各线程自旋到点后及时进入 BREAK
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
        if (m_portName.isEmpty()) { m_ready = true; return; }   // 空闲槽位

        Dmx512Sender sender;
        m_opened = sender.open(m_portName, m_baudRate);
        m_ready = true;
        if (!m_opened) return;

        quint64 seen = 0;
        for (;;) {
            QByteArray frame;
            quint64 target = 0;
            {
                QMutexLocker locker(&m_mtx);
                while (!m_quit && m_gen == seen)
                    m_cond.wait(&m_mtx);
                if (m_quit) break;
                seen = m_gen;
                frame = m_frame;
                target = m_targetUs;
            }
            // 自旋等待绝对时刻: 线程唤醒延迟被 commit 的提前量吸收,
            // 到点后全部线程在同一 QPC 刻度附近进入 BREAK (偏差数十微秒级)
            while (DmxSyncGroup::nowUs() < target) {}
            if (sender.sendFrameAsync(frame))
                m_sent.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    QString m_portName;
    qint32 m_baudRate;
    QMutex m_mtx;
    QWaitCondition m_cond;
    QByteArray m_frame;
    quint64 m_targetUs = 0;
    quint64 m_gen = 0;
    bool m_quit = false;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_opened{false};
    std::atomic<quint64> m_sent{0};
};

QStringList DmxSyncGroup::start(const QStringList &portNames, qint32 baudRate) {
    stop();
    QStringList failed;
    m_workers.reserve(int(portNames.size()));
    for (const QString &name : portNames) {
        auto *w = new Impl(name, baudRate);
        w->start();
        m_workers.append(w);
    }
    // 等待全部 worker 完成串口打开 (单口 open 含驱动调用, 上限 3s 防异常驱动卡死 UI)
    QElapsedTimer et;
    et.start();
    for (Impl *w : m_workers) {
        while (!w->ready() && et.elapsed() < 3000)
            QThread::msleep(2);
    }
    for (int i = 0; i < m_workers.size(); ++i) {
        Impl *w = m_workers[i];
        const bool active = !portNames[i].isEmpty();
        if (!w->ready()) {
            // 打开流程超时: 记失败并回收该线程 (驱动挂死等极端情况)
            w->requestQuit();
            if (!w->wait(1000)) w->terminate();
            w->wait();
            delete w;
            m_workers[i] = nullptr;
            if (active) failed << portNames[i];
        } else if (active && !w->opened()) {
            failed << portNames[i];
        }
    }
    m_running = !m_workers.isEmpty();
    return failed;
}

void DmxSyncGroup::stop() {
    m_running = false;
    for (Impl *w : m_workers)
        if (w) w->requestQuit();
    for (Impl *w : m_workers) {
        if (!w) continue;
        if (!w->wait(3000)) w->terminate();
        w->wait();
        delete w;
    }
    m_workers.clear();
}

bool DmxSyncGroup::isOpen(int i) const {
    return i >= 0 && i < m_workers.size() && m_workers[i] && m_workers[i]->opened();
}

quint64 DmxSyncGroup::framesSent(int i) const {
    return i >= 0 && i < m_workers.size() && m_workers[i] ? m_workers[i]->sent() : 0;
}

void DmxSyncGroup::commit(const QVector<QByteArray> &frames, quint64 targetUs) {
    if (!m_running) return;
    for (int i = 0; i < m_workers.size(); ++i) {
        Impl *w = m_workers[i];
        if (w && w->opened())
            w->stage(frames.value(i), targetUs);
    }
}

quint64 DmxSyncGroup::nowUs() {
#ifdef Q_OS_WIN
    static LARGE_INTEGER freq = []{ LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    return quint64(double(cnt.QuadPart) * 1000000.0 / double(freq.QuadPart));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return quint64(ts.tv_sec) * 1000000ull + quint64(ts.tv_nsec) / 1000ull;
#endif
}

// ============================================================
// DmxRxGroup — 多端口 DMX512 接收线程组
// ============================================================
#include <QSerialPort>
#include <QDateTime>
#include <functional>

// 单端口接收 worker: 线程内创建 QSerialPort 并阻塞式 waitForReadyRead 循环组帧,
// 完整帧经 frameReady 队列信号送 GUI; 高频字节读取与帧对齐不占用主线程
class DmxRxGroup::Impl : public QThread {
public:
    // sink 在 worker 线程被调用; 由 DmxRxGroup 传入 (内部 emit frameReady,
    // 接收方在 GUI 线程 → 自动队列投递)
    Impl(int slot, const QString &portName, int frameFormat, qint32 baudRate,
         std::function<void(int, QByteArray, qint64)> sink)
        : m_slot(slot), m_portName(portName), m_frameFormat(frameFormat),
          m_baudRate(baudRate), m_sink(std::move(sink)) {}

    void requestQuit() { m_quit.store(true); }
    bool ready() const { return m_ready.load(); }
    bool opened() const { return m_opened.load(); }
    quint64 received() const { return m_recv.load(); }
    quint64 dropped() const { return m_dropped.load(); }

protected:
    void run() override {
        if (m_portName.isEmpty()) { m_ready = true; return; }   // 空闲槽位

        QSerialPort sp;
        sp.setPortName(m_portName);
        if (!sp.open(QIODevice::ReadOnly)) { m_ready = true; return; }
        const bool paramsOk = sp.setBaudRate(m_baudRate)
                              && sp.setDataBits(QSerialPort::Data8)
                              && sp.setParity(QSerialPort::NoParity)
                              && sp.setStopBits(QSerialPort::TwoStop)
                              && sp.setFlowControl(QSerialPort::NoFlowControl);
        if (!paramsOk || sp.baudRate() != m_baudRate) { m_ready = true; return; }
        m_opened = true;
        m_ready = true;

        // 帧长与对齐规则同原 GUI 线程实现:
        //   fmt0: 513 = 0x00+512; fmt1: 514 = 前导1字节+0x00+512; fmt2: 512 无起始码
        const int frameLen = (m_frameFormat == 1) ? 514 : (m_frameFormat == 2 ? 512 : 513);
        QByteArray buf;
        while (!m_quit.load()) {
            if (!sp.waitForReadyRead(100)) continue;   // 100ms 轮询退出标志
            buf += sp.readAll();
            while (buf.size() >= frameLen) {
                if (m_quit.load()) break;
                QByteArray dmx;
                int consume = frameLen;
                if (m_frameFormat == 2) {
                    dmx = buf.left(512);
                } else if (uchar(buf.at(0)) == 0x00) {
                    dmx = buf.mid(1, 512);
                } else if (frameLen >= 514 || uchar(buf.at(1)) == 0x00) {
                    dmx = buf.mid(2, 512);   // 514 格式跳过前导字节; 513 格式起始码错位 1 字节
                } else {
                    buf.remove(0, 1);        // 失步: 丢 1 字节重找起始码
                    m_dropped.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (dmx.size() < 512) dmx.append(QByteArray(512 - dmx.size(), 0));
                buf.remove(0, consume);
                m_recv.fetch_add(1, std::memory_order_relaxed);
                m_sink(m_slot, dmx, QDateTime::currentMSecsSinceEpoch() * 1000);
            }
            // 防积压: 缓冲远超一帧说明消费端跟不上或噪声, 保留尾部重新对齐
            if (buf.size() > frameLen * 4) buf = buf.right(frameLen * 2);
        }
        m_opened = false;
    }

private:
    int m_slot;
    QString m_portName;
    int m_frameFormat;
    qint32 m_baudRate;
    std::function<void(int, QByteArray, qint64)> m_sink;
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_opened{false};
    std::atomic<quint64> m_recv{0};
    std::atomic<quint64> m_dropped{0};
};

DmxRxGroup::DmxRxGroup(QObject *parent) : QObject(parent) {}

QStringList DmxRxGroup::start(const QStringList &portNames, int frameFormat, qint32 baudRate) {
    stop();
    QStringList failed;
    m_workers.reserve(int(portNames.size()));
    for (int i = 0; i < portNames.size(); ++i) {
        auto *w = new Impl(i, portNames[i], frameFormat, baudRate,
                           [this](int slot, QByteArray dmx, qint64 arrivedUs) {
                               emit frameReady(slot, dmx, arrivedUs);
                           });
        w->start();
        m_workers.append(w);
    }
    // 等待全部 worker 完成打开 (上限 3s 防异常驱动卡死 UI)
    QElapsedTimer et;
    et.start();
    for (Impl *w : m_workers) {
        while (!w->ready() && et.elapsed() < 3000)
            QThread::msleep(2);
    }
    for (int i = 0; i < m_workers.size(); ++i) {
        Impl *w = m_workers[i];
        const bool active = !portNames[i].isEmpty();
        if (!w->ready()) {
            w->requestQuit();
            if (!w->wait(1000)) w->terminate();
            w->wait();
            delete w;
            m_workers[i] = nullptr;
            if (active) failed << portNames[i];
        } else if (active && !w->opened()) {
            failed << portNames[i];
        }
    }
    m_running = !m_workers.isEmpty();
    return failed;
}

void DmxRxGroup::stop() {
    m_running = false;
    for (Impl *w : m_workers)
        if (w) w->requestQuit();
    for (Impl *w : m_workers) {
        if (!w) continue;
        if (!w->wait(1000)) w->terminate();
        w->wait();
        delete w;
    }
    m_workers.clear();
}

bool DmxRxGroup::isOpen(int i) const {
    return i >= 0 && i < m_workers.size() && m_workers[i] && m_workers[i]->opened();
}

quint64 DmxRxGroup::framesReceived(int i) const {
    return i >= 0 && i < m_workers.size() && m_workers[i] ? m_workers[i]->received() : 0;
}

quint64 DmxRxGroup::droppedBytes(int i) const {
    return i >= 0 && i < m_workers.size() && m_workers[i] ? m_workers[i]->dropped() : 0;
}
