// ArtNetSource.cpp
#include "ArtNetSource.h"
#include "../../core/Logger.h"

ArtNetSource::ArtNetSource(QObject *parent)
    : IProtocolSource(parent) {
    m_receiver = new ArtNetReceiver(this);
    connect(m_receiver, &ArtNetReceiver::dmxFrameReceived,
            this, &ArtNetSource::onArtNetDmxReceived);
    connect(m_receiver, &ArtNetReceiver::statusChanged,
            this, &ArtNetSource::onArtNetStatusChanged);
}

ArtNetSource::~ArtNetSource() {
    stop();
}

bool ArtNetSource::start(const ConnectionParams &params) {
    quint16 port = params.localPort > 0 ? params.localPort : ArtNet::DEFAULT_PORT;
    QString bindAddr = params.localBindAddress.isEmpty() ? "0.0.0.0" : params.localBindAddress;
    
    LOG_INFO(QString("[ArtNetSource] 启动监听 %1:%2").arg(bindAddr).arg(port));
    
    // 先设过滤再启动
    if (!params.filterUniverses.isEmpty()) {
        m_receiver->setFilterUniverses(params.filterUniverses);
    }
    
    return m_receiver->start(port, bindAddr);
}

void ArtNetSource::stop() {
    if (m_receiver) m_receiver->stop();
}

void ArtNetSource::setFilterUniverses(const QList<quint16> &universes) {
    m_receiver->setFilterUniverses(universes);
}

void ArtNetSource::onArtNetDmxReceived(const ArtNetDmxFrame &frame) {
    LightFrame lf;
    lf.sourceProtocol = ProtocolType::ArtNet;
    lf.universe = frame.universe;
    lf.universeRaw = quint8(frame.universe & 0x0F); // ArtNet Universe 低 4 bit
    lf.dmxData = QByteArray(reinterpret_cast<const char*>(frame.dmxData), 
                            qMin(int(frame.length), ArtNet::DMX_CHANNEL_COUNT));
    lf.timestampUs = frame.receiveTimeUs;
    lf.sequence = frame.sequence;
    
    emit frameReceived(lf);
}

void ArtNetSource::onArtNetStatusChanged(const QString &status) {
    emit statusChanged(status);
}
