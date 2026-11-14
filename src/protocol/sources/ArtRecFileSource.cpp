// ArtRecFileSource.cpp
#include "ArtRecFileSource.h"
#include "../../core/Logger.h"

ArtRecFileSource::ArtRecFileSource(QObject *parent)
    : IProtocolSource(parent) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(false);
    connect(m_timer, &QTimer::timeout, this, &ArtRecFileSource::onTick);
}

ArtRecFileSource::~ArtRecFileSource() {
    stop();
}

bool ArtRecFileSource::start(const ConnectionParams &params) {
    m_params = params;
    m_filterUniverses = params.filterUniverses;
    
    if (params.filePath.isEmpty()) {
        QString err = tr("[ArtRecFileSource] 未指定 filePath");
        emit errorOccurred(err);
        LOG_ERROR(err);
        return false;
    }
    
    if (!m_reader.open(params.filePath)) {
        QString err = tr("[ArtRecFileSource] 打开文件失败: %1").arg(params.filePath);
        emit errorOccurred(err);
        LOG_ERROR(err);
        return false;
    }
    
    // 计算 tick 间隔: 文件原始帧率 / playbackSpeed
    quint16 fps = m_reader.header().frameRate;
    if (fps == 0) fps = 30;
    qreal speed = params.playbackSpeed > 0 ? params.playbackSpeed : 1.0;
    
    qint64 intervalMs = qMax<qint64>(1, qint64(1000.0 / (fps * speed) + 0.5));
    
    LOG_INFO(QString("[ArtRecFileSource] 开始重演: %1, fps=%2, speed=%3x, interval=%4ms, totalFrames=%5, loop=%6")
                 .arg(params.filePath)
                 .arg(fps)
                 .arg(speed, 0, 'f', 2)
                 .arg(intervalMs)
                 .arg(m_reader.header().totalFrames)
                 .arg(params.loopPlayback ? "yes" : "no"));
    
    m_totalFramesEmitted = 0;
    m_ticksProcessed = 0;
    
    emit statusChanged(tr("重演开始: %1 (%2 fps)").arg(params.filePath).arg(fps));
    
    m_timer->start(int(intervalMs));
    return true;
}

void ArtRecFileSource::stop() {
    if (m_timer && m_timer->isActive()) {
        m_timer->stop();
        LOG_INFO(QString("[ArtRecFileSource] 停止重演, 已 emit %1 个 LightFrame").arg(m_totalFramesEmitted));
        emit statusChanged(tr("重演停止"));
    }
    m_reader.close();
}

void ArtRecFileSource::onTick() {
    qint64 frameIndex;
    qint64 timestampUs;
    QMap<int, QByteArray> universeData;
    
    if (!m_reader.readNextFrame(frameIndex, timestampUs, universeData)) {
        // 文件到末尾
        if (m_params.loopPlayback) {
            if (!m_reader.seekToFrame(0)) {
                LOG_WARN("[ArtRecFileSource] seek(0) 失败, 停止重演");
                stop();
                emit errorOccurred(tr("loop seek 失败"));
                return;
            }
            // 重读一帧
            if (!m_reader.readNextFrame(frameIndex, timestampUs, universeData)) {
                stop();
                return;
            }
            LOG_INFO("[ArtRecFileSource] loop: 回到帧 0");
        } else {
            LOG_INFO("[ArtRecFileSource] 文件到末尾, 重演结束");
            stop();
            emit playbackFinished();
            return;
        }
    }
    
    ++m_ticksProcessed;
    
    // 该帧可能含多个 universe, 每个 universe emit 一个 LightFrame
    for(auto it = universeData.constBegin(); it != universeData.constEnd(); ++it) {
        int uniIndex = it.key();
        const QByteArray &data = it.value();
        
        // universe index → encoded universe
        quint16 encodedUniverse = 0;
        if (uniIndex >= 0 && uniIndex < m_reader.universes().size()) {
            encodedUniverse = m_reader.universes()[uniIndex].encoded();
        }
        
        // Universe 过滤
        if (!m_filterUniverses.isEmpty() && !m_filterUniverses.contains(encodedUniverse))
            continue;
        
        LightFrame lf;
        lf.sourceProtocol = ProtocolType::ArtRecFile;
        lf.universe = encodedUniverse;
        lf.universeRaw = quint8(encodedUniverse & 0x0F);
        lf.dmxData = data;
        lf.timestampUs = timestampUs;
        lf.sequence = quint32(frameIndex & 0xFFFFFFFF);
        
        emit frameReceived(lf);
        ++m_totalFramesEmitted;
    }
}
