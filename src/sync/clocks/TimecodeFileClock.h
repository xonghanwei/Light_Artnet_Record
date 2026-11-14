// TimecodeFileClock.h — 从 .tc / .lts / .srt 文件读取时间码, 按配置帧率吐 SMPTE 时间戳
//
// 格式约定 (简单子集):
//   - 每行一条时间码: HH:MM:SS:FF 或 HH:MM:SS,frame (srt 风格)
//   - 首行可含 "#FPS=30" 或 "#FPS=25" 注释头
//   - 也可每行带帧序号前缀: "00:00:01:00  some_comment"
//
// 行为: 每次 QTimer tick 根据当前配置帧率 advance 一帧, emit frameTick
//       timestampUs = (currentLine.lineFrames - 起始帧) / fps * 1e6
//       读完文件 → clockFinished, 若 loopOnEnd=true 则回到首行
#ifndef TIMECODE_FILE_CLOCK_H
#define TIMECODE_FILE_CLOCK_H

#include "../ISyncClock.h"
#include <QFile>
#include <QTimer>
#include <atomic>
#include <vector>

class TimecodeFileClock : public ISyncClock {
    Q_OBJECT
public:
    explicit TimecodeFileClock(QObject *parent = nullptr);
    ~TimecodeFileClock() override;

    SyncClockType clockType() const override { return SyncClockType::TimecodeFile; }
    QString clockName() const override { return "Timecode File"; }

    bool start(const SyncClockConfig &config) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }

    qint64 currentTimestampUs() const override;
    int frameRate() const override { return m_frameRate; }

private slots:
    void onTick();

private:
    struct Line {
        qint64 frameIndex;  // 绝对帧序号 (基于 fileFps)
        qint64 timestampUs; // 对应的微秒
    };

    // 解析一行时间码 → 绝对帧数, fps 由调用方提供
    static bool parseLine(const QString &line, int fileFps, Line &out);

    std::vector<Line> m_lines;
    qint64 m_currentFrame = -1;

    SyncClockConfig m_config;
    QTimer *m_timer = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<qint64> m_lastTsUs{-1};
    int m_frameRate = 30;  // clock 自己的帧率 (吐帧的节奏)
    int m_fileFps   = 30;  // 文件解析用的帧率
};

#endif // TIMECODE_FILE_CLOCK_H
