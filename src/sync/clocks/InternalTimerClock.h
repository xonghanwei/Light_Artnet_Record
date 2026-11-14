// InternalTimerClock.h — 内部 QElapsedTimer + QTimer 时钟
//
// 最简单的时钟: start() 时记录墙钟, 之后每次 currentTimestampUs() 返回
// 墙钟偏移. QTimer 按帧率定时 emit frameTick, 让上层不用自己算帧周期.
#ifndef INTERNAL_TIMER_CLOCK_H
#define INTERNAL_TIMER_CLOCK_H

#include "../ISyncClock.h"
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>

class InternalTimerClock : public ISyncClock {
    Q_OBJECT
public:
    explicit InternalTimerClock(QObject *parent = nullptr);
    ~InternalTimerClock() override;

    SyncClockType clockType() const override { return SyncClockType::InternalTimer; }
    QString clockName() const override { return "Internal Timer"; }

    bool start(const SyncClockConfig &config) override;
    void stop() override;
    bool isRunning() const override { return m_timer && m_timer->isActive(); }

    qint64 currentTimestampUs() const override;
    int frameRate() const override { return m_frameRate; }

    // 额外: 暂停/恢复 (RecorderEngine 暂停时不停止时钟, 而是冻结偏移)
    void pause();
    void resume();
    bool isPaused() const { return m_paused; }

private slots:
    void onTick();

private:
    QTimer *m_timer = nullptr;
    QElapsedTimer m_elapsed;

    SyncClockConfig m_config;
    int m_frameRate = 30;

    // 暂停逻辑
    bool m_paused = false;
    qint64 m_pausedOffsetUs = 0;      // 暂停时已经过的时间
    qint64 m_lastTickFrameIndex = -1; // 上一次 tick 发送的帧 (避免重复 emit)
};

#endif // INTERNAL_TIMER_CLOCK_H
