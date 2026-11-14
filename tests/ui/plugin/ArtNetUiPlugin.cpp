// ArtNetUiPlugin.cpp — 把功能测试 UI 封装为可被宿主加载的 DLL
#include "ArtNetUiPlugin.h"
#include "../demos/ArtNetRecorderMainWindow.h"

#include <QApplication>
#include <QWidget>
#include <QUdpSocket>
#include <QHostAddress>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QThread>
#include <cstring>

static QString g_lastResult;

static void setResult(const QString &s) { g_lastResult = s; }

// ArtDmx: "Art-Net\0" + OpCode 0x5000 + ProtVer 14 + Seq/Phys + SubUni/Net + Len(be) + 512
static QByteArray buildArtDmx(quint16 universe, const QByteArray &dmx, quint8 seq) {
    QByteArray pkt(18 + 512, 0);
    std::memcpy(pkt.data(), "Art-Net\0", 8);
    auto *d = reinterpret_cast<uchar*>(pkt.data());
    d[8] = 0x00; d[9] = 0x50;                    // OpDMX = 0x5000 (little endian)
    d[10] = 0;   d[11] = 14;                     // ProtVer
    d[12] = seq; d[13] = 0;                      // Sequence / Physical
    d[14] = uchar(universe & 0xFF);              // SubUni
    d[15] = uchar((universe >> 8) & 0x7F);       // Net
    d[16] = 0x02; d[17] = 0x00;                  // Length = 512 (big endian)
    const int n = qMin<int>(512, dmx.size());
    std::memcpy(d + 18, dmx.constData(), n);
    return pkt;
}

extern "C" {

ARTNET_UI_PLUGIN_API const char* artnet_ui_plugin_version(void) {
    return "1.0.0";
}

ARTNET_UI_PLUGIN_API const char* artnet_ui_plugin_qt_version(void) {
    return qVersion();
}

ARTNET_UI_PLUGIN_API void* artnet_ui_plugin_create_window(void* qapp, void* parent) {
    if (!qapp) {
        setResult(QStringLiteral("创建窗口失败: qapp 为空"));
        return nullptr;
    }
    if (!qobject_cast<QApplication*>(static_cast<QObject*>(qapp))) {
        setResult(QStringLiteral("创建窗口失败: 传入的 qapp 不是 QApplication"));
        return nullptr;
    }
    auto *w = new ArtNetRecorderMainWindow(static_cast<QWidget*>(parent));
    w->setAttribute(Qt::WA_DeleteOnClose, true);
    w->show();
    setResult(QStringLiteral("窗口已创建"));
    return w;
}

ARTNET_UI_PLUGIN_API void artnet_ui_plugin_destroy_window(void* window) {
    if (!window) return;
    auto *w = static_cast<QWidget*>(window);
    w->close();
    w->deleteLater();
}

ARTNET_UI_PLUGIN_API int artnet_ui_plugin_selftest(int count) {
    if (count <= 0) count = 20;

    QUdpSocket rx;
    if (!rx.bind(QHostAddress(QHostAddress::LocalHost), 0)) {
        setResult(QStringLiteral("自检失败: 接收 socket 绑定失败 (%1)").arg(rx.errorString()));
        return 1;
    }
    const quint16 port = rx.localPort();

    QUdpSocket tx;
    QByteArray dmx(512, 0);
    dmx[0] = char(200);                          // CH1 = 200, 其余 0

    int sent = 0;
    for (int i = 0; i < count; ++i) {
        const QByteArray pkt = buildArtDmx(0, dmx, quint8(i & 0xFF));
        if (tx.writeDatagram(pkt, QHostAddress(QHostAddress::LocalHost), port) == pkt.size())
            ++sent;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        QThread::msleep(2);
    }

    int received = 0, badLen = 0, badId = 0, badCh1 = 0;
    QElapsedTimer t; t.start();
    while (t.elapsed() < 2000 && received < sent) {
        while (rx.hasPendingDatagrams()) {
            const qint64 sz = rx.pendingDatagramSize();
            if (sz <= 0) break;
            QByteArray p;
            p.resize(int(sz));
            if (rx.readDatagram(p.data(), p.size()) < 0) break;
            ++received;
            if (p.size() < 18 + 512) { ++badLen; continue; }
            if (std::memcmp(p.constData(), "Art-Net\0", 8) != 0) { ++badId; continue; }
            const auto *d = reinterpret_cast<const uchar*>(p.constData());
            if (quint16(d[8] | (d[9] << 8)) != 0x5000) { ++badId; continue; }
            if (uchar(d[18]) != 200) { ++badCh1; continue; }
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }

    const bool ok = (sent == count) && (received == sent)
                    && (badLen == 0) && (badId == 0) && (badCh1 == 0);
    setResult(QStringLiteral("Art-Net 本机回环自检: 发送 %1, 接收 %2, 长度错误 %3, 标识错误 %4, CH1 错误 %5 → %6")
                  .arg(sent).arg(received).arg(badLen).arg(badId).arg(badCh1)
                  .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
    return ok ? 0 : 2;
}

ARTNET_UI_PLUGIN_API const char* artnet_ui_plugin_last_result(void) {
    static QByteArray buf;
    buf = g_lastResult.toUtf8();
    return buf.constData();
}

} // extern "C"
