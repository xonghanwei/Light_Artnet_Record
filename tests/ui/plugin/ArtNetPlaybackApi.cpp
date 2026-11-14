// ArtNetPlaybackApi.cpp — 外部时间码驱动的 .artrec 重演 C API
//
// 时间码 → 帧号 → 读取录制帧 → OutputManager 发送 (Art-Net / DMX512)
#include "ArtNetUiPlugin.h"
#include "fileformat/ArtRecReader.h"
#include "fileformat/ArtRecWriter.h"
#include "output/OutputManager.h"

#include <QString>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMap>
#include <QByteArray>
#include <QUdpSocket>
#include <QHostAddress>
#include <QCoreApplication>
#include <QThread>
#include <cmath>
#include <cstring>

static QString g_playbackSelftestText;

struct PlaybackCtx {
    ArtRecReader reader;
    OutputManager *out = nullptr;
    bool opened = false;
    bool targetReady = false;
    qint64 currentFrame = -1;
    quint64 framesSent = 0;
    QString error;
};

static PlaybackCtx* ctxOf(void *p) { return static_cast<PlaybackCtx*>(p); }

static void setError(PlaybackCtx *c, const QString &s) {
    if (c) c->error = s;
}

extern "C" {

ARTNET_UI_PLUGIN_API void* artnet_ui_playback_create(void) {
    auto *c = new PlaybackCtx;
    c->out = new OutputManager;
    return c;
}

ARTNET_UI_PLUGIN_API void artnet_ui_playback_destroy(void* ctx) {
    auto *c = ctxOf(ctx);
    if (!c) return;
    if (c->out) {
        c->out->stop();
        delete c->out;
        c->out = nullptr;
    }
    delete c;
}

ARTNET_UI_PLUGIN_API int artnet_ui_playback_open(void* ctx, const char* artrecPath) {
    auto *c = ctxOf(ctx);
    if (!c || !artrecPath) return -1;
    if (!c->reader.open(QString::fromUtf8(artrecPath))) {
        setError(c, QStringLiteral("打开录制文件失败: %1").arg(QString::fromUtf8(artrecPath)));
        c->opened = false;
        return -1;
    }
    c->opened = true;
    c->currentFrame = -1;
    c->framesSent = 0;
    c->error.clear();
    return 0;
}

ARTNET_UI_PLUGIN_API int artnet_ui_playback_set_artnet_target(void* ctx, const char* ip, int port) {
    auto *c = ctxOf(ctx);
    if (!c || !c->out) return -1;
    if (!ip || !*ip) {
        setError(c, QStringLiteral("Art-Net 目标 IP 为空"));
        return -1;
    }
    c->out->addArtNetTarget(QString::fromUtf8(ip), quint16(port > 0 ? port : 6454));
    if (!c->out->start()) {
        setError(c, QStringLiteral("Art-Net 输出启动失败"));
        return -1;
    }
    c->targetReady = true;
    return 0;
}

ARTNET_UI_PLUGIN_API int artnet_ui_playback_set_dmx_port(void* ctx, const char* portName) {
    auto *c = ctxOf(ctx);
    if (!c || !c->out) return -1;
    if (!portName || !*portName) {
        setError(c, QStringLiteral("DMX 串口名为空"));
        return -1;
    }
    const int id = c->out->addDmx512Target(QString::fromUtf8(portName), 250000);
    if (id <= 0) {
        setError(c, QStringLiteral("添加 DMX 输出目标失败: %1").arg(QString::fromUtf8(portName)));
        return -1;
    }
    if (!c->out->start()) {
        setError(c, QStringLiteral("DMX 输出启动失败 (串口被占用?)"));
        return -1;
    }
    c->targetReady = true;
    return 0;
}

ARTNET_UI_PLUGIN_API int artnet_ui_playback_set_timecode(void* ctx, long long timestampUs, int fps) {
    auto *c = ctxOf(ctx);
    if (!c || !c->opened) { setError(c, QStringLiteral("尚未打开录制文件")); return -1; }
    if (timestampUs < 0) { setError(c, QStringLiteral("时间码为负")); return -1; }

    const ArtRecHeader &h = c->reader.header();
    double f = double(fps);
    if (f == 2997.0) f = 29.97;
    if (f <= 0) {
        f = (h.frameRate == 2997) ? 29.97 : double(h.frameRate);
    }
    if (f <= 0) f = 30.0;

    qint64 frame = qint64(std::floor(double(timestampUs) * f / 1000000.0 + 0.5));
    const qint64 total = qint64(h.totalFrames);
    if (total > 0) frame = qBound<qint64>(0, frame, total - 1);

    if (frame == c->currentFrame) return 1;      // 重复时间码, 不重复发送

    if (!c->reader.seekToFrame(frame)) {
        setError(c, QStringLiteral("定位帧 %1 失败").arg(frame));
        return -1;
    }
    qint64 idx = -1, tsUs = 0;
    QMap<int, QByteArray> universeData;
    if (!c->reader.readNextFrame(idx, tsUs, universeData)) {
        setError(c, QStringLiteral("读取帧 %1 失败").arg(frame));
        return -1;
    }

    const QList<UniverseEntry> &unis = c->reader.universes();
    int sentUniverses = 0;
    for (auto it = universeData.constBegin(); it != universeData.constEnd(); ++it) {
        quint16 uni = 0;
        if (it.key() >= 0 && it.key() < unis.size())
            uni = unis.at(it.key()).encoded();
        c->out->sendDmxFrame(uni, it.value());
        ++sentUniverses;
    }

    c->currentFrame = frame;
    ++c->framesSent;
    if (sentUniverses == 0) {
        // 帧存在但没有 DMX 数据块 (例如仅有 RDM), 仍算已定位
        c->error = QStringLiteral("帧 %1 无 DMX 数据").arg(frame);
    }
    return 0;
}

ARTNET_UI_PLUGIN_API int artnet_ui_playback_set_timecode_smpte(void* ctx, int hh, int mm,
                                                               int ss, int ff, int fps) {
    if (fps <= 0) fps = 30;
    const long long totalFrames = (long long)hh * 3600 * fps + (long long)mm * 60 * fps
                                  + (long long)ss * fps + ff;
    const long long us = totalFrames * 1000000LL / fps;
    return artnet_ui_playback_set_timecode(ctx, us, fps);
}

ARTNET_UI_PLUGIN_API void artnet_ui_playback_reset(void* ctx) {
    auto *c = ctxOf(ctx);
    if (c) c->currentFrame = -1;
}

ARTNET_UI_PLUGIN_API long long artnet_ui_playback_current_frame(void* ctx) {
    auto *c = ctxOf(ctx);
    return c ? c->currentFrame : -1;
}

ARTNET_UI_PLUGIN_API int artnet_ui_playback_total_frames(void* ctx) {
    auto *c = ctxOf(ctx);
    if (!c || !c->opened) return 0;
    return int(c->reader.header().totalFrames);
}

ARTNET_UI_PLUGIN_API int artnet_ui_playback_frames_sent(void* ctx) {
    auto *c = ctxOf(ctx);
    return c ? int(c->framesSent) : 0;
}

ARTNET_UI_PLUGIN_API const char* artnet_ui_playback_last_error(void* ctx) {
    static QByteArray buf;
    auto *c = ctxOf(ctx);
    buf = c ? c->error.toUtf8() : QByteArray("invalid context");
    return buf.constData();
}

ARTNET_UI_PLUGIN_API int artnet_ui_playback_selftest(void) {
    // 1) 生成 30 帧临时录制文件 (1 个 Universe, CH1 = 帧号 * 8)
    const QString path = QDir::temp().filePath("artnet_playback_selftest.artrec");
    QFile::remove(path);

    ArtRecHeader h;
    h.frameRate = 30;
    h.universeCount = 1;
    QList<UniverseEntry> unis;
    unis << UniverseEntry(0, 512);

    ArtRecWriter w;
    if (!w.create(path, h, unis)) {
        g_playbackSelftestText = QStringLiteral("创建临时 .artrec 失败: %1").arg(path);
        return 1;
    }

    const int frames = 30;
    for (int i = 0; i < frames; ++i) {
        QByteArray dmx(512, 0);
        dmx[0] = char((i * 8) & 0xFF);
        QMap<quint16, QByteArray> m;
        m[0] = dmx;
        if (!w.writeFrame(i, qint64(i) * 33333, m, true)) {
            w.close();
            QFile::remove(path);
            g_playbackSelftestText = QStringLiteral("写入第 %1 帧失败").arg(i);
            return 2;
        }
    }
    w.finalize(frames, qint64(frames) * 33333);

    // 2) 本机 UDP 接收端
    QUdpSocket rx;
    if (!rx.bind(QHostAddress(QHostAddress::LocalHost), 0)) {
        QFile::remove(path);
        g_playbackSelftestText = QStringLiteral("UDP 接收 socket 绑定失败");
        return 3;
    }
    const quint16 port = rx.localPort();

    // 3) 通过回放 API 逐帧送入时间码
    void *ctx = artnet_ui_playback_create();
    int result = 0;
    QString detail;
    if (artnet_ui_playback_open(ctx, path.toUtf8().constData()) != 0) {
        result = 4;
        detail = QStringLiteral("打开失败: %1").arg(QString::fromUtf8(artnet_ui_playback_last_error(ctx)));
    } else if (artnet_ui_playback_set_artnet_target(ctx, "127.0.0.1", port) != 0) {
        result = 5;
        detail = QStringLiteral("设置目标失败: %1").arg(QString::fromUtf8(artnet_ui_playback_last_error(ctx)));
    } else {
        for (int i = 0; i < frames; ++i) {
            const long long us = qint64(i) * 1000000LL / 30;
            artnet_ui_playback_set_timecode(ctx, us, 30);
        }
        // 回收并校验
        int received = 0, bad = 0;
        QThread::msleep(120);
        while (rx.hasPendingDatagrams()) {
            QByteArray p;
            p.resize(int(rx.pendingDatagramSize()));
            if (rx.readDatagram(p.data(), p.size()) < 0) break;
            ++received;
            if (p.size() < 18 + 512) { ++bad; continue; }
            const auto *d = reinterpret_cast<const uchar*>(p.constData());
            if (std::memcmp(p.constData(), "Art-Net\0", 8) != 0 || quint16(d[8] | (d[9] << 8)) != 0x5000)
                ++bad;
            else if (uchar(d[18]) % 8 != 0)
                ++bad;
        }
        const int sent = artnet_ui_playback_frames_sent(ctx);
        if (sent != frames || received < frames - 2 || bad != 0) {
            result = 6;
            detail = QStringLiteral("发送 %1/%2, 接收 %3, 异常包 %4").arg(sent).arg(frames).arg(received).arg(bad);
        } else {
            detail = QStringLiteral("发送 %1 帧, 接收 %2, 异常包 %3 → PASS")
                         .arg(sent).arg(received).arg(bad);
        }
    }

    artnet_ui_playback_destroy(ctx);
    QFile::remove(path);

    g_playbackSelftestText = detail;
    return result;
}

ARTNET_UI_PLUGIN_API const char* artnet_ui_playback_selftest_result(void) {
    static QByteArray buf;
    buf = g_playbackSelftestText.toUtf8();
    return buf.constData();
}

} // extern "C"
