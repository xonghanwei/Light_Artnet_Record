// sdk/test/tc_listener_test.cpp — 内建 Art-Net TimeCode 监听 → 同步重演 验证
//
// 与 main.cpp 的区别: 这里创建 QCoreApplication, 让 DLL 内的 QUdpSocket/QTimer
// 能正常派发事件, 从而验证 "DLL 自己接收外部时间码" 这条链路.
#include "ArtNetSdk.h"

#include <QCoreApplication>
#include <QUdpSocket>
#include <QHostAddress>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QSet>
#include <QStringList>
#include <cstdio>
#include <cstring>

static int g_pass = 0, g_fail = 0;

static void check(const char *name, bool ok, const QString &detail = QString()) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
                detail.isEmpty() ? "" : " — ", detail.toLocal8Bit().constData());
    ok ? ++g_pass : ++g_fail;
}

// Art-Net TimeCode 包 (OpCode 0x0D00), 13 字节
static QByteArray makeArtNetTc(int hh, int mm, int ss, int ff, int smpteType = 3) {
    QByteArray p(13, 0);
    p[0] = char(0x00);
    p[1] = char(0x0D);
    p[2] = 0;
    p[3] = 14;
    p[6] = char((smpteType & 0x0F) << 4);
    p[7] = char(hh & 0x3F);
    p[8] = char(mm & 0x3F);
    p[9] = char(ss & 0x3F);
    p[10] = char(ff & 0x1F);
    return p;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const QString path = QDir::temp().filePath("artnet_sdk_tc_listener.artrec");
    QFile::remove(path);

    // 准备 30 帧素材: CH1 = 帧号 * 8
    ArtNetSdk *sdk = ans_create();
    const int uni = 0;
    if (ans_start_recording(sdk, path.toUtf8().constData(), 30, &uni, 1) != 0) {
        std::printf("[FAIL] 录制启动: %s\n", ans_last_error(sdk));
        return 1;
    }
    for (int i = 0; i < 30; ++i) {
        QByteArray dmx(512, 0);
        dmx[0] = char((i * 8) & 0xFF);
        const unsigned char *arr[1] = { reinterpret_cast<const unsigned char*>(dmx.constData()) };
        const int lens[1] = { 512 };
        ans_record_frame(sdk, qint64(i) * 33333, &uni, arr, lens, 1);
    }
    ans_stop_recording(sdk);

    QUdpSocket rx;
    if (!rx.bind(QHostAddress(QHostAddress::LocalHost), 0)) {
        std::printf("[FAIL] UDP 绑定失败\n");
        return 1;
    }

    if (ans_open_recording(sdk, path.toUtf8().constData()) != 0
        || ans_add_artnet_target(sdk, "127.0.0.1", rx.localPort()) <= 0
        || ans_start_outputs(sdk) != 0) {
        std::printf("[FAIL] 打开/输出启动: %s\n", ans_last_error(sdk));
        return 1;
    }

    // 找一个空闲端口作为 TC 监听口
    QUdpSocket probe;
    probe.bind(QHostAddress(QHostAddress::LocalHost), 0);
    const quint16 tcPort = probe.localPort();
    probe.close();

    check("启动 Art-Net TimeCode 监听",
          ans_start_timecode_listener(sdk, ANS_TC_SOURCE_ARTNET, tcPort, 30) == 0,
          ans_last_error(sdk));

    QUdpSocket tx;
    auto sendTc = [&](int frame) {
        tx.writeDatagram(makeArtNetTc(0, 0, 0, frame), QHostAddress::LocalHost, tcPort);
    };

    QTimer::singleShot(100, [&] { sendTc(5); });
    QTimer::singleShot(250, [&] { sendTc(10); });
    QTimer::singleShot(400, [&] { sendTc(15); });

    QTimer::singleShot(900, [&] {
        QSet<int> ch1;
        int packets = 0, bad = 0;
        while (rx.hasPendingDatagrams()) {
            QByteArray p;
            p.resize(int(rx.pendingDatagramSize()));
            if (rx.readDatagram(p.data(), p.size()) < 0) break;
            ++packets;
            const auto *d = reinterpret_cast<const uchar*>(p.constData());
            if (p.size() < 530 || std::memcmp(p.constData(), "Art-Net\0", 8) != 0
                || quint16(d[8] | (d[9] << 8)) != 0x5000)
                ++bad;
            else
                ch1.insert(int(d[18]));
        }
        check("收到回放数据", packets > 0, QString("%1 packets").arg(packets));
        QStringList ch1Text;
        for (int v : ch1) ch1Text << QString::number(v);
        check("通道值对应时间码帧", ch1.contains(40) && ch1.contains(80) && ch1.contains(120),
              "CH1=" + ch1Text.join(","));
        check("无异常包", bad == 0);
        check("回放状态=已收到时间码", ans_playback_state(sdk) == 2);

        ans_stop_timecode_listener(sdk);
        ans_destroy(sdk);
        QFile::remove(path);

        std::printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
        app.exit(g_fail == 0 ? 0 : 1);
    });

    return app.exec();
}
