// Dmx512Source.cpp - DMX512 串口被动接收
#include "Dmx512Source.h"
#include "../../core/Logger.h"
#include <QSerialPortInfo>
#include <QDateTime>

Dmx512Source::Dmx512Source(QObject *parent)
    : IProtocolSource(parent) {
}

Dmx512Source::~Dmx512Source() {
    stop();
}

bool Dmx512Source::start(const ConnectionParams &params) {
    if (m_running) return false;
    
    if (params.serialPortName.isEmpty()) {
        LOG_ERROR("[Dmx512Source] 未指定串口名");
        emit errorOccurred("需要 --serial-port 参数");
        return false;
    }
    
    const qint32 baud = params.serialBaudRate > 0 ? params.serialBaudRate : 250000;

    m_serial = new QSerialPort(this);
    m_serial->setPortName(params.serialPortName);

    connect(m_serial, &QSerialPort::readyRead,
            this, &Dmx512Source::onReadyRead);

    if (!m_serial->open(QIODevice::ReadOnly)) {
        LOG_ERROR(QString("[Dmx512Source] 打开 %1 失败: %2")
            .arg(params.serialPortName, m_serial->errorString()));
        emit errorOccurred(m_serial->errorString());
        delete m_serial;
        m_serial = nullptr;
        return false;
    }

    // 必须在 open 之后应用参数: 端口未打开时 setBaudRate 只缓存并恒返回 true,
    // 驱动是否支持 250000 (非标准波特率) 要到真正下发时才校验
    bool paramsOk = m_serial->setBaudRate(baud)
                    && m_serial->setDataBits(QSerialPort::Data8)
                    && m_serial->setParity(QSerialPort::NoParity)
                    && m_serial->setStopBits(QSerialPort::TwoStop)
                    && m_serial->setFlowControl(QSerialPort::NoFlowControl);
    if (!paramsOk || m_serial->baudRate() != baud) {
        LOG_ERROR(QString("[Dmx512Source] 串口不支持 %1 8N2 参数: %2 - %3")
            .arg(baud).arg(params.serialPortName, m_serial->errorString()));
        emit errorOccurred(m_serial->errorString());
        m_serial->close();
        delete m_serial;
        m_serial = nullptr;
        return false;
    }

    m_running = true;
    m_framesReceived = 0;
    m_filterUniverses = params.filterUniverses;
    m_filterEnabled = !params.filterUniverses.isEmpty();
    
    LOG_INFO(QString("[Dmx512Source] 已打开 %1 @ %2 baud (只读模式)")
        .arg(params.serialPortName).arg(m_serial->baudRate()));
    emit statusChanged(QString("DMX512 reading from %1").arg(params.serialPortName));
    
    return true;
}

void Dmx512Source::stop() {
    if (!m_running) return;
    
    m_running = false;
    if (m_serial) {
        m_serial->close();
        delete m_serial;
        m_serial = nullptr;
    }
    
    LOG_INFO("[Dmx512Source] 已停止");
    emit statusChanged("DMX512 stopped");
}

void Dmx512Source::setFilterUniverses(const QList<quint16> &universes) {
    // DMX512 没有 Universe 概念, 这里只存储不做过滤
    Q_UNUSED(universes);
}

void Dmx512Source::onReadyRead() {
    if (!m_serial) return;
    processSerialData();
}

void Dmx512Source::processSerialData() {
    if (!m_serial) return;
    
    QByteArray incoming = m_serial->readAll();
    m_readBuffer.append(incoming);
    
    // DMX512 每帧固定 513 字节 (Start Code 0x00 + 512 channels)
    // 串口层可能把一帧拆成多次 read, 所以用缓冲区对齐
    while (m_readBuffer.size() >= 513) {
        // 尝试找到 Start Code (0x00) 作为帧对齐标记
        int startIdx = -1;
        for (int i = 0; i < m_readBuffer.size() && i < 10; ++i) {
            if (quint8(m_readBuffer[i]) == 0x00) {
                startIdx = i;
                break;
            }
        }
        
        if (startIdx < 0) {
            // 没找到 Start Code, 丢弃全部
            m_readBuffer.clear();
            break;
        }
        
        if (startIdx > 0) {
            m_readBuffer = m_readBuffer.mid(startIdx);
        }
        
        if (m_readBuffer.size() < 513) break;
        
        // 取出一帧: Start Code + 512 channels
        QByteArray frame = m_readBuffer.left(513);
        m_readBuffer = m_readBuffer.mid(513);
        
        LightFrame lf;
        lf.sourceProtocol = ProtocolType::Dmx512;
        lf.universe = 0;  // DMX512 没有多 Universe
        lf.universeRaw = 0;
        lf.dmxData = frame.mid(1, 512); // 跳过 Start Code
        lf.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        lf.sequence = quint32(m_framesReceived);
        
        m_framesReceived++;
        emit frameReceived(lf);
    }
}
