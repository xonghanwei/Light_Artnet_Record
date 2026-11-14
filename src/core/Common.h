// Common.h - 通用类型定义
#ifndef COMMON_H
#define COMMON_H

#include <QtGlobal>
#include <cstdint>
#include <cstring>
#include <QByteArray>
#include <QString>

// ArtNet 常量
namespace ArtNet {
    const quint16 DEFAULT_PORT = 6454;
    const int DMX_CHANNEL_COUNT = 512;
    const int MAX_UNIVERSES = 256;
    
    // ArtNet OpCodes
    const quint16 OP_POLL     = 0x2000;
    const quint16 OP_POLL_REPLY = 0x2100;
    const quint16 OP_DMX     = 0x5000;
    const quint16 OP_ADDRESS = 0x5100;
    const quint16 OP_INPUT   = 0x5200;
    const quint16 OP_RDM     = 0x6000;
    const quint16 OP_RDMSUB  = 0x6100;
    const quint16 OP_TIMECODE = 0x9700;
    const quint16 OP_TIMESYNC = 0x9800;
    const quint16 OP_TRIGGER  = 0x9900;
    const quint16 OP_DIAG     = 0x2300;
    const quint16 OP_DIAG_REPLY = 0x2400;
    const quint16 OP_FIRMWARE = 0x1F00;
    const quint16 OP_FIRMWARE_REPLY = 0x2000;
    
    // ArtNet 魔术字
    const char ID_STRING[8] = {'A', 'r', 't', '-', 'N', 'e', 't', '\0'};
    
    // Universe 编码: Net(7bit) | SubNet(4bit) | Universe(4bit)
    inline quint16 encodeUniverse(quint8 net, quint8 subNet, quint8 universe) {
        return (quint16(net & 0x7F) << 8) | 
               (quint16(subNet & 0x0F) << 4) | 
               (quint16(universe & 0x0F));
    }
    
    inline quint8 universeNet(quint16 universe) {
        return quint8((universe >> 8) & 0x7F);
    }
    
    inline quint8 universeSubNet(quint16 universe) {
        return quint8((universe >> 4) & 0x0F);
    }
    
    inline quint8 universeNumber(quint16 universe) {
        return quint8(universe & 0x0F);
    }
}

// 帧率定义
enum class FrameRate : int {
    FPS_24 = 24,
    FPS_25 = 25,
    FPS_2997 = 2997,  // 表示 29.97
    FPS_30 = 30,
    FPS_60 = 60
};

inline int frameRateValue(FrameRate fr) {
    switch(fr) {
        case FrameRate::FPS_24: return 24;
        case FrameRate::FPS_25: return 25;
        case FrameRate::FPS_2997: return 29; // 整数部分用于计算
        case FrameRate::FPS_30: return 30;
        case FrameRate::FPS_60: return 60;
    }
    return 30;
}

inline double frameRateDouble(FrameRate fr) {
    switch(fr) {
        case FrameRate::FPS_24: return 24.0;
        case FrameRate::FPS_25: return 25.0;
        case FrameRate::FPS_2997: return 29.97;
        case FrameRate::FPS_30: return 30.0;
        case FrameRate::FPS_60: return 60.0;
    }
    return 30.0;
}

inline qint64 frameIntervalUs(FrameRate fr) {
    // 返回每帧的微秒数
    double d = frameRateDouble(fr);
    return qint64(1000000.0 / d + 0.5);
}

// 时间码类型
enum class TimeCodeType : quint8 {
    None = 0,
    MTC = 1,        // MIDI Time Code
    Video = 2,      // 视频帧时间戳
    Audio = 3       // 音频LTC时间码
};

// SMPTE时间码格式
enum class TimeCodeFormat : quint8 {
    TC_24FPS = 0,
    TC_25FPS = 1,
    TC_2997FPS = 2,
    TC_30FPS = 3
};

// ArtNet DMX 帧 (接收时解析的数据结构)
struct ArtNetDmxFrame {
    quint8  sequence = 0;       // 序列号 0-255
    quint8  physical = 0;       // 物理端口号
    quint16 universe = 0;       // Universe 编码
    quint16 length = 0;         // DMX 数据长度 (最大512)
    quint8  dmxData[512] = {0}; // DMX 通道数据
    qint64  receiveTimeUs = 0;  // 接收时间戳(微秒)
};

// 时间码结构
struct TimeCode {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;
    int frameRate = 30;         // 帧率
    bool dropFrame = false;     // 是否丢帧模式(29.97)
    qint64 timestampUs = 0;     // 精确时间戳(微秒)
    
    // 转换为总帧数
    qint64 toTotalFrames() const {
        qint64 fps = frameRate;
        if (fps == 0) fps = 30;
        
        if (dropFrame && fps == 29) {
            // 29.97 丢帧模式
            // 每分钟跳过 2 帧, 每 10 分钟跳过 18 帧
            qint64 totalSeconds = hours * 3600LL + minutes * 60LL + seconds;
            qint64 total = totalSeconds * fps + frames;
            
            constexpr qint64 DROP_PER_MIN = 2;       // 每分钟丢 2 帧
            constexpr qint64 DROP_PER_10MIN = 18;    // 每 10 分钟丢 18 帧 (2×9)
            qint64 totalMinutes = hours * 60LL + minutes;
            qint64 tens = totalMinutes / 10;
            qint64 singles = totalMinutes % 10;
            qint64 totalDrops = tens * DROP_PER_10MIN + singles * DROP_PER_MIN;
            
            return total - totalDrops;
        }
        else {
            return (hours * 3600LL + minutes * 60LL + seconds) * fps + frames;
        }
    }
    
    // 从总帧数转换
    static TimeCode fromTotalFrames(qint64 totalFrames, int fps, bool dropFrame = false) {
        TimeCode tc;
        tc.frameRate = fps;
        tc.dropFrame = dropFrame;
        
        qint64 framesPerMin = fps * 60;
        qint64 framesPer10Min = framesPerMin * 10;
        qint64 framesPerHour = framesPerMin * 60;
        
        if (dropFrame && fps == 29) {
            qint64 total = totalFrames;
            
            // 计算丢帧补偿
            qint64 hoursCount = total / (framesPerHour - 108); // 每小时丢 108 帧
            qint64 remainder = total % (framesPerHour - 108);
            
            qint64 tensCount = remainder / (framesPer10Min - 18);
            remainder = remainder % (framesPer10Min - 18);
            
            qint64 minutesCount = remainder / (framesPerMin - 2);
            remainder = remainder % (framesPerMin - 2);
            
            tc.hours = int(hoursCount);
            tc.minutes = int(tensCount * 10 + minutesCount);
            tc.seconds = int(remainder / fps);
            tc.frames = int(remainder % fps);
        }
        else {
            tc.hours = int(totalFrames / framesPerHour);
            totalFrames %= framesPerHour;
            tc.minutes = int(totalFrames / framesPerMin);
            totalFrames %= framesPerMin;
            tc.seconds = int(totalFrames / fps);
            tc.frames = int(totalFrames % fps);
        }
        
        return tc;
    }
    
    QString toString() const {
        return QString("%1:%2:%3:%4")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'))
            .arg(frames, 2, 10, QChar('0'));
    }
};

// 录制状态
enum class RecordState {
    Idle,           // 空闲
    WaitingSync,    // 等待同步信号
    Recording,      // 录制中
    Paused,         // 暂停
    Stopped         // 已停止
};

inline QString recordStateToString(RecordState state) {
    switch(state) {
        case RecordState::Idle: return "Idle";
        case RecordState::WaitingSync: return "Waiting Sync";
        case RecordState::Recording: return "Recording";
        case RecordState::Paused: return "Paused";
        case RecordState::Stopped: return "Stopped";
    }
    return "Unknown";
}

#include <QMetaType>
Q_DECLARE_METATYPE(RecordState)
Q_DECLARE_METATYPE(ArtNetDmxFrame)

#endif // COMMON_H
