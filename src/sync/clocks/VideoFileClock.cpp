// VideoFileClock.cpp
#include "VideoFileClock.h"
#include "../../core/Logger.h"

VideoFileClock::VideoFileClock(QObject *parent)
    : ISyncClock(parent) {
    m_pollTimer = new QTimer(this);
    m_pollTimer->setSingleShot(false);
    connect(m_pollTimer, &QTimer::timeout, this, &VideoFileClock::onPoll);
}

VideoFileClock::~VideoFileClock() {
    stop();
}

bool VideoFileClock::start(const SyncClockConfig &config) {
    m_config = config;

    if (config.mediaFilePath.isEmpty()) {
        emit errorOccurred(tr("VideoFileClock 需要 mediaFilePath"));
        return false;
    }

    m_decoder = new FFmpegDecoder();
    if (!m_decoder->open(config.mediaFilePath, /*openVideo=*/true, /*openAudio=*/false)) {
        emit errorOccurred(tr("VideoFileClock: 无法打开 %1").arg(config.mediaFilePath));
        delete m_decoder;
        m_decoder = nullptr;
        return false;
    }

    m_frameRate = config.frameRate > 0 ? config.frameRate : m_decoder->videoFrameRate();
    m_lastTsUs.store(-1);
    m_firstFrame = true;
    m_looped = 0;

    LOG_INFO(QString("[VideoFileClock] start: %1, fps=%2, loop=%3")
                 .arg(config.mediaFilePath).arg(m_frameRate)
                 .arg(config.loopOnEnd ? "yes" : "no"));

    m_running.store(true);

    // 用稍快的间隔轮询, 取到新帧就 emit frameTick
    int pollMs = qMax(1, 1000 / (m_frameRate * 2));
    m_pollTimer->start(pollMs);

    emit statusChanged(tr("视频时钟已启动: %1 (%2 fps)").arg(config.mediaFilePath).arg(m_frameRate));
    return true;
}

void VideoFileClock::stop() {
    m_pollTimer->stop();
    m_running.store(false);
    m_lastTsUs.store(-1);
    if (m_decoder) {
        m_decoder->close();
        delete m_decoder;
        m_decoder = nullptr;
    }
    LOG_INFO("[VideoFileClock] stop");
}

qint64 VideoFileClock::currentTimestampUs() const {
    return m_lastTsUs.load();
}

int VideoFileClock::frameRate() const {
    return m_frameRate;
}

void VideoFileClock::onPoll() {
    if (!m_decoder) return;

    qint64 pts = m_decoder->decodeNextFrame(0);
    if (pts < 0) {
        // EOF
        if (m_config.loopOnEnd) {
            m_decoder->seek(0);
            m_firstFrame = true;
            m_startPtsUs = 0;
            ++m_looped;
            emit clockLooped(m_looped);
            LOG_INFO(QString("[VideoFileClock] loop #%1").arg(m_looped));
            return;
        }
        // 结束
        LOG_INFO("[VideoFileClock] 视频已到 EOF");
        m_running.store(false);
        m_pollTimer->stop();
        emit clockFinished();
        return;
    }

    if (m_firstFrame) {
        m_startPtsUs = pts;
        m_firstFrame = false;
    }

    qint64 offset = pts - m_startPtsUs;
    m_lastTsUs.store(offset);

    qint64 idx = offset * m_frameRate / 1000000;
    emit frameTick(idx, offset);
}
