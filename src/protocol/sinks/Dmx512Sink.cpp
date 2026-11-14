// Dmx512Sink.cpp
#include "Dmx512Sink.h"
#include "../../core/Logger.h"

Dmx512Sink::Dmx512Sink(QObject *parent)
    : IProtocolSink(parent) {
}

Dmx512Sink::~Dmx512Sink() {
    stop();
}

bool Dmx512Sink::start(const ConnectionParams &params) {
    if (params.serialPortName.isEmpty()) {
        emit sendError("DMX512Sink: 未指定串口名 (serialPortName)");
        return false;
    }
    
    qint32 baud = params.serialBaudRate > 0 ? params.serialBaudRate : 250000;
    bool ok = m_sender.open(params.serialPortName, baud);
    if (ok) {
        QString info = QString("DMX512Sink 已启动 (%1 @ %2)")
            .arg(params.serialPortName).arg(baud);
        LOG_INFO(info);
        emit started(info);
    } else {
        emit sendError(QString("DMX512Sink 打开串口失败: %1").arg(params.serialPortName));
    }
    return ok;
}

void Dmx512Sink::stop() {
    m_sender.close();
    LOG_INFO("DMX512Sink 已停止");
    emit stopped();
}

bool Dmx512Sink::isRunning() const {
    return m_sender.isOpen();
}

bool Dmx512Sink::sendFrame(const LightFrame &frame) {
    QByteArray dmx = frame.paddedDmx();
    bool ok = m_sender.sendFrame(dmx);
    if (ok) {
        emit frameSent(frame.universe, 1);
    }
    return ok;
}
