// ArtNetSdk.cpp — 第三方 SDK 实现 (无 Qt Widgets 依赖)
#include "ArtNetSdk.h"

#include "core/Common.h"
#include "fileformat/ArtRecReader.h"
#include "fileformat/ArtRecWriter.h"
#include "output/OutputManager.h"
#include "sync/ISyncClock.h"
#include "sync/SyncClockFactory.h"
#include "sync/SyncTypes.h"

#include <QString>
#include <QByteArray>
#include <QMap>
#include <QList>
#include <QFile>
#include <QDir>
#include <QUdpSocket>
#include <QHostAddress>
#include <QThread>
#include <cmath>
#include <cstring>

struct ArtNetSdk {
    OutputManager *out = nullptr;

    // 回放
    ArtRecReader reader;
    bool playbackOpen = false;
    qint64 currentFrame = -1;
    quint64 framesSent = 0;
    qint64 tcOffsetUs = 0;          // 外部时间码基准偏移
    ISyncClock *listener = nullptr;  // 内建时间码监听

    // 录制
    ArtRecWriter writer;
    bool recording = false;
    bool hasDmxTarget = false;      // 是否配置过 DMX512 目标
    qint64 recordIndex = 0;
    qint64 firstTsUs = 0;
    qint64 lastTsUs = 0;
    int recordFps = 30;
    QList<UniverseEntry> recordUniverses;

    QString error;
};

static QString g_selftestText;

static void setErr(ArtNetSdk *s, const QString &e) {
    if (s) s->error = e;
}

static QString qs(const char *utf8) {
    return utf8 ? QString::fromUtf8(utf8) : QString();
}

// 有效帧率: 0=文件帧率, 2997=29.97, 5994=59.94
static double effectiveFps(ArtNetSdk *sdk, int fps) {
    double f = double(fps);
    if (f == 2997.0) f = 29.97;
    else if (f == 5994.0) f = 59.94;
    if (f <= 0) {
        const int hf = sdk ? sdk->reader.header().frameRate : 30;
        f = (hf == 2997) ? 29.97 : (hf == 5994 ? 59.94 : double(hf));
    }
    if (f <= 0) f = 30.0;
    return f;
}

// 按绝对时间码定位并输出对应帧
// 返回: 0 已发送; 1 忽略 (偏移前/重复帧); -1 失败
static int sendFrameAt(ArtNetSdk *sdk, qint64 timestampUs, int fps) {
    if (!sdk || !sdk->playbackOpen) {
        setErr(sdk, QStringLiteral("尚未打开录制文件"));
        return -1;
    }
    const qint64 rel = timestampUs - sdk->tcOffsetUs;
    if (rel < 0) return 1;   // 时间码还没进入文件范围

    const double f = effectiveFps(sdk, fps);
    qint64 frame = qint64(std::floor(double(rel) * f / 1000000.0 + 0.5));
    const qint64 total = qint64(sdk->reader.header().totalFrames);
    if (total > 0) frame = qBound<qint64>(0, frame, total - 1);
    if (frame == sdk->currentFrame) return 1;

    if (!sdk->reader.seekToFrame(frame)) {
        setErr(sdk, QStringLiteral("定位帧 %1 失败").arg(frame));
        return -1;
    }
    qint64 idx = -1, tsUs = 0;
    QMap<int, QByteArray> universeData;
    if (!sdk->reader.readNextFrame(idx, tsUs, universeData)) {
        setErr(sdk, QStringLiteral("读取帧 %1 失败").arg(frame));
        return -1;
    }

    const QList<UniverseEntry> &unis = sdk->reader.universes();
    for (auto it = universeData.constBegin(); it != universeData.constEnd(); ++it) {
        quint16 uni = 0;
        if (it.key() >= 0 && it.key() < unis.size())
            uni = unis.at(it.key()).encoded();
        sdk->out->sendDmxFrame(uni, it.value());
    }
    sdk->currentFrame = frame;
    ++sdk->framesSent;
    return 0;
}

extern "C" {

ANS_API const char* ans_version(void) { return "1.0.0"; }
ANS_API const char* ans_qt_version(void) { return qVersion(); }

ANS_API ArtNetSdk* ans_create(void) {
    auto *s = new ArtNetSdk;
    s->out = new OutputManager;
    return s;
}

ANS_API void ans_destroy(ArtNetSdk* sdk) {
    if (!sdk) return;
    ans_stop_timecode_listener(sdk);
    if (sdk->recording) {
        sdk->writer.finalize(sdk->recordIndex,
                             sdk->lastTsUs > sdk->firstTsUs
                                 ? sdk->lastTsUs - sdk->firstTsUs : 0);
    }
    if (sdk->out) {
        sdk->out->stop();
        delete sdk->out;
    }
    delete sdk;
}

ANS_API const char* ans_last_error(ArtNetSdk* sdk) {
    static QByteArray buf;
    buf = sdk ? sdk->error.toUtf8() : QByteArray("invalid sdk handle");
    return buf.constData();
}

/* ================= 输出 ================= */

ANS_API int ans_add_artnet_target(ArtNetSdk* sdk, const char* ip, int port) {
    if (!sdk || !sdk->out) return -1;
    if (!ip || !*ip) { setErr(sdk, QStringLiteral("Art-Net 目标 IP 为空")); return -1; }
    const int id = sdk->out->addArtNetTarget(qs(ip), quint16(port > 0 ? port : 6454));
    if (id <= 0) setErr(sdk, QStringLiteral("添加 Art-Net 目标失败"));
    return id;
}

ANS_API int ans_add_dmx512_target(ArtNetSdk* sdk, const char* portName, int baudRate) {
    if (!sdk || !sdk->out) return -1;
    if (!portName || !*portName) { setErr(sdk, QStringLiteral("DMX 串口名为空")); return -1; }
    const int id = sdk->out->addDmx512Target(qs(portName), baudRate > 0 ? baudRate : 250000);
    if (id <= 0) setErr(sdk, QStringLiteral("添加 DMX512 目标失败: %1").arg(qs(portName)));
    else sdk->hasDmxTarget = true;
    return id;
}

ANS_API void ans_clear_targets(ArtNetSdk* sdk) {
    if (!sdk || !sdk->out) return;
    sdk->out->stop();
    sdk->out->clearAll();
    sdk->hasDmxTarget = false;
}

ANS_API int ans_start_outputs(ArtNetSdk* sdk) {
    if (!sdk || !sdk->out) return -1;
    if (!sdk->out->start()) {
        setErr(sdk, QStringLiteral("输出启动失败"));
        return -1;
    }
    // Dmx512Sender 打开失败不会让 OutputManager::start() 返回 false, 这里补一次校验
    if (sdk->hasDmxTarget && sdk->out->dmx512Sender()
        && !sdk->out->dmx512Sender()->isOpen()) {
        setErr(sdk, QStringLiteral("DMX512 串口打开失败 (端口不存在或被占用?)"));
        return -1;
    }
    return 0;
}

ANS_API void ans_stop_outputs(ArtNetSdk* sdk) {
    if (sdk && sdk->out) sdk->out->stop();
}

ANS_API int ans_send_dmx(ArtNetSdk* sdk, int universe, const unsigned char* data, int length) {
    if (!sdk || !sdk->out || !data) return -1;
    const int n = qBound(0, length, 512);
    sdk->out->sendDmxFrame(quint16(universe & 0x7FFF),
                           QByteArray(reinterpret_cast<const char*>(data), n));
    return 0;
}

ANS_API unsigned long long ans_packets_sent(ArtNetSdk* sdk) {
    return (sdk && sdk->out) ? sdk->out->totalPacketsSent() : 0ULL;
}

/* ================= 回放 ================= */

ANS_API int ans_open_recording(ArtNetSdk* sdk, const char* path) {
    if (!sdk) return -1;
    if (!path || !*path) { setErr(sdk, QStringLiteral("录制文件路径为空")); return -1; }
    ans_stop_timecode_listener(sdk);
    if (!sdk->reader.open(qs(path))) {
        setErr(sdk, QStringLiteral("打开录制文件失败: %1").arg(qs(path)));
        sdk->playbackOpen = false;
        return -1;
    }
    sdk->playbackOpen = true;
    sdk->currentFrame = -1;
    sdk->framesSent = 0;
    sdk->error.clear();
    return 0;
}

ANS_API int ans_set_timecode(ArtNetSdk* sdk, long long timestampUs, int fps) {
    if (!sdk || !sdk->playbackOpen) {
        setErr(sdk, QStringLiteral("尚未打开录制文件"));
        return -1;
    }
    if (timestampUs < 0) { setErr(sdk, QStringLiteral("时间码为负")); return -1; }
    return sendFrameAt(sdk, timestampUs, fps);
}

ANS_API void ans_set_timecode_offset(ArtNetSdk* sdk, long long offsetUs) {
    if (sdk) sdk->tcOffsetUs = offsetUs;
}

ANS_API int ans_set_timecode_string(ArtNetSdk* sdk, const char* smpte, int fps) {
    if (!sdk || !sdk->playbackOpen) {
        setErr(sdk, QStringLiteral("尚未打开录制文件"));
        return -1;
    }
    if (!smpte || !*smpte) { setErr(sdk, QStringLiteral("时间码字符串为空")); return -1; }

    const QString text = QString::fromLatin1(smpte).trimmed();
    const bool semi = text.contains(';');
    if (!text.contains(':') && !semi) {
        setErr(sdk, QStringLiteral("SMPTE 格式应为 HH:MM:SS:FF"));
        return -1;
    }
    // 丢帧用最后一段的分号标记: "01:00:00;00" → 统一按冒号切分
    QString normalized = text;
    normalized.replace(';', ':');
    const QStringList parts = normalized.split(':');
    if (parts.size() != 4) { setErr(sdk, QStringLiteral("SMPTE 格式应为 HH:MM:SS:FF")); return -1; }

    int v[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        v[i] = parts.at(i).toInt(&ok);
        if (!ok) { setErr(sdk, QStringLiteral("SMPTE 字段不是数字: %1").arg(parts.at(i))); return -1; }
    }
    const int hh = v[0], mm = v[1], ss = v[2], ff = v[3];
    const double f = effectiveFps(sdk, fps);
    const int nominal = int(std::lround(f));
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59
        || ff < 0 || ff >= nominal) {
        setErr(sdk, QStringLiteral("SMPTE 数值越界: %1").arg(text));
        return -1;
    }

    const bool dropFrame = semi && (qAbs(f - 29.97) < 0.01 || qAbs(f - 59.94) < 0.01);
    qint64 tsUs = 0;
    if (dropFrame) {
        // 29.97/59.94 丢帧计数: 每分钟丢 2/4 帧, 整十分钟不丢
        const int dropPerMin = (nominal >= 60) ? 4 : 2;
        const int totalMinutes = hh * 60 + mm;
        const qint64 frameNumber = qint64(nominal) * (hh * 3600 + mm * 60 + ss) + ff
                                   - dropPerMin * (totalMinutes - totalMinutes / 10);
        tsUs = qint64(std::llround(double(frameNumber) * 1001000000.0 / (nominal * 1000.0)));
    } else {
        const qint64 totalFrames = qint64(hh * 3600 + mm * 60 + ss) * nominal + ff;
        tsUs = qint64(std::llround(double(totalFrames) * 1000000.0 / f));
    }
    return sendFrameAt(sdk, tsUs, fps);
}

ANS_API int ans_set_timecode_frame(ArtNetSdk* sdk, long long frameIndex, int fps) {
    if (!sdk || !sdk->playbackOpen) {
        setErr(sdk, QStringLiteral("尚未打开录制文件"));
        return -1;
    }
    if (frameIndex < 0) { setErr(sdk, QStringLiteral("帧号为负")); return -1; }
    const double f = effectiveFps(sdk, fps);
    const qint64 tsUs = qint64(std::llround(double(frameIndex) * 1000000.0 / f)) + sdk->tcOffsetUs;
    return sendFrameAt(sdk, tsUs, fps);
}

ANS_API int ans_playback_state(ArtNetSdk* sdk) {
    if (!sdk || !sdk->playbackOpen) return 0;
    return (sdk->framesSent > 0 || sdk->listener) ? 2 : 1;
}

ANS_API int ans_playback_info(ArtNetSdk* sdk, AnsPlaybackInfo* info) {
    if (!sdk || !sdk->playbackOpen || !info) {
        setErr(sdk, QStringLiteral("回放未打开或参数为空"));
        return -1;
    }
    const ArtRecHeader &h = sdk->reader.header();
    info->frameRateX100 = (h.frameRate == 2997) ? 2997
                        : (h.frameRate == 5994) ? 5994 : int(h.frameRate) * 100;
    info->totalFrames = int(h.totalFrames);
    info->totalDurationUs = qint64(h.totalDurationUs);
    info->universeCount = int(h.universeCount);
    return 0;
}

ANS_API int ans_start_timecode_listener(ArtNetSdk* sdk, int source, int param, int fps) {
    if (!sdk || !sdk->playbackOpen) {
        setErr(sdk, QStringLiteral("尚未打开录制文件"));
        return -1;
    }
    if (!sdk->out || !sdk->out->isRunning()) {
        setErr(sdk, QStringLiteral("输出未启动, 请先 ans_start_outputs"));
        return -1;
    }

    ans_stop_timecode_listener(sdk);

    SyncClockConfig cfg;
    cfg.frameRate = fps > 0 ? fps : int(std::lround(effectiveFps(sdk, 0)));
    if (source == ANS_TC_SOURCE_ARTNET) {
        cfg.clockType = SyncClockType::ArtNetTimeCode;
        cfg.artNetPort = quint16(param > 0 ? param : 6454);
    } else if (source == ANS_TC_SOURCE_MTC) {
        cfg.clockType = SyncClockType::MidiMTC;
        cfg.midiInputDeviceIndex = param;
    } else {
        setErr(sdk, QStringLiteral("未知时间码源: %1").arg(source));
        return -1;
    }

    ISyncClock *clk = SyncClockFactory::instance().create(cfg.clockType);
    if (!clk) { setErr(sdk, QStringLiteral("创建时间码时钟失败")); return -1; }

    // 收到外部时间码即按文件帧率定位并输出
    QObject::connect(clk, &ISyncClock::frameTick, clk,
                     [sdk](qint64, qint64 ts) { sendFrameAt(sdk, ts, 0); });
    QObject::connect(clk, &ISyncClock::errorOccurred, clk,
                     [sdk](const QString &e) { setErr(sdk, e); });

    if (!clk->start(cfg)) {
        setErr(sdk, QStringLiteral("时间码监听启动失败 (端口/设备不可用?)"));
        delete clk;
        return -1;
    }
    sdk->listener = clk;
    sdk->error.clear();
    return 0;
}

ANS_API void ans_stop_timecode_listener(ArtNetSdk* sdk) {
    if (!sdk || !sdk->listener) return;
    sdk->listener->stop();
    delete sdk->listener;
    sdk->listener = nullptr;
}

ANS_API int ans_set_timecode_smpte(ArtNetSdk* sdk, int hh, int mm, int ss, int ff, int fps) {
    if (!sdk || !sdk->playbackOpen) {
        setErr(sdk, QStringLiteral("尚未打开录制文件"));
        return -1;
    }
    const double f = effectiveFps(sdk, fps);
    const int nominal = int(std::lround(f));
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59
        || ff < 0 || ff >= nominal) {
        setErr(sdk, QStringLiteral("SMPTE 数值越界: %1:%2:%3:%4")
                        .arg(hh).arg(mm).arg(ss).arg(ff));
        return -1;
    }
    // 非丢帧: 按名义帧率计数后换算为真实时间
    const qint64 totalFrames = qint64(hh * 3600 + mm * 60 + ss) * nominal + ff;
    const qint64 tsUs = qint64(std::llround(double(totalFrames) * 1000000.0 / f));
    return sendFrameAt(sdk, tsUs, fps);
}

ANS_API void ans_reset_playback(ArtNetSdk* sdk) {
    if (sdk) sdk->currentFrame = -1;
}

ANS_API long long ans_current_frame(ArtNetSdk* sdk) { return sdk ? sdk->currentFrame : -1; }
ANS_API int ans_total_frames(ArtNetSdk* sdk) {
    return (sdk && sdk->playbackOpen) ? int(sdk->reader.header().totalFrames) : 0;
}
ANS_API int ans_frames_sent(ArtNetSdk* sdk) { return sdk ? int(sdk->framesSent) : 0; }

/* ================= 录制 ================= */

ANS_API int ans_start_recording(ArtNetSdk* sdk, const char* path, int fps,
                                const int* universes, int universeCount) {
    if (!sdk) return -1;
    if (!path || !*path) { setErr(sdk, QStringLiteral("录制文件路径为空")); return -1; }
    if (universeCount <= 0 || !universes) {
        setErr(sdk, QStringLiteral("至少需要一个 universe"));
        return -1;
    }
    if (sdk->recording) {
        setErr(sdk, QStringLiteral("已在录制中, 请先 ans_stop_recording"));
        return -1;
    }

    QFile::remove(qs(path));
    ArtRecHeader h;
    h.frameRate = fps > 0 ? fps : 30;
    h.universeCount = quint16(universeCount);
    sdk->recordFps = h.frameRate;

    sdk->recordUniverses.clear();
    for (int i = 0; i < universeCount; ++i)
        sdk->recordUniverses << UniverseEntry(quint16(universes[i] & 0x7FFF), 512);

    if (!sdk->writer.create(qs(path), h, sdk->recordUniverses)) {
        setErr(sdk, QStringLiteral("创建录制文件失败: %1").arg(qs(path)));
        return -1;
    }
    sdk->recording = true;
    sdk->recordIndex = 0;
    sdk->firstTsUs = 0;
    sdk->lastTsUs = 0;
    sdk->error.clear();
    return 0;
}

ANS_API int ans_record_frame(ArtNetSdk* sdk, long long timestampUs,
                             const int* universes,
                             const unsigned char* const* dmxData,
                             const int* dmxLen, int count) {
    if (!sdk || !sdk->recording) {
        setErr(sdk, QStringLiteral("尚未开始录制"));
        return -1;
    }
    QMap<quint16, QByteArray> map;
    for (int i = 0; i < count; ++i) {
        if (!universes || !dmxData || !dmxData[i]) continue;
        const int len = qBound(0, dmxLen ? dmxLen[i] : 512, 512);
        map.insert(quint16(universes[i] & 0x7FFF),
                   QByteArray(reinterpret_cast<const char*>(dmxData[i]), len));
    }
    if (map.isEmpty()) { setErr(sdk, QStringLiteral("本帧没有有效 DMX 数据")); return -1; }

    if (sdk->recordIndex == 0) sdk->firstTsUs = timestampUs;
    sdk->lastTsUs = timestampUs;
    if (!sdk->writer.writeFrame(sdk->recordIndex, timestampUs, map, true)) {
        setErr(sdk, QStringLiteral("写入第 %1 帧失败").arg(sdk->recordIndex));
        return -1;
    }
    ++sdk->recordIndex;
    return 0;
}

ANS_API int ans_stop_recording(ArtNetSdk* sdk) {
    if (!sdk || !sdk->recording) return -1;
    const qint64 frames = sdk->recordIndex;
    const qint64 duration = sdk->lastTsUs > sdk->firstTsUs
                                ? sdk->lastTsUs - sdk->firstTsUs
                                : (frames > 0 ? qint64(1000000.0 / sdk->recordFps * frames) : 0);
    const bool ok = sdk->writer.finalize(frames, duration);
    sdk->recording = false;
    if (!ok) { setErr(sdk, QStringLiteral("录制文件收尾失败")); return -1; }
    return 0;
}

ANS_API long long ans_recorded_frames(ArtNetSdk* sdk) {
    return sdk ? sdk->recordIndex : 0;
}

/* ================= 自检 ================= */

ANS_API int ans_selftest(void) {
    const QString path = QDir::temp().filePath("artnet_sdk_selftest.artrec");
    QFile::remove(path);

    const int recordFrames = 1801;   // 需覆盖 00:01:00 位置才能校验丢帧转换
    const int pushFrames = 30;
    const int uni = 0;

    ArtNetSdk *sdk = ans_create();
    int result = 0;
    QString detail;

    // 1) 录制 30 帧, CH1 = 帧号 * 8
    if (ans_start_recording(sdk, path.toUtf8().constData(), 30, &uni, 1) != 0) {
        result = 1;
        detail = QStringLiteral("录制启动失败: %1").arg(qs(ans_last_error(sdk)));
    } else {
        for (int i = 0; i < recordFrames; ++i) {
            QByteArray dmx(512, 0);
            dmx[0] = char((i * 8) & 0xFF);
            const unsigned char *arr[1] = { reinterpret_cast<const unsigned char*>(dmx.constData()) };
            const int lens[1] = { 512 };
            if (ans_record_frame(sdk, qint64(i) * 33333, &uni, arr, lens, 1) != 0) {
                result = 2;
                detail = QStringLiteral("写入帧 %1 失败").arg(i);
                break;
            }
        }
        if (result == 0) ans_stop_recording(sdk);
    }

    QUdpSocket rx;
    if (result == 0 && !rx.bind(QHostAddress(QHostAddress::LocalHost), 0)) {
        result = 3;
        detail = QStringLiteral("UDP 绑定失败");
    }

    // 取最后一个 ArtDmx 包的 CH1 值
    auto ch1Of = [](const QByteArray &p) -> int {
        if (p.size() < 18 + 512) return -1;
        if (std::memcmp(p.constData(), "Art-Net\0", 8) != 0) return -1;
        const auto *d = reinterpret_cast<const uchar*>(p.constData());
        if (quint16(d[8] | (d[9] << 8)) != 0x5000) return -1;
        return int(d[18]);
    };
    auto drainRx = [&rx]() {
        while (rx.hasPendingDatagrams()) {
            QByteArray d;
            d.resize(int(rx.pendingDatagramSize()));
            rx.readDatagram(d.data(), d.size());
        }
    };
    auto readLast = [&rx](int timeoutMs, QByteArray *pkt) -> bool {
        if (!rx.waitForReadyRead(timeoutMs)) return false;
        bool got = false;
        while (rx.hasPendingDatagrams()) {
            QByteArray d;
            d.resize(int(rx.pendingDatagramSize()));
            if (rx.readDatagram(d.data(), d.size()) < 0) break;
            if (pkt) *pkt = d;
            got = true;
        }
        return got;
    };

    if (result == 0) {
        if (ans_open_recording(sdk, path.toUtf8().constData()) != 0) {
            result = 4;
            detail = QStringLiteral("打开录制失败: %1").arg(qs(ans_last_error(sdk)));
        } else if (ans_add_artnet_target(sdk, "127.0.0.1", rx.localPort()) <= 0
                   || ans_start_outputs(sdk) != 0) {
            result = 5;
            detail = QStringLiteral("输出启动失败: %1").arg(qs(ans_last_error(sdk)));
        } else {
            // 2) 宿主推送 30 个时间码 → 应输出 30 帧
            for (int i = 0; i < pushFrames; ++i)
                ans_set_timecode(sdk, qint64(i) * 1000000LL / 30, 30);

            int received = 0, bad = 0;
            rx.waitForReadyRead(300);
            while (rx.hasPendingDatagrams()) {
                QByteArray p;
                p.resize(int(rx.pendingDatagramSize()));
                if (rx.readDatagram(p.data(), p.size()) < 0) break;
                ++received;
                if (ch1Of(p) < 0 || ch1Of(p) % 8 != 0) ++bad;
            }
            const int sent = ans_frames_sent(sdk);
            if (sent != pushFrames || received < pushFrames - 2 || bad != 0) {
                result = 6;
                detail = QStringLiteral("推送时间码: 发送 %1/%2, 接收 %3, 异常包 %4")
                             .arg(sent).arg(pushFrames).arg(received).arg(bad);
            }
        }
    }

    // 3) SMPTE 字符串 / 丢帧 / 偏移 / 帧号 / 文件信息
    if (result == 0) {
        QString step;
        QByteArray pkt;

        drainRx();
        if (ans_set_timecode_string(sdk, "00:00:00:10", 30) != 0) {
            step = "SMPTE 字符串解析";
            result = 7;
        } else if (!readLast(300, &pkt) || ans_current_frame(sdk) != 10 || ch1Of(pkt) != 80) {
            step = QStringLiteral("SMPTE 定位 (帧 %1, CH1 %2)")
                       .arg(ans_current_frame(sdk)).arg(ch1Of(pkt));
            result = 7;
        }

        if (result == 0) {
            drainRx();
            // 非丢帧: 00:01:00:00 @30 = 第 1800 帧
            if (ans_set_timecode_string(sdk, "00:01:00:00", 30) != 0
                || !readLast(300, &pkt)
                || ans_current_frame(sdk) != 1800 || ch1Of(pkt) != 64) {
                step = QStringLiteral("SMPTE 分钟位 (帧 %1, CH1 %2)")
                           .arg(ans_current_frame(sdk)).arg(ch1Of(pkt));
                result = 8;
            }
        }

        if (result == 0) {
            drainRx();
            // 29.97 丢帧: 00:01:00;00 = 1800 - 2 = 第 1798 帧 (整分钟丢 2 帧)
            if (ans_set_timecode_string(sdk, "00:01:00;00", 2997) != 0
                || !readLast(300, &pkt)
                || ans_current_frame(sdk) != 1798 || ch1Of(pkt) != 48) {
                step = QStringLiteral("丢帧时间码 (帧 %1, CH1 %2)")
                           .arg(ans_current_frame(sdk)).arg(ch1Of(pkt));
                result = 8;
            }
        }

        if (result == 0) {
            drainRx();
            ans_set_timecode_offset(sdk, 3600LL * 1000000LL);   // 01:00:00:00 → 文件第 0 帧
            if (ans_set_timecode_string(sdk, "01:00:00:20", 30) != 0
                || !readLast(300, &pkt)
                || ans_current_frame(sdk) != 20 || ch1Of(pkt) != 160) {
                step = QStringLiteral("时间码偏移 (帧 %1, CH1 %2)")
                           .arg(ans_current_frame(sdk)).arg(ch1Of(pkt));
                result = 9;
            }
        }

        if (result == 0) {
            drainRx();
            if (ans_set_timecode_frame(sdk, 5, 30) != 0
                || !readLast(300, &pkt)
                || ans_current_frame(sdk) != 5 || ch1Of(pkt) != 40) {
                step = QStringLiteral("帧号驱动 (帧 %1, CH1 %2)")
                           .arg(ans_current_frame(sdk)).arg(ch1Of(pkt));
                result = 10;
            }
        }

        AnsPlaybackInfo info;
        if (result == 0 && (ans_playback_info(sdk, &info) != 0
                            || info.totalFrames != recordFrames
                            || info.frameRateX100 != 3000
                            || info.universeCount != 1)) {
            step = QStringLiteral("文件信息 (帧率x100 %1, 总帧 %2, 域 %3)")
                       .arg(info.frameRateX100).arg(info.totalFrames).arg(info.universeCount);
            result = 11;
        }

        ans_set_timecode_offset(sdk, 0);

        if (result == 0) {
            drainRx();
            // 29.97 便捷入口: 00:01:00:00 (非丢帧) = 第 1800 帧
            if (ans_set_timecode_smpte(sdk, 0, 1, 0, 0, 2997) != 0
                || !readLast(300, &pkt)
                || ans_current_frame(sdk) != 1800 || ch1Of(pkt) != 64) {
                step = QStringLiteral("SMPTE 便捷入口 (帧 %1, CH1 %2)")
                           .arg(ans_current_frame(sdk)).arg(ch1Of(pkt));
                result = 12;
            }
        }

        if (result == 0) {
            detail = QStringLiteral("录制 %1 帧 → 推送时间码 %2/%2 → SMPTE/丢帧/偏移/帧号/文件信息 → PASS")
                         .arg(recordFrames).arg(pushFrames);
        } else {
            detail = step;
        }
    }

    ans_destroy(sdk);
    QFile::remove(path);

    g_selftestText = detail;
    return result;
}

ANS_API const char* ans_selftest_result(void) {
    static QByteArray buf;
    buf = g_selftestText.toUtf8();
    return buf.constData();
}

} // extern "C"
