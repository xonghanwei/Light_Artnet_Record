// AudioFileClock.h — 音频时钟 (占位: 包装 FFmpegDecoder audio stream)
// 真正的 LTC 解码需要 libltc 或自实现. 先提供 PTS 模式, 行为和 VideoFileClock 类似
#ifndef AUDIO_FILE_CLOCK_H
#define AUDIO_FILE_CLOCK_H

#include "../ISyncClock.h"
#include "../../media/FFmpegDecoder.h"
#include <QTimer>
#include <atomic>

class AudioFileClock : public ISyncClock {
    Q_OBJECT
public:
    explicit AudioFileClock(QObject *parent = nullptr);
    ~AudioFileClock() override;

    SyncClockType clockType() const override { return SyncClockType::AudioFile; }
    QString clockName() const override { return "Audio File"; }

    bool start(const SyncClockConfig &config) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }

    qint64 currentTimestampUs() const override;
    int frameRate() const override { return m_frameRate; }

private slots:
    void onPoll();

private:
    FFmpegDecoder *m_decoder = nullptr;
    QTimer *m_pollTimer = nullptr;
    SyncClockConfig m_config;
    int m_frameRate = 30;
    std::atomic<bool> m_running{false};
    std::atomic<qint64> m_lastTsUs{-1};
    bool m_firstFrame = true;
    qint64 m_startPtsUs = 0;
};

#endif // AUDIO_FILE_CLOCK_H
