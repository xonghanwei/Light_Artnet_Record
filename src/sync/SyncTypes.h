// SyncTypes.h — 同步抽象层共用类型
#ifndef SYNC_TYPES_H
#define SYNC_TYPES_H

#include "../core/Common.h"
#include <QString>
#include <QStringList>
#include <QUrl>

// 前置声明 ISyncClock
class ISyncClock;

// ============ 时间码源类型 ============
enum class SyncClockType : quint8 {
    InternalTimer = 0,   // 内部 QTimer + QElapsedTimer
    VideoFile,            // 视频文件 PTS (FFmpeg video stream)
    AudioFile,            // 音频 PTS / LTC 解码 (FFmpeg audio stream)
    ArtNetTimeCode,       // ArtNet opcode 0x0D TimeCode 包 (Art-Net 3 SMPTE)
    MidiMTC,              // MIDI Time Code (quarter-frame or full-frame)
    TimecodeFile,         // .tc / .lts / .srt 等时间码文件 (按时间戳吐 SMPTE)
    Unknown
};

inline QString syncClockTypeToString(SyncClockType t) {
    switch(t) {
        case SyncClockType::InternalTimer:  return "Internal Timer";
        case SyncClockType::VideoFile:      return "Video File";
        case SyncClockType::AudioFile:      return "Audio File / LTC";
        case SyncClockType::ArtNetTimeCode: return "ArtNet TimeCode";
        case SyncClockType::MidiMTC:        return "MIDI Time Code";
        case SyncClockType::TimecodeFile:   return "Timecode File (.tc/.lts/.srt)";
        case SyncClockType::Unknown:        return "Unknown";
    }
    return "Unknown";
}

inline SyncClockType syncClockTypeFromString(const QString &s) {
    QString l = s.toLower().trimmed();
    if (l == "internal" || l == "timer" || l == "internalclock")       return SyncClockType::InternalTimer;
    if (l == "video" || l == "videofile")                              return SyncClockType::VideoFile;
    if (l == "audio" || l == "audiofile" || l == "ltc")                return SyncClockType::AudioFile;
    if (l == "artnettc" || l == "artnettimecode" || l == "artnet_tc") return SyncClockType::ArtNetTimeCode;
    if (l == "mtc" || l == "midimt" || l == "midi")                    return SyncClockType::MidiMTC;
    if (l == "tcfile" || l == "timecodefile" || l == "tc" || l == "lts" || l == "srt")
                                                                       return SyncClockType::TimecodeFile;
    return SyncClockType::Unknown;
}

// ============ 时钟配置参数 ============
struct SyncClockConfig {
    SyncClockType clockType = SyncClockType::InternalTimer;

    // 通用
    int frameRate = 30;                    // 期望帧率 (有些 clock 用, 有些忽略)
    bool loopOnEnd = false;                // VideoFile/TimecodeFile 到达末尾后循环

    // VideoFile / AudioFile / TimecodeFile
    QString mediaFilePath;                 // 视频/音频/时间码文件路径

    // ArtNet TimeCode
    quint16 artNetPort = 6454;             // ArtNet TimeCode 监听端口 (通常 6454 也可能有专门口)
    QString artNetBindAddress = "0.0.0.0";

    // MIDI MTC
    QString midiInputDeviceName;           // MIDI 输入设备名 (空=自动选第一个可用)
    int     midiInputDeviceIndex = -1;     // 或直接指定 index (-1=自动)
    quint8  midiInputChannel = 0;          // MTC quarter-frame channel (0-15), 0 常用

    // TimecodeFile
    bool    timecodeFileHasRateHeader = true; // 文件首行是否含 FPS 头

    // 允许一定延迟等待同步源就绪 (毫秒). 0=不等待, <0=无限等待
    int     maxWaitForSyncMs = 5000;
};

#endif // SYNC_TYPES_H
