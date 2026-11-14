// ISyncClock.h — 统一时间基准抽象接口
//
// 所有"给出当前时间戳"的时间源都实现这个接口。
// 使用方式 (RecorderEngine / PlayerEngine):
//
//   auto *clk = SyncClockFactory::instance().create(config.clockType);
//   clk->start(config);
//   // 每帧循环:
//   if (clk->waitNextFrame()) {          // 阻塞到下一帧时间
//       qint64 ts = clk->currentTimestampUs();
//       // ... 录这一帧 ...
//   }
//   clk->stop();
//
// 非阻塞模式 (配合 QTimer / 自己的事件循环):
//   connect(clk, &ISyncClock::frameTick, ...);  // 每个同步帧 emit 一次
//   clk->start(config);
//
#ifndef ISYNC_CLOCK_H
#define ISYNC_CLOCK_H

#include "SyncTypes.h"
#include "../core/Common.h"
#include <QObject>
#include <chrono>

/**
 * @brief 统一时间基准抽象接口
 *
 * 设计原则:
 *   - start(config) → stop() 生命周期
 *   - currentTimestampUs() 永远返回"当前应该录/放的时间戳" (微秒)
 *   - 实现可以是主动 push (emit frameTick) 也可以是被动 pull (调用方每帧查询)
 *   - 所有实现必须保证: 在 start() → stop() 期间, currentTimestampUs() 单调递增
 *     (TimecodeFile/VideoFile 到末尾后 loopOnEnd=false 会返回 -1 表示结束)
 */
class ISyncClock : public QObject {
    Q_OBJECT
public:
    explicit ISyncClock(QObject *parent = nullptr) : QObject(parent) {}
    ~ISyncClock() override = default;

    /** 实现类型标识 */
    virtual SyncClockType clockType() const = 0;
    /** 人类可读名 */
    virtual QString clockName() const = 0;

    /**
     * @brief 启动时钟, 开始产出时间戳
     * @return 是否成功启动 (连接/打开文件/绑定端口都成功)
     */
    virtual bool start(const SyncClockConfig &config) = 0;

    /** 停止时钟, 释放资源 */
    virtual void stop() = 0;

    /** 是否在运行 */
    virtual bool isRunning() const = 0;

    /**
     * @brief 当前时间戳 (微秒). 非运行时返回 -1.
     *
     * 语义: 对于 InternalTimer, 返回"从 start 起经过的墙钟微秒"
     *       对于 VideoFile, 返回"视频 PTS 相对于 start 时 PTS 的微秒偏移"
     *       对于 MidiMTC / ArtNetTimeCode, 返回"第一个有效时间码到达后的 SMPTE 微秒偏移"
     *       单调递增保证 (除非 loop 回零, 那时 emit looped 信号)
     */
    virtual qint64 currentTimestampUs() const = 0;

    /**
     * @brief 当前帧序号 (相对于 start). 非运行时返回 -1.
     * 基于 frameRate 计算: frameIndex = currentTimestampUs * frameRate / 1_000_000
     */
    qint64 currentFrameIndex() const {
        qint64 ts = currentTimestampUs();
        if (ts < 0) return -1;
        int fps = frameRate();
        if (fps <= 0) fps = 30;
        return ts * fps / 1000000;
    }

    /** 配置的帧率 (某些实现内部会用自己的帧率) */
    virtual int frameRate() const = 0;

    /** 当前时间码 (SMPTE HH:MM:SS:FF), 非运行时返回零值 */
    TimeCode currentTimeCode() const {
        qint64 ts = currentTimestampUs();
        if (ts < 0) return TimeCode();
        int fps = frameRate();
        if (fps <= 0) fps = 30;
        qint64 totalFrames = ts * fps / 1000000LL;
        return TimeCode::fromTotalFrames(totalFrames, fps);
    }

signals:
    /**
     * @brief 每个同步帧到达时 emit 一次
     * @param frameIndex 当前帧序号 (基于配置帧率)
     * @param timestampUs 当前时间戳 (微秒)
     *
     * 注意: 对于 pull 模式的调用方, 完全可以忽略这个 signal 直接查 currentTimestampUs()
     */
    void frameTick(qint64 frameIndex, qint64 timestampUs);

    /**
     * @brief 时钟到达末尾
     * VideoFile 到 EOF / TimecodeFile 读完 / 接收端超时没收到新时间码
     * loopOnEnd=false 时 emit 后会自动 stop()
     */
    void clockFinished();

    /**
     * @brief 时钟循环回零 (loopOnEnd=true 时)
     * @param loopCount 已经循环的次数 (从 1 开始)
     */
    void clockLooped(int loopCount);

    /**
     * @brief 内部错误
     */
    void errorOccurred(const QString &error);

    /**
     * @brief 状态文本 (日志/状态栏用)
     */
    void statusChanged(const QString &status);
};

#endif // ISYNC_CLOCK_H
