// AudioFileClock.cpp — 占位, 行为和 VideoFileClock 同构
// 真正的 LTC 解码需要 libltc, 将来扩展. 当前用 audio PTS 模式.
#include "AudioFileClock.h"
#include "../../core/Logger.h"

AudioFileClock::AudioFileClock(QObject *parent)
    : ISyncClock(parent) {
    m_pollTimer = new QTimer(this);
    m_pollTimer->setSingleShot(false);
    connect(m_pollTimer, &QTimer::timeout, this, &AudioFileClock::onPoll);
}

AudioFileClock::~AudioFileClock() { stop(); }

bool AudioFileClock::start(const SyncClockConfig &config) {
    m_config = config;
    if (config.mediaFilePath.isEmpty()) {
        emit errorOccurred(tr("AudioFileClock 需要 mediaFilePath"));
        return false;
    }

    m_decoder = new FFmpegDecoder();
    if (!m_decoder->open(config.mediaFilePath, /*video=*/false, /*audio=*/true)) {
        emit errorOccurred(tr("AudioFileClock: 无法打开 %1").arg(config.mediaFilePath));
        delete m_decoder; m_decoder = nullptr; return false;
    }

    m_frameRate = config.frameRate > 0 ? config.frameRate : 30;
    m_lastTsUs.store(-1);
    m_firstFrame = true;
    m_running.store(true);

    int pollMs = qMax(1, 1000 / (m_frameRate * 2));
    m_pollTimer->start(pollMs);

    LOG_INFO(QString("[AudioFileClock] start: %1 (LTC 模式, 实际 LTC 解码待实现)")
                 .arg(config.mediaFilePath));
    emit statusChanged(tr("音频时钟已启动 (PTS 模式)"));
    return true;
}

void AudioFileClock::stop() {
    m_pollTimer->stop();
    m_running.store(false);
    m_lastTsUs.store(-1);
    if (m_decoder) { m_decoder->close(); delete m_decoder; m_decoder = nullptr; }
}

qint64 AudioFileClock::currentTimestampUs() const { return m_lastTsUs.load(); }

void AudioFileClock::onPoll() {
    if (!m_decoder) return;
    qint64 pts = m_decoder->decodeNextFrame(1);   // 1 = 音频流
    if (pts < 0) {
        if (m_config.loopOnEnd) {
            m_decoder->seek(0); m_firstFrame = true; m_startPtsUs = 0;
            emit clockLooped(1); return;
        }
        m_running.store(false); m_pollTimer->stop();
        emit clockFinished(); return;
    }
    if (m_firstFrame) { m_startPtsUs = pts; m_firstFrame = false; }
    qint64 offset = pts - m_startPtsUs;
    m_lastTsUs.store(offset);
    qint64 idx = offset * m_frameRate / 1000000;
    emit frameTick(idx, offset);
}
