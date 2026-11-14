// VideoFileClock.h - 视频文件时钟 (包装 FFmpegDecoder)
#ifndef VIDEO_FILE_CLOCK_H
#define VIDEO_FILE_CLOCK_H

#include "../ISyncClock.h"
#include "../../media/FFmpegDecoder.h"
#include <QTimer>
#include <atomic>

class VideoFileClock : public ISyncClock {
    Q_OBJECT
public:
    explicit VideoFileClock(QObject *parent = nullptr);
    ~VideoFileClock() override;

    SyncClockType clockType() const override { return SyncClockType::VideoFile; }
    QString clockName() const override { return "Video File"; }

    bool start(const SyncClockConfig &config) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }

    qint64 currentTimestampUs() const override;
    int frameRate() const override;

private slots:
    void onPoll();

private:
    FFmpegDecoder *m_decoder = nullptr;
    QTimer *m_pollTimer = nullptr;

    SyncClockConfig m_config;
    int m_frameRate = 30;
    std::atomic<bool> m_running{false};
    std::atomic<qint64> m_lastTsUs{-1};  // 最近一次解码出的视频 PTS (相对于 start)
    bool m_firstFrame = true;
    qint64 m_startPtsUs = 0;  // start 时视频 PTS
    int m_looped = 0;
};

#endif // VIDEO_FILE_CLOCK_H
