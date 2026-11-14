// RecorderEngine.h - 录制引擎核心
#ifndef RECORDER_ENGINE_H
#define RECORDER_ENGINE_H

#include "../core/Common.h"
#include "../fileformat/ArtRecWriter.h"
#include "../network/ArtNetReceiver.h"
#include "../media/FFmpegDecoder.h"
#include "../output/OutputManager.h"
#include "../sync/ISyncClock.h"
#include "../sync/SyncClockFactory.h"
#include <QObject>
#include <QMap>
#include <QList>
#include <QTimer>
#include <QThread>
#include <QMutex>
#include <QElapsedTimer>

class RecorderEngine : public QObject {
    Q_OBJECT
public:
    enum class SyncSource {
        InternalClock,   // 内部时钟 (→ SyncClockType::InternalTimer)
        ArtNetTimeCode,  // ArtNet opcode 0x0D (→ SyncClockType::ArtNetTimeCode)
        MidiMTC,         // MIDI Time Code (→ SyncClockType::MidiMTC)
        TimecodeFile,    // .tc/.lts 文件 (→ SyncClockType::TimecodeFile)
        VideoFile,       // 视频文件 PTS (FFmpeg) (→ SyncClockType::VideoFile, 需 ARTNET_WITH_FFMPEG)
        AudioFile        // 音频文件 LTC (→ SyncClockType::AudioFile, 需 ARTNET_WITH_FFMPEG)
    };
    
    static SyncClockType syncSourceToClockType(SyncSource src) {
        switch (src) {
            case SyncSource::InternalClock:  return SyncClockType::InternalTimer;
            case SyncSource::ArtNetTimeCode: return SyncClockType::ArtNetTimeCode;
            case SyncSource::MidiMTC:        return SyncClockType::MidiMTC;
            case SyncSource::TimecodeFile:   return SyncClockType::TimecodeFile;
            case SyncSource::VideoFile:      return SyncClockType::VideoFile;
            case SyncSource::AudioFile:      return SyncClockType::AudioFile;
        }
        return SyncClockType::InternalTimer;
    }
    
    static QString syncSourceToString(SyncSource src) {
        switch (src) {
            case SyncSource::InternalClock:  return "Internal Clock";
            case SyncSource::ArtNetTimeCode: return "ArtNet TimeCode";
            case SyncSource::MidiMTC:        return "MIDI Time Code";
            case SyncSource::TimecodeFile:   return "Timecode File";
            case SyncSource::VideoFile:      return "Video File";
            case SyncSource::AudioFile:      return "Audio File";
        }
        return "Unknown";
    }
    
    struct Config {
        QString outputFilePath;        // 输出文件路径
        SyncSource syncSource = SyncSource::InternalClock;
        QString mediaFilePath;         // 媒体文件路径 (video/audio模式需要)
        QList<quint16> universes;      // 要录制的 Universe 列表 (encoded)
        int frameRate = 30;            // 目标帧率
        qint64 maxDurationUs = 0;      // 最大录制时长(微秒), 0=不限
        quint16 artNetPort = ArtNet::DEFAULT_PORT;
        int previewIntervalMs = 1000; // 状态预览间隔
        bool verbose = false;
        
        // === 输出/转发配置 ===
        QList<OutputTargetConfig> outputTargets; // 同时转发到的目标 (可空)
        bool enablePassthrough = false;  // 录制同时开启转发
    };
    
    explicit RecorderEngine(QObject *parent = nullptr);
    ~RecorderEngine();
    
    /**
     * @brief 开始录制
     * @param config 录制配置
     * @return 成功返回true
     */
    bool start(const Config &config);
    
    /**
     * @brief 停止录制
     */
    void stop();
    
    /**
     * @brief 暂停
     */
    void pause();
    
    /**
     * @brief 恢复
     */
    void resume();
    
    /**
     * @brief 当前状态
     */
    RecordState state() const { return m_state; }
    
    /**
     * @brief 已录制帧数
     */
    qint64 recordedFrames() const { return m_recordedFrames; }
    
    /**
     * @brief 当前录制时长 (微秒)
     */
    qint64 currentDurationUs() const;
    
    /**
     * @brief 当前时间码
     */
    TimeCode currentTimeCode() const;
    
    /**
     * @brief 帧率
     */
    int frameRate() const { return m_config.frameRate; }
    
public slots:
    /**
     * @brief 注入 MTC quarter-frame (仅 MidiMTC 模式有效, 其他模式忽略)
     * @param nibble MTC quarter-frame nibble (0-7, 高 4bit 是内容, 低 4bit 是索引)
     */
    void injectMidiMtcQuarterFrame(quint8 nibble);
    
signals:
    void stateChanged(RecordState newState);
    void frameRecorded(qint64 frameIndex, qint64 timestampUs);
    void recordingFinished(qint64 totalFrames, qint64 totalDurationUs);
    void errorOccurred(const QString &error);
    void statusUpdate(const QString &status);
    
private slots:
    void onDmxFrameReceived(const ArtNetDmxFrame &frame);
    void onPreviewTick();
    void onSyncFrameTick(qint64 frameIndex, qint64 timestampUs);
    void onSyncClockFinished();
    
private:
    void setState(RecordState state);
    void recordingLoop();
    
    // 资源清理 (停止时释放所有子组件)
    void cleanup();
    
    // 获取当前时间戳 (微秒) — 委托给 ISyncClock
    qint64 getCurrentTimestampUs();
    
    // 把 RecorderEngine::Config 转换成 SyncClockConfig
    SyncClockConfig buildSyncClockConfig() const;
    
    // 目标帧序号 → 目标时间戳 (保留给需要的场景)
    qint64 frameIndexToUs(qint64 frameIndex) const;
    
    // 检测 DMX 数据是否有变化
    bool dmxDataChanged(quint16 universe) const;
    
    // 检查录制停止条件
    bool shouldStop();
    
    // 构造 UniverseEntry 列表
    QList<UniverseEntry> buildUniverseEntries() const;
    
    // 把当前帧写入 ArtRecWriter (公共逻辑, frameTick 和其他路径共用)
    void writeCurrentFrame(qint64 frameIndex, qint64 timestampUs);
    
    Config m_config;
    RecordState m_state = RecordState::Idle;
    
    // Sync 层 (统一时间基准, 替代 QDateTime::currentMSecsSinceEpoch + FFmpegDecoder PTS)
    ISyncClock *m_syncClock = nullptr;
    
    // DMX 数据缓存 (key=encoded universe)
    QMap<quint16, QByteArray> m_universeCache;
    // 上一帧的数据快照 (用于变化检测)
    QMap<quint16, QByteArray> m_prevUniverseSnapshot;
    QMutex m_cacheMutex;
    
    // 组件
    ArtRecWriter *m_writer = nullptr;
    ArtNetReceiver *m_receiver = nullptr;
    FFmpegDecoder *m_decoder = nullptr;  // 仅 VideoFile/AudioFile 回退时创建
    OutputManager *m_outputManager = nullptr;  // 输出转发管理 (可选)
    QTimer *m_previewTimer = nullptr;
    
    // 统计
    qint64 m_recordedFrames = 0;
    qint64 m_lastFrameIndex = -1;
    TimeCode m_lastTimeCode;
};

#endif // RECORDER_ENGINE_H
