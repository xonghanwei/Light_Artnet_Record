// ArtNetTimeCodeClock.h — 监听 ArtNet opcode 0x0D TimeCode 包
//
// Art-Net 3 SMPTE: opcode = 0x0D00 (low-endian 00 0D)
// 包结构: 2B opcode + 2B version + 1B sequence + 1B type(0x00) + 1B hour + 1B min + 1B sec + 1B frame + 3B filler
// Hour 高位带 drop-frame 标志 (bit 7)
// SMPTE 类型 (type field 高 4bit): 0=24fps 1=25fps 2=29.97 3=30fps
#ifndef ARTNET_TC_CLOCK_H
#define ARTNET_TC_CLOCK_H

#include "../ISyncClock.h"
#include <QUdpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>

class ArtNetTimeCodeClock : public ISyncClock {
    Q_OBJECT
public:
    explicit ArtNetTimeCodeClock(QObject *parent = nullptr);
    ~ArtNetTimeCodeClock() override;

    SyncClockType clockType() const override { return SyncClockType::ArtNetTimeCode; }
    QString clockName() const override { return "ArtNet TimeCode"; }

    bool start(const SyncClockConfig &config) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }

    qint64 currentTimestampUs() const override;
    int frameRate() const override { return m_frameRate; }

    // ArtNet opcode 常量
    static constexpr quint16 OPCODE_ARTTIME = 0x0D00;  // 小端序: 00 0D
    static constexpr quint16 OPCODE_IDX_LO  = 0;       // opcode 在包偏移 0-1

private slots:
    void onReadyRead();
    void onTimeout();

private:
    QUdpSocket *m_socket = nullptr;
    QTimer *m_timeoutTimer = nullptr;  // 没收到新 TC 的超时
    SyncClockConfig m_config;

    std::atomic<bool> m_running{false};
    std::atomic<qint64> m_lastTsUs{-1};  // SMPTE 微秒偏移
    std::atomic<bool> m_hasReceived{false};
    int m_frameRate = 30;

    quint32 m_expectedPacketsPerSec = 0;
    QElapsedTimer m_lastReceive;

    // ArtNet TimeCode 包解析
    struct ParsedTC {
        int hours; int minutes; int seconds; int frames;
        int fps; bool dropFrame;
        qint64 toUs() const {
            qint64 totalFrames = (hours*3600 + minutes*60 + seconds)*fps + frames;
            return totalFrames * 1000000LL / fps;
        }
    };
    bool parseArtNetTC(const QByteArray &data, ParsedTC &out);
};

#endif // ARTNET_TC_CLOCK_H
