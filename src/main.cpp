// main.cpp - ArtNet 录制器命令行入口
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QStringList>
#include <QTimer>

#include "core/Common.h"
#include "core/Logger.h"
#include "recorder/RecorderEngine.h"
#include "fileformat/ArtRecReader.h"
#include "output/OutputManager.h"
#include "output/ArtNetSender.h"
#include "output/Dmx512Sender.h"
#include "protocol/ProtocolFactory.h"
#include "protocol/ProtocolTypes.h"
#include "protocol/sinks/NpuSink.h"

#include <csignal>
#include <atomic>

// 全局中断标志
std::atomic<bool> g_interrupted(false);

void signalHandler(int sig) {
    Q_UNUSED(sig);
    g_interrupted = true;
}

// 解析 Universe 列表
QList<quint16> parseUniverseList(const QString &str) {
    QList<quint16> result;
    QStringList parts = str.split(',', Qt::SkipEmptyParts);
    for (const auto &part : parts) {
        QString trimmed = part.trimmed();
        bool ok = false;
        quint16 u = quint16(trimmed.toInt(&ok, 16)); // 默认16进制 (ArtNet编码格式)
        if (!ok) {
            u = quint16(trimmed.toInt(&ok, 10));
        }
        if (ok) {
            result.append(u);
        }
    }
    return result;
}

// 解析帧率
int parseFrameRate(const QString &str) {
    if (str == "24" || str == "24fps") return 24;
    if (str == "25" || str == "25fps") return 25;
    if (str == "29.97" || str == "29.97fps" || str == "29") return 29; // 29.97
    if (str == "30" || str == "30fps") return 30;
    if (str == "60" || str == "60fps") return 60;
    return str.toInt();
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("artnet_recorder");
    QCoreApplication::setApplicationVersion("1.2.0");
    
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // ==================== 命令行解析 ====================
    
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "ArtNet/DMX512 录制器 - 基于时间码同步的专业灯光录制工具\n\n"
        "支持的同步模式:\n"
        "  internal   内部系统时钟 (默认)\n"
        "  video      视频文件时间戳 (FFmpeg解码)\n"
        "  audio      音频文件 (LTC, 未来扩展)"
    );
    parser.addHelpOption();
    parser.addVersionOption();
    
    // 必需选项
    QCommandLineOption outputOption(QStringList() << "o" << "output",
        "输出录制文件路径 (.artrec)", "output_file");
    
    QCommandLineOption modeOption(QStringList() << "m" << "mode",
        "运行模式: record (录制), verify (验证文件)", "mode", "record");
    
    // 录制选项
    QCommandLineOption syncOption(QStringList() << "s" << "sync",
        "同步源: internal, video, audio (默认: internal)", "sync_source", "internal");
    
    QCommandLineOption mediaOption(QStringList() << "media",
        "媒体文件路径 (video/audio模式需要)", "media_file");
    
    QCommandLineOption universeOption(QStringList() << "u" << "universe",
        "要录制的 Universe 列表, 逗号分隔 (如 \"0,1,2,3\" 或十六进制 \"0x0,0x1,0x2\")",
        "universe_list", "0,1,2,3");
    
    QCommandLineOption fpsOption(QStringList() << "f" << "fps",
        "目标帧率: 24, 25, 29.97, 30, 60 (默认: 30)", "fps", "30");
    
    QCommandLineOption durationOption(QStringList() << "d" << "duration",
        "最大录制时长 (秒), 0=不限 (默认: 0)", "seconds", "0");
    
    QCommandLineOption portOption(QStringList() << "p" << "port",
        "ArtNet 监听端口 (默认: 6454)", "port", "6454");
    
    QCommandLineOption verboseOption(QStringList() << "v" << "verbose",
        "详细输出模式");
    
    QCommandLineOption previewOption(QStringList() << "no-preview",
        "禁用实时预览输出 (仅在 verbose 模式下)");
    
    // === 边录边转发选项 ===
    
    QCommandLineOption artnetForwardOption(QStringList() << "artnet-forward",
        "ArtNet 转发目标 IP[:端口] (可多次指定, 端口默认 6454)", 
        "ip[:port]");
    
    QCommandLineOption artnetForwardUniv(QStringList() << "artnet-forward-univ",
        "ArtNet 转发的 Universe 过滤 (空=全部, 默认: 全部)",
        "universe_list", "");
    
    QCommandLineOption dmxPortOption(QStringList() << "dmx-port",
        "DMX512 串口输出端口 (如 COM3, /dev/ttyUSB0)",
        "port_name");
    
    QCommandLineOption dmxBaudOption(QStringList() << "dmx-baud",
        "DMX512 串口波特率 (默认: 250000)", "baud_rate", "250000");
    
    QCommandLineOption dmxUnivOption(QStringList() << "dmx-universe",
        "DMX512 输出的 Universe (空=取第一个录制的, 默认: 空)",
        "universe", "");
    
    QCommandLineOption npuForwardOption(QStringList() << "npu-forward",
        "MA NPU (Ma-Net2) 转发目标 IP[:端口], 端口默认 9999 (可多次指定)",
        "ip[:port]");
    
    QCommandLineOption npuForwardUniv(QStringList() << "npu-forward-univ",
        "NPU 转发的 Universe 过滤 (空=全部, 默认: 全部)",
        "universe_list", "");
    
    // 添加选项
    parser.addOption(outputOption);
    parser.addOption(modeOption);
    parser.addOption(syncOption);
    parser.addOption(mediaOption);
    parser.addOption(universeOption);
    parser.addOption(fpsOption);
    parser.addOption(durationOption);
    parser.addOption(portOption);
    parser.addOption(verboseOption);
    parser.addOption(previewOption);
    parser.addOption(artnetForwardOption);
    parser.addOption(artnetForwardUniv);
    parser.addOption(dmxPortOption);
    parser.addOption(dmxBaudOption);
    parser.addOption(dmxUnivOption);
    parser.addOption(npuForwardOption);
    parser.addOption(npuForwardUniv);
    
    parser.process(app);
    
    // 设置日志级别
    if (parser.isSet(verboseOption)) {
        Logger::instance().setLevel(LogLevel::Debug);
    } else {
        Logger::instance().setLevel(LogLevel::Info);
    }
    
    // ==================== 模式分发 ====================
    
    QString mode = parser.value(modeOption);
    QString outputFile = parser.value(outputOption);
    
    if (mode == "verify") {
        // 验证/检查录制文件
        if (outputFile.isEmpty()) {
            LOG_ERROR("验证模式需要指定 -o <file.artrec>");
            return 1;
        }
        
        ArtRecReader reader;
        if (!reader.open(outputFile)) {
            return 1;
        }
        
        const ArtRecHeader &h = reader.header();
        LOG_INFO(QString("=== 录制文件信息 ==="));
        LOG_INFO(QString("文件: %1").arg(outputFile));
        LOG_INFO(QString("版本: %1").arg(h.version, 0, 16));
        LOG_INFO(QString("帧率: %1").arg(h.frameRate));
        LOG_INFO(QString("总帧: %1").arg(h.totalFrames));
        LOG_INFO(QString("时长: %1 ms").arg(h.totalDurationUs / 1000));
        LOG_INFO(QString("Universe 数: %1").arg(h.universeCount));
        LOG_INFO(QString("创建时间: %1").arg(
            QDateTime::fromMSecsSinceEpoch(h.createTimestamp).toString()));
        
        QString mediaFile = reader.mediaFilePath();
        if (!mediaFile.isEmpty()) {
            LOG_INFO(QString("关联媒体: %1").arg(mediaFile));
        }
        
        // 读取前几帧示例
        LOG_INFO(QString("\n=== 帧数据示例 (前5帧) ==="));
        qint64 frameIndex, timestampUs;
        QMap<int, QByteArray> data;
        int count = 0;
        while (count < 5 && reader.readNextFrame(frameIndex, timestampUs, data)) {
            LOG_INFO(QString("Frame %1: ts=%2 us, universes=%3")
                .arg(frameIndex)
                .arg(timestampUs)
                .arg(data.size()));
            count++;
        }
        
        return 0;
    }
    
    // ==================== 录制模式 ====================
    
    if (outputFile.isEmpty()) {
        LOG_ERROR("请指定输出文件: -o <output.artrec>");
        parser.showHelp(1);
        return 1;
    }
    
    // 解析选项
    QString syncStr = parser.value(syncOption).toLower();
    RecorderEngine::SyncSource syncSource;
    
    if (syncStr == "video") {
        syncSource = RecorderEngine::SyncSource::VideoFile;
    } else if (syncStr == "audio") {
        syncSource = RecorderEngine::SyncSource::AudioFile;
    } else {
        syncSource = RecorderEngine::SyncSource::InternalClock;
    }
    
    if ((syncSource == RecorderEngine::SyncSource::VideoFile || 
         syncSource == RecorderEngine::SyncSource::AudioFile) && 
        !parser.isSet(mediaOption)) {
        LOG_ERROR(QString("%1 同步模式需要指定 --media <file>")
            .arg(syncStr));
        return 1;
    }
    
    QString mediaFile = parser.value(mediaOption);
    QList<quint16> universes = parseUniverseList(parser.value(universeOption));
    int fps = parseFrameRate(parser.value(fpsOption));
    qint64 durationSec = parser.value(durationOption).toLongLong();
    quint16 port = quint16(parser.value(portOption).toInt());
    bool verbose = parser.isSet(verboseOption);
    
    if (universes.isEmpty()) {
        LOG_ERROR("没有有效的 Universe");
        return 1;
    }
    
    // 构造配置
    RecorderEngine::Config config;
    config.outputFilePath = outputFile;
    config.syncSource = syncSource;
    config.mediaFilePath = mediaFile;
    config.universes = universes;
    config.frameRate = fps;
    config.maxDurationUs = durationSec * 1000000;
    config.artNetPort = port;
    config.verbose = verbose;
    
    // === 解析输出/转发目标 ===
    
    QList<OutputTargetConfig> outputTargets;
    bool hasForward = false;
    
    // ArtNet 转发目标 (可多次指定)
    QStringList artnetForwardList = parser.values(artnetForwardOption);
    if (!artnetForwardList.isEmpty()) {
        QList<quint16> forwardFilterUniv;
        if (parser.isSet(artnetForwardUniv)) {
            forwardFilterUniv = parseUniverseList(parser.value(artnetForwardUniv));
        }
        
        for (const auto &targetStr : artnetForwardList) {
            // 解析 "ip:port" 或 "ip"
            QString ip = targetStr;
            quint16 fwdPort = ArtNet::DEFAULT_PORT;
            int colonIdx = targetStr.lastIndexOf(':');
            if (colonIdx > 0) {
                ip = targetStr.left(colonIdx);
                fwdPort = quint16(targetStr.mid(colonIdx + 1).toInt());
            }
            
            OutputTargetConfig tgt;
            tgt.type = OutputType::ArtNet;
            tgt.artNetIp = ip;
            tgt.artNetPort = fwdPort;
            tgt.filterUniverses = forwardFilterUniv;
            tgt.enabled = true;
            
            outputTargets.append(tgt);
            hasForward = true;
            
            LOG_INFO(QString("→ ArtNet 转发目标: %1:%2")
                .arg(ip).arg(fwdPort));
        }
    }
    
    // DMX512 串口输出
    if (parser.isSet(dmxPortOption)) {
        OutputTargetConfig tgt;
        tgt.type = OutputType::DMX512;
        tgt.dmxPortName = parser.value(dmxPortOption);
        tgt.dmxBaudRate = parser.value(dmxBaudOption).toInt();
        
        if (parser.isSet(dmxUnivOption)) {
            tgt.filterUniverses = parseUniverseList(parser.value(dmxUnivOption));
        } else {
            // 默认用录制的第一个 Universe
            if (!universes.isEmpty()) {
                tgt.filterUniverses.append(universes.first());
            }
        }
        
        tgt.enabled = true;
        outputTargets.append(tgt);
        hasForward = true;
        
        LOG_INFO(QString("→ DMX512 串口: %1 @ %2 baud, Universe: %3")
            .arg(tgt.dmxPortName)
            .arg(tgt.dmxBaudRate)
            .arg([&]{
                QStringList uList;
                for (auto u : tgt.filterUniverses)
                    uList.append(QString("0x%1").arg(u, 0, 16));
                return uList.isEmpty() ? "默认第一个" : uList.join(",");
            }()));
    }
    
    config.enablePassthrough = hasForward;
    config.outputTargets = outputTargets;
    
    // NPU (Ma-Net2) 转发目标 (可多次指定)
    QStringList npuForwardList = parser.values(npuForwardOption);
    if (!npuForwardList.isEmpty()) {
        QList<quint16> npuFilterUniv;
        if (parser.isSet(npuForwardUniv)) {
            npuFilterUniv = parseUniverseList(parser.value(npuForwardUniv));
        }
        
        for (const auto &targetStr : npuForwardList) {
            QString ip = targetStr;
            quint16 fwdPort = NpuProtocol::DEFAULT_PORT;
            int colonIdx = targetStr.lastIndexOf(':');
            if (colonIdx > 0) {
                ip = targetStr.left(colonIdx);
                fwdPort = quint16(targetStr.mid(colonIdx + 1).toInt());
            }
            
            OutputTargetConfig tgt;
            tgt.type = OutputType::Npu;
            tgt.artNetIp = ip;      // NPU 也用 remoteAddress 字段
            tgt.artNetPort = fwdPort;
            tgt.filterUniverses = npuFilterUniv;
            tgt.enabled = true;
            
            outputTargets.append(tgt);
            hasForward = true;
            config.enablePassthrough = true;
            
            LOG_INFO(QString("→ NPU 转发目标: %1:%2")
                .arg(ip).arg(fwdPort));
        }
        config.outputTargets = outputTargets;
    }
    
    // 创建录制引擎
    RecorderEngine engine;
    
    // 连接信号
    QObject::connect(&engine, &RecorderEngine::stateChanged,
        [](RecordState state) {
            switch (state) {
                case RecordState::Recording:
                    LOG_INFO("▶ 录制中... (按 Ctrl+C 停止)");
                    break;
                case RecordState::Stopped:
                    LOG_INFO("■ 录制已停止");
                    break;
                default:
                    break;
            }
        });
    
    QObject::connect(&engine, &RecorderEngine::recordingFinished,
        [](qint64 frames, qint64 durationUs) {
            LOG_INFO(QString("✅ 录制完成: %1 帧, 时长 %2 ms")
                .arg(frames)
                .arg(durationUs / 1000));
        });
    
    QObject::connect(&engine, &RecorderEngine::errorOccurred,
        [](const QString &err) {
            LOG_ERROR(QString("❌ 错误: %1").arg(err));
        });
    
    // 启动录制
    if (!engine.start(config)) {
        return 1;
    }
    
    // 使用定时器检查中断标志
    QTimer interruptTimer;
    interruptTimer.setInterval(100);
    QObject::connect(&interruptTimer, &QTimer::timeout, [&]() {
        if (g_interrupted) {
            LOG_INFO("收到中断信号, 正在停止录制...");
            engine.stop();
            QCoreApplication::quit();
        }
        
        if (engine.state() == RecordState::Stopped) {
            QCoreApplication::quit();
        }
    });
    interruptTimer.start();
    
    // 打印启动提示
    LOG_INFO("========================================");
    LOG_INFO(QString("🎬 ArtNet 录制器 v1.2 (带转发 + Protocol 抽象层)"));
    LOG_INFO(QString("   模式: %1").arg(
        syncSource == RecorderEngine::SyncSource::InternalClock ? "内部时钟" :
        syncSource == RecorderEngine::SyncSource::VideoFile ? "视频同步" : "音频同步"));
    LOG_INFO(QString("   帧率: %1 fps").arg(fps));
    LOG_INFO(QString("   Universe: %1 个").arg(universes.size()));
    LOG_INFO(QString("   输出: %1").arg(outputFile));
    LOG_INFO(QString("   ArtNet 监听: 端口 %1").arg(port));
    
    if (hasForward) {
        int npuCount = 0;
        for (const auto &t : outputTargets) {
            if (t.type == OutputType::Npu) npuCount++;
        }
        LOG_INFO(QString("   转发目标: %1 个 (ArtNet: %2, NPU: %3, DMX512: %4)")
            .arg(outputTargets.size())
            .arg(outputTargets.size() - npuCount - (parser.isSet(dmxPortOption) ? 1 : 0))
            .arg(npuCount)
            .arg(parser.isSet(dmxPortOption) ? 1 : 0));
    } else {
        LOG_INFO(QString("   转发: 未启用 (--artnet-forward / --npu-forward / --dmx-port 开启)"));
    }
    LOG_INFO("========================================");
    
    return app.exec();
}
