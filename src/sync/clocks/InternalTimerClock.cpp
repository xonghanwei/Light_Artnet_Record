// InternalTimerClock.cpp
#include "InternalTimerClock.h"
#include "../../core/Logger.h"

InternalTimerClock::InternalTimerClock(QObject *parent)
    : ISyncClock(parent) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(false);
    connect(m_timer, &QTimer::timeout, this, &InternalTimerClock::onTick);
}

InternalTimerClock::~InternalTimerClock() {
    stop();
}

bool InternalTimerClock::start(const SyncClockConfig &config) {
    m_config = config;
    m_frameRate = config.frameRate > 0 ? config.frameRate : 30;
    m_paused = false;
    m_pausedOffsetUs = 0;
    m_lastTickFrameIndex = -1;

    // 计算 tick 间隔
    double intervalMs = 1000.0 / m_frameRate;
    int intervalMsInt = qMax(1, int(intervalMs + 0.5));

    LOG_INFO(QString("[InternalTimerClock] start: fps=%1, interval=%2ms")
                 .arg(m_frameRate).arg(intervalMsInt));

    m_elapsed.start();
    m_timer->start(intervalMsInt);
    emit statusChanged(tr("内部时钟已启动 (%1 fps)").arg(m_frameRate));
    return true;
}

void InternalTimerClock::stop() {
    if (m_timer && m_timer->isActive()) {
        m_timer->stop();
        LOG_INFO("[InternalTimerClock] stop");
        emit statusChanged(tr("内部时钟已停止"));
    }
}

qint64 InternalTimerClock::currentTimestampUs() const {
    if (!isRunning()) return -1;
    if (m_paused) return m_pausedOffsetUs;
    // 运行中: pausedOffsetUs (之前已经过的时间) + 当前 elapsed (resume 后新过的时间)
    return m_pausedOffsetUs + m_elapsed.nsecsElapsed() / 1000;
}

void InternalTimerClock::pause() {
    if (!isRunning() || m_paused) return;
    // 累加: 之前 paused 的 + 当前已经过的
    m_pausedOffsetUs += m_elapsed.nsecsElapsed() / 1000;
    m_elapsed.restart();  // restart 后 nsecsElapsed 从 0 开始
    m_paused = true;
    LOG_INFO(QString("[InternalTimerClock] pause at cumulative %1 us").arg(m_pausedOffsetUs));
}

void InternalTimerClock::resume() {
    if (!isRunning() || !m_paused) return;
    m_elapsed.restart();  // restart 后新的经过时间会加到 pausedOffsetUs 上
    m_paused = false;
    LOG_INFO(QString("[InternalTimerClock] resume (baseline %1 us)").arg(m_pausedOffsetUs));
}

void InternalTimerClock::onTick() {
    if (m_paused) return;

    qint64 ts = currentTimestampUs();
    qint64 idx = ts * m_frameRate / 1000000;

    // 跳过重复帧 (高 fps 下 timer 抖动可能导致同一帧被 tick 多次)
    if (idx > m_lastTickFrameIndex) {
        m_lastTickFrameIndex = idx;
        emit frameTick(idx, ts);
    }
}
