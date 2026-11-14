// Dmx512Sender.cpp
#include "Dmx512Sender.h"
#include "../core/Logger.h"
#include <QSerialPortInfo>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#endif

Dmx512Sender::Dmx512Sender(QObject *parent)
    : QObject(parent) {
}

Dmx512Sender::~Dmx512Sender() {
    close();
}

bool Dmx512Sender::open(const QString &portName, qint32 baudRate) {
    if (isOpen()) {
        if (m_portName == portName) return true;
        close();
    }
    
    m_serial = new QSerialPort(this);
    m_serial->setPortName(portName);
    
    // DMX512 标准串口参数: 250000 8N2
    m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::TwoStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);
    
    if (!m_serial->open(QIODevice::WriteOnly)) {
        QString err = m_serial->errorString();
        LOG_ERROR(QString("DMX512 串口打开失败: %1 - %2").arg(portName, err));
        emit portError(err);
        delete m_serial;
        m_serial = nullptr;
        return false;
    }
    
    m_portName = portName;
    m_framesSent = 0;
    
    LOG_INFO(QString("DMX512 串口已打开: %1 @ %2 baud").arg(portName).arg(baudRate));
    emit portOpened(portName);
    
    return true;
}

void Dmx512Sender::close() {
    if (m_serial) {
        if (m_serial->isOpen()) {
            m_serial->close();
            emit portClosed();
        }
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

bool Dmx512Sender::sendFrame(const QByteArray &channels) {
    QMutexLocker locker(&m_mutex);
    
    if (!isOpen() || !m_serial) return false;
    
    QByteArray frame = buildDmxFrame(channels);
    
    // === 发送 BREAK + MAB ===
    // DMX512 要求 BREAK (90±10μs) + MAB (12±2μs)
    // 不同 USB-DMX 设备对 BREAK 的实现支持程度不同
    // 
    // 策略: 优先用平台 API 发送硬件 BREAK, 失败则用软件模拟
    
#ifdef Q_OS_WIN
    // Windows: 用 EscapeCommFunction 发送硬件 BREAK
    HANDLE handle = m_serial->handle();
    if (handle != INVALID_HANDLE_VALUE) {
        EscapeCommFunction(handle, SETBREAK);
        QThread::usleep(100);  // 保持 100μs 低电平
        EscapeCommFunction(handle, CLRBREAK);
        QThread::usleep(15);   // MAB
    } else {
        // 回退: 软件模拟 BREAK
        // 写 0x00 字节 + 足够长的延时
        m_serial->write("\x00\x00", 2);
        QThread::usleep(100);
        m_serial->flush();
    }
#else
    // Linux/macOS: 用 tcsetattr 发送 BREAK
    int fd = m_serial->handle();
    if (fd >= 0) {
        // 尝试用 tcflow 或 tcsendbreak
        tcsendbreak(fd, 0);  // POSIX BREAK
        QThread::usleep(15); // MAB
    } else {
        m_serial->write("\x00\x00", 2);
        QThread::usleep(100);
        m_serial->flush();
    }
#endif
    
    // === 发送 Start Code (0x00) + 512 channels ===
    qint64 written = m_serial->write(frame);
    if (written != frame.size()) {
        LOG_WARN(QString("DMX512 写入不完整: %1/%2").arg(written).arg(frame.size()));
        return false;
    }
    
    m_serial->flush();
    
    m_framesSent++;
    return true;
}

bool Dmx512Sender::sendRaw(const QByteArray &data) {
    QMutexLocker locker(&m_mutex);
    if (!isOpen() || !m_serial) return false;
    
    qint64 written = m_serial->write(data);
    m_serial->flush();
    return written == data.size();
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
