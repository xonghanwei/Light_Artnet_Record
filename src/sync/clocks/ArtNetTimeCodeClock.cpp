// ArtNetTimeCodeClock.cpp
#include "ArtNetTimeCodeClock.h"
#include "../../core/Logger.h"

ArtNetTimeCodeClock::ArtNetTimeCodeClock(QObject *parent)
    : ISyncClock(parent) {
    m_socket = new QUdpSocket(this);
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(false);
    m_timeoutTimer->setInterval(2000);  // 2 秒没收到新 TC → clockFinished
    connect(m_timeoutTimer, &QTimer::timeout, this, &ArtNetTimeCodeClock::onTimeout);
    connect(m_socket, &QUdpSocket::readyRead, this, &ArtNetTimeCodeClock::onReadyRead);
}

ArtNetTimeCodeClock::~ArtNetTimeCodeClock() { stop(); }

bool ArtNetTimeCodeClock::start(const SyncClockConfig &config) {
    m_config = config;
    m_frameRate = config.frameRate > 0 ? config.frameRate : 30;
    m_lastTsUs.store(-1);
    m_hasReceived.store(false);

    if (!m_socket->bind(QHostAddress(config.artNetBindAddress), config.artNetPort)) {
        emit errorOccurred(tr("ArtNetTimeCodeClock: 绑定 %1:%2 失败")
                               .arg(config.artNetBindAddress).arg(config.artNetPort));
        return false;
    }

    m_running.store(true);
    m_timeoutTimer->start();
    m_lastReceive.start();

    LOG_INFO(QString("[ArtNetTimeCodeClock] start: bind %1:%2, expected fps=%3")
                 .arg(config.artNetBindAddress).arg(config.artNetPort).arg(m_frameRate));
    emit statusChanged(tr("ArtNet TimeCode 监听: %1:%2").arg(config.artNetBindAddress).arg(config.artNetPort));
    return true;
}

void ArtNetTimeCodeClock::stop() {
    m_timeoutTimer->stop();
    m_running.store(false);
    m_socket->close();
    m_lastTsUs.store(-1);
    LOG_INFO("[ArtNetTimeCodeClock] stop");
}

qint64 ArtNetTimeCodeClock::currentTimestampUs() const {
    return m_lastTsUs.load();
}

bool ArtNetTimeCodeClock::parseArtNetTC(const QByteArray &data, ParsedTC &out) {
    // 最小 ArtNet TimeCode 包: opcode(2) + version(2) + seq(1) + type(1) + tc(4) + filler(3) = 13
    if (data.size() < 13) return false;

    const quint8 *d = reinterpret_cast<const quint8*>(data.constData());
    quint16 opcode = quint16(d[0]) | (quint16(d[1]) << 8);
    if (opcode != OPCODE_ARTTIME) return false;

    // d[6] = type + seq (低 4bit = seq, 高 4bit = smpte type)
    quint8 smpteType = (d[6] >> 4) & 0x0F;
    // d[7] = hour (bit 7 = drop frame)
    out.hours      = d[7] & 0x3F;
    out.dropFrame  = bool(d[7] & 0x80);
    out.minutes    = d[8] & 0x3F;
    out.seconds    = d[9] & 0x3F;
    out.frames     = d[10] & 0x1F;

    switch (smpteType) {
        case 0: out.fps = 24; break;
        case 1: out.fps = 25; break;
        case 2: out.fps = 29; out.dropFrame = true; break;
        case 3: out.fps = 30; break;
        default: out.fps = 30; break;
    }
    return true;
}

void ArtNetTimeCodeClock::onReadyRead() {
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(m_socket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;
        m_socket->readDatagram(data.data(), data.size(), &sender, &senderPort);

        ParsedTC tc;
        if (!parseArtNetTC(data, tc)) continue;

        m_lastReceive.restart();
        m_timeoutTimer->start();  // 重置超时

        qint64 ts = tc.toUs();
        m_lastTsUs.store(ts);

        qint64 idx = ts * m_frameRate / 1000000;
        emit frameTick(idx, ts);

        if (!m_hasReceived.exchange(true)) {
            LOG_INFO(QString("[ArtNetTimeCodeClock] 收到首个 TC: %1:%2:%3:%4 @%5fps").arg(tc.hours).arg(tc.minutes).arg(tc.seconds).arg(tc.frames).arg(tc.fps));
        }
    }
}

void ArtNetTimeCodeClock::onTimeout() {
    if (!m_running.load()) return;
    // 超过 2 秒没收到新 TC
    emit errorOccurred(tr("ArtNet TimeCode 超时 (2s 没收到新包)"));
    m_running.store(false);
    m_timeoutTimer->stop();
    emit clockFinished();
}
