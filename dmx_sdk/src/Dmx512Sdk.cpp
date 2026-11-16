// Dmx512Sdk.cpp - DMX512 串口读写 SDK 实现
//
// 内部复用生产代码: Dmx512Sender (发送, 含 BREAK/MAB 时序与降级)
//                  Dmx512Source (接收, 513 字节帧解析)
// 接收运行在 SDK 自建 QThread 上, 宿主 C 程序无需 Qt 事件循环。
//
#define DMX512_SDK_VERSION "1.0.0"

#include "Dmx512Sdk.h"

#include "output/Dmx512Sender.h"
#include "protocol/sources/Dmx512Source.h"
#include "protocol/IProtocolSource.h"
#include "protocol/ProtocolTypes.h"
#include "core/Common.h"

#include <QCoreApplication>
#include <QThread>
#include <QSerialPortInfo>
#include <QString>
#include <QByteArray>

#include <cstring>
#include <string>

namespace {

struct DmxContext {
    Dmx512Sender *sender = nullptr;
    QThread *rxThread = nullptr;          // 承载 source 的工作线程
    IProtocolSource *source = nullptr;    // Dmx512Source 实例, 属 rxThread
    bool rxRunning = false;

    Dmx512FrameCallback frameCb = nullptr;
    void *frameUser = nullptr;
    Dmx512ErrorCallback errCb = nullptr;
    void *errUser = nullptr;

    std::string lastError;
    std::string version = DMX512_SDK_VERSION;
};

DmxContext *g_ctx = nullptr;

void setLastError(const QString &e) {
    if (g_ctx) g_ctx->lastError = e.toUtf8().constData();
}

void clearLastError() {
    if (g_ctx) g_ctx->lastError.clear();
}

ConnectionParams makeSerialParams(const QString &port, int baud) {
    ConnectionParams p;
    p.protocol = ProtocolType::Dmx512;
    p.serialPortName = port;
    p.serialBaudRate = baud > 0 ? baud : 250000;
    return p;
}

} // namespace

extern "C" {

DMX512_SDK_API int dmx_init(void) {
    if (g_ctx) return 0;
    clearLastError();

    // 接收线程依赖 Qt 事件派发 (postEvent/queued invoke)。纯 C 宿主没有应用对象时,
    // SDK 自动创建进程级 QCoreApplication。
    // 注意: Qt 只允许一个应用实例 —— 宿主若为 Qt 程序, 必须先自行创建
    // QApplication/QCoreApplication 再调 dmx_init, 否则二次创建会 abort。
    if (!QCoreApplication::instance()) {
        static int s_argc = 0;
        new QCoreApplication(s_argc, nullptr);
    }

    try {
        g_ctx = new DmxContext();
    } catch (...) {
        return -1;
    }

    g_ctx->sender = new Dmx512Sender();
    // 发送侧错误统一记录到 lastError (open 失败已同步返回, 此处兜底运行期错误)
    QObject::connect(g_ctx->sender, &Dmx512Sender::portError,
                     g_ctx->sender, [](const QString &err) { setLastError(err); });
    return 0;
}

DMX512_SDK_API void dmx_shutdown(void) {
    if (!g_ctx) return;

    dmx_stop_rx();

    if (g_ctx->sender) {
        g_ctx->sender->close();
        delete g_ctx->sender;
    }
    delete g_ctx;
    g_ctx = nullptr;
}

DMX512_SDK_API const char *dmx_version_string(void) {
    static const char kVer[] = DMX512_SDK_VERSION;
    return kVer;
}

DMX512_SDK_API const char *dmx_last_error(void) {
    if (!g_ctx) return "";
    return g_ctx->lastError.c_str();
}

/* ==================== 发送 ==================== */

DMX512_SDK_API int dmx_open_port(const char *port, int baud) {
    if (!g_ctx || !g_ctx->sender || !port) {
        setLastError(QStringLiteral("dmx_open_port: 未初始化或参数为空"));
        return -1;
    }
    clearLastError();
    const QString name = QString::fromUtf8(port);
    if (!g_ctx->sender->open(name, baud > 0 ? baud : 250000)) {
        if (g_ctx->lastError.empty())
            setLastError(QStringLiteral("无法打开串口 %1").arg(name));
        return -1;
    }
    return 0;
}

DMX512_SDK_API void dmx_close_port(void) {
    if (!g_ctx || !g_ctx->sender) return;
    g_ctx->sender->close();
}

DMX512_SDK_API int dmx_is_open(void) {
    if (!g_ctx || !g_ctx->sender) return 0;
    return g_ctx->sender->isOpen() ? 1 : 0;
}

DMX512_SDK_API int dmx_send_frame(const unsigned char *channels, int count) {
    if (!g_ctx || !g_ctx->sender) {
        setLastError(QStringLiteral("dmx_send_frame: 未初始化"));
        return -1;
    }
    if (count < 1 || count > DMX512_CHANNELS || (!channels && count > 0)) {
        setLastError(QStringLiteral("dmx_send_frame: 通道数 %1 超出 1..%2")
                         .arg(count).arg(DMX512_CHANNELS));
        return -1;
    }
    clearLastError();
    QByteArray data(reinterpret_cast<const char *>(channels), count);
    return g_ctx->sender->sendFrame(data) ? 0 : -1;
}

DMX512_SDK_API int dmx_send_raw(const unsigned char *data, int len) {
    if (!g_ctx || !g_ctx->sender) {
        setLastError(QStringLiteral("dmx_send_raw: 未初始化"));
        return -1;
    }
    if (len < 1 || len > DMX512_CHANNELS + 1 || !data) {
        setLastError(QStringLiteral("dmx_send_raw: 长度 %1 超出 1..%2")
                         .arg(len).arg(DMX512_CHANNELS + 1));
        return -1;
    }
    clearLastError();
    QByteArray raw(reinterpret_cast<const char *>(data), len);
    // 注意: sendRaw 不含 BREAK/MAB, 由调用方控制时序 (自定义 Start Code 场景)
    return g_ctx->sender->sendRaw(raw) ? 0 : -1;
}

DMX512_SDK_API unsigned long long dmx_frames_sent(void) {
    if (!g_ctx || !g_ctx->sender) return 0;
    return static_cast<unsigned long long>(g_ctx->sender->totalSentFrames());
}

/* ==================== 接收 ==================== */

DMX512_SDK_API int dmx_start_rx(const char *port, int baud) {
    if (!g_ctx || !port) {
        setLastError(QStringLiteral("dmx_start_rx: 未初始化或参数为空"));
        return -1;
    }
    if (g_ctx->rxRunning) {
        setLastError(QStringLiteral("dmx_start_rx: 接收已在运行"));
        return -1;
    }
    clearLastError();

    g_ctx->rxThread = new QThread();
    g_ctx->rxThread->setObjectName("dmx_rx");
    auto *src = new Dmx512Source();          // 无 parent
    src->moveToThread(g_ctx->rxThread);      // 必须迁移, 否则 BlockingQueuedConnection 死锁
    g_ctx->source = src;

    // 信号桥: context=source → 回调在 rxThread 上直接执行
    QObject::connect(src, &IProtocolSource::frameReceived, src,
                     [](const LightFrame &f) {
                         if (g_ctx && g_ctx->frameCb && f.dmxData.size() > 0) {
                             g_ctx->frameCb(
                                 reinterpret_cast<const unsigned char *>(f.dmxData.constData()),
                                 f.dmxData.size(), g_ctx->frameUser);
                         }
                     });
    QObject::connect(src, &IProtocolSource::errorOccurred, src,
                     [](const QString &err) {
                         setLastError(err);
                         if (g_ctx && g_ctx->errCb)
                             g_ctx->errCb(err.toUtf8().constData(), g_ctx->errUser);
                     });

    g_ctx->rxThread->start();

    const ConnectionParams params = makeSerialParams(QString::fromUtf8(port), baud);
    bool ok = false;
    QMetaObject::invokeMethod(
        src, [src, params, &ok]() { ok = src->start(params); },
        Qt::BlockingQueuedConnection);   // 串口在 rxThread 内创建, 结果同步取回

    if (!ok) {
        g_ctx->source = nullptr;
        g_ctx->rxThread->quit();
        g_ctx->rxThread->wait(2000);
        delete src;   // 线程已退出, deleteLater 不会再被处理, 直接删除
        delete g_ctx->rxThread;
        g_ctx->rxThread = nullptr;
        if (g_ctx->lastError.empty())
            setLastError(QStringLiteral("无法在 %1 启动 DMX 接收").arg(QString::fromUtf8(port)));
        return -1;
    }
    g_ctx->rxRunning = true;
    return 0;
}

DMX512_SDK_API void dmx_stop_rx(void) {
    if (!g_ctx || !g_ctx->rxRunning) return;

    QMetaObject::invokeMethod(g_ctx->source, [this_src = g_ctx->source]() { this_src->stop(); },
                              Qt::BlockingQueuedConnection);
    g_ctx->rxRunning = false;

    auto *src = g_ctx->source;
    g_ctx->source = nullptr;
    g_ctx->rxThread->quit();
    g_ctx->rxThread->wait(3000);
    delete src;       // 线程已退出, deleteLater 不会再被处理, 直接删除
    delete g_ctx->rxThread;
    g_ctx->rxThread = nullptr;
}

DMX512_SDK_API int dmx_is_rx_running(void) {
    return (g_ctx && g_ctx->rxRunning) ? 1 : 0;
}

DMX512_SDK_API unsigned long long dmx_frames_received(void) {
    if (!g_ctx || !g_ctx->source || !g_ctx->rxRunning) return 0;
    // source 属 rxThread, framesReceived 是普通 quint64 计数, 直接读近似安全
    return static_cast<unsigned long long>(g_ctx->source->framesReceived());
}

/* ==================== 回调 ==================== */

DMX512_SDK_API void dmx_set_frame_callback(Dmx512FrameCallback cb, void *user) {
    if (!g_ctx) return;
    g_ctx->frameCb = cb;
    g_ctx->frameUser = user;
}

DMX512_SDK_API void dmx_set_error_callback(Dmx512ErrorCallback cb, void *user) {
    if (!g_ctx) return;
    g_ctx->errCb = cb;
    g_ctx->errUser = user;
}

/* ==================== 工具 ==================== */

DMX512_SDK_API int dmx_list_ports(char *buf, int buf_size) {
    QByteArray out;
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos) {
        out += info.portName().toUtf8();
        out += ';';
    }
    const int need = out.size() + 1;
    if (buf && buf_size > 0) {
        const int cp = qMin(buf_size - 1, static_cast<int>(out.size()));
        std::memcpy(buf, out.constData(), cp);
        buf[cp] = '\0';
    }
    return need;
}

} // extern "C"
