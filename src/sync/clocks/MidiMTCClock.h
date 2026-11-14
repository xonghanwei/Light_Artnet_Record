// MidiMTCClock.h — MIDI Time Code 接收时钟
//
// Quarter-Frame: F1 <nibble> 共 8 个 quarter-frame 组成一帧
//   nibble 含义 (高 4bit 是内容, 低 4bit 是索引):
//     0=frame ls, 1=frame ms, 2=second ls, 3=second ms
//     4=minute ls, 5=minute ms, 6=hour ls, 7=hour ms + type/drop
// Full-Frame: F0 7F <device> 01 02 <hours> <minutes> <seconds> <frames> F7
//
// MIDI 访问:
//   a) config 指定了设备 (midiInputDeviceIndex>=0 或 midiInputDeviceName 非空)
//      → Windows 下用 WinMM midiInOpen/midiInStart 真实接收 (回调里排队到本对象线程)
//   b) 未指定设备
//      → 空实现 (stub): start() 返回 true, 由外部调用 onMtcQuarterFrame 喂入
#ifndef MIDI_MTC_CLOCK_H
#define MIDI_MTC_CLOCK_H

#include "../ISyncClock.h"
#include <QTimer>
#include <atomic>

class MidiMTCClock : public ISyncClock {
    Q_OBJECT
public:
    explicit MidiMTCClock(QObject *parent = nullptr);
    ~MidiMTCClock() override;

    SyncClockType clockType() const override { return SyncClockType::MidiMTC; }
    QString clockName() const override { return "MIDI Time Code"; }

    bool start(const SyncClockConfig &config) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }

    qint64 currentTimestampUs() const override;
    int frameRate() const override { return m_frameRate; }

public slots:
    /** 外部 (MIDI 端口回调) 收到一个 MTC quarter-frame 时调用 */
    void onMtcQuarterFrame(quint8 nibble);
    /** 收到 MTC full-frame sysex 时调用 */
    void onMtcFullFrame(quint8 hours, quint8 minutes, quint8 seconds, quint8 frames,
                        quint8 smpteType);

private slots:
    void onTimeout();

private:
    bool openMidiInput(const SyncClockConfig &config);
    void closeMidiInput();
    // Quarter-frame 累积器
    struct QFAccum {
        quint8 frame_ls = 0, frame_ms = 0;
        quint8 sec_ls   = 0, sec_ms   = 0;
        quint8 min_ls   = 0, min_ms   = 0;
        quint8 hour_ls  = 0, hour_ms  = 0;
        int    lastIndex = -1;  // 上一次收到的 quarter-frame index
        bool   waitingRestart = false;  // 收到 index 0 时重置
        int    fps = 30;
        bool   dropFrame = false;

        qint64 toUs() const {
            int h = (hour_ms << 4) | hour_ls;
            int m = (min_ms   << 4) | min_ls;
            int s = (sec_ms   << 4) | sec_ls;
            int f = (frame_ms << 4) | frame_ls;
            qint64 total = ((h*3600 + m*60 + s) * fps + f);
            return total * 1000000LL / fps;
        }
    };

    QFAccum m_qf;
    SyncClockConfig m_config;
    std::atomic<bool> m_running{false};
    std::atomic<qint64> m_lastTsUs{-1};
    int m_frameRate = 30;

    QTimer *m_timeoutTimer = nullptr;
    void *m_midiInHandle = nullptr;   // WinMM HMIDIIN (仅 Windows 有效)
};

#endif // MIDI_MTC_CLOCK_H
