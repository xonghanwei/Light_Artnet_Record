// RecorderEngine.cpp — 录制引擎核心 (v1.3: 接入 ISyncClock 统一时间基准)
#include "RecorderEngine.h"
#include "../core/Logger.h"
#include "../sync/clocks/MidiMTCClock.h"
#include <QDateTime>
#include <QtMath>
#include <cmath>

RecorderEngine::RecorderEngine(QObject *parent)
    : QObject(parent) {
}

RecorderEngine::~RecorderEngine() {
    stop();
}

// ============================================================
// Config → SyncClockConfig 转换
// ============================================================

SyncClockConfig RecorderEngine::buildSyncClockConfig() const {
    SyncClockConfig cfg;
    cfg.clockType = syncSourceToClockType(m_config.syncSource);
    cfg.frameRate = m_config.frameRate;
    cfg.mediaFilePath = m_config.mediaFilePath;

    switch (m_config.syncSource) {
        case SyncSource::InternalClock:
            // 无额外参数
            break;
        case SyncSource::ArtNetTimeCode:
            cfg.artNetPort = m_config.artNetPort;
            cfg.artNetBindAddress = "0.0.0.0";
            break;
        case SyncSource::TimecodeFile:
            cfg.timecodeFileHasRateHeader = true;
            break;
        case SyncSource::MidiMTC:
            cfg.midiInputChannel = 0;
            break;
        case SyncSource::VideoFile:
        case SyncSource::AudioFile:
            // mediaFilePath 已设置
            break;
    }
    return cfg;
}

// ============================================================
// start — 接入 SyncClockFactory
// ============================================================

bool RecorderEngine::start(const Config &config) {
    if (m_state == RecordState::Recording || m_state == RecordState::Paused) {
        LOG_ERROR("已经在录制中");
        return false;
    }
    
    m_config = config;
    
    LOG_INFO(QString("启动录制器:"));
    LOG_INFO(QString("  输出文件: %1").arg(config.outputFilePath));
    LOG_INFO(QString("  同步源: %1").arg(syncSourceToString(config.syncSource)));
    if (!config.mediaFilePath.isEmpty()) {
        LOG_INFO(QString("  媒体文件: %1").arg(config.mediaFilePath));
    }
    LOG_INFO(QString("  目标帧: %1 fps").arg(config.frameRate));
    LOG_INFO(QString("  Universe: %1").arg([&]{
        QStringList list;
        for (auto u : config.universes) {
            list.append(QString("0x%1").arg(u, 0, 16));
        }
        return list.join(", ");
    }()));
    
    // 1. 创建 ISyncClock — 统一时间基准
    SyncClockConfig syncCfg = buildSyncClockConfig();
    m_syncClock = SyncClockFactory::instance().create(syncCfg.clockType);

    if (!m_syncClock) {
        // 工厂返回 nullptr — 可能是 VideoFile/AudioFile 没 FFmpeg 支持
        LOG_ERROR(QString("SyncClockFactory 无法创建时钟类型: %1")
            .arg(syncClockTypeToString(syncCfg.clockType)));
        emit errorOccurred(QString("无法创建同步时钟: %1")
            .arg(syncSourceToString(config.syncSource)));
        return false;
    }

    // 如果 SyncClock 支持外部提供的媒体路径 (VideoFile/AudioFile/TimecodeFile),
    // 且 Config.mediaFilePath 有值, 用它覆盖
    if (!config.mediaFilePath.isEmpty()) {
        syncCfg.mediaFilePath = config.mediaFilePath;
    }

    // 2. 启动时钟
    if (!m_syncClock->start(syncCfg)) {
        LOG_ERROR("同步时钟启动失败");
        emit errorOccurred(QString("同步时钟启动失败: %1")
            .arg(syncSourceToString(config.syncSource)));
        delete m_syncClock;
        m_syncClock = nullptr;
        return false;
    }

    // 连接时钟信号 → 驱动录制
    connect(m_syncClock, &ISyncClock::frameTick,
            this, &RecorderEngine::onSyncFrameTick);
    connect(m_syncClock, &ISyncClock::clockFinished,
            this, &RecorderEngine::onSyncClockFinished);
    connect(m_syncClock, &ISyncClock::errorOccurred,
            this, [this](const QString &err) {
                LOG_ERROR(QString("SyncClock error: %1").arg(err));
                emit errorOccurred(err);
            });

    LOG_INFO(QString("同步时钟已启动: %1 @ %2 fps")
        .arg(m_syncClock->clockName()).arg(m_syncClock->frameRate()));

    // 3. 启动 ArtNet 接收器
    // 注意: ArtNetTimeCodeClock 也会 bind artNetPort (收 opcode 0x0D),
    // 如果它已经占用了这个端口, ArtNetReceiver 改用 artNetPort + 1
    // (DMX 实际仍发到 artNetPort, 这在 ArtNet 协议层处理; 测试场景里两边 bind 同一端口会失败)
    m_receiver = new ArtNetReceiver(this);
    connect(m_receiver, &ArtNetReceiver::dmxFrameReceived,
            this, &RecorderEngine::onDmxFrameReceived);
    
    m_receiver->setFilterUniverses(config.universes);
    
    quint16 receiverPort = config.artNetPort;
    if (config.syncSource == SyncSource::ArtNetTimeCode) {
        // ArtNetTimeCodeClock 先占用了 artNetPort, 让 Receiver 用 artNetPort + 1
        receiverPort = config.artNetPort + 1;
    }
    
    if (!m_receiver->start(receiverPort)) {
        QString errDetail = m_receiver->lastError();
        LOG_ERROR(QString("ArtNet 接收器启动失败: %1").arg(errDetail));
        emit errorOccurred(errDetail);
        cleanup();
        return false;
    }
    
    // 4. 初始化 ArtRecWriter
    m_writer = new ArtRecWriter();
    
    ArtRecHeader header;
    header.frameRate = quint16(m_config.frameRate);
    header.timeCodeType = quint8([&]{
        // SyncClockType → timeCodeType
        switch(m_syncClock->clockType()) {
            case SyncClockType::InternalTimer:  return 0;
            case SyncClockType::ArtNetTimeCode: return 1;
            case SyncClockType::VideoFile:      return 2;
            case SyncClockType::AudioFile:      return 3;
            case SyncClockType::MidiMTC:        return 4;
            case SyncClockType::TimecodeFile:   return 5;
            default: return 0;
        }
    }());
    header.timeCodeFormat = quint8([&]{
        int fps = m_config.frameRate;
        if (fps == 24) return 0;
        if (fps == 25) return 1;
        if (fps == 29) return 2;
        if (fps == 30) return 3;
        return 3;
    }());
    header.createTimestamp = QDateTime::currentMSecsSinceEpoch();
    
    QList<UniverseEntry> entries = buildUniverseEntries();
    
    if (!m_writer->create(config.outputFilePath, header, entries)) {
        LOG_ERROR("录制文件创建失败");
        emit errorOccurred("录制文件创建失败");
        cleanup();
        return false;
    }
    
    // 5. 初始化 OutputManager (如果配置了)
    if (config.enablePassthrough && !config.outputTargets.isEmpty()) {
        m_outputManager = new OutputManager(this);
        for (const auto &targetCfg : config.outputTargets) {
            m_outputManager->addTarget(targetCfg);
        }
        if (!m_outputManager->start()) {
            LOG_WARN("OutputManager 启动失败 (不影响录制, 但转发不可用)");
            delete m_outputManager;
            m_outputManager = nullptr;
        } else {
            LOG_INFO(QString("OutputManager 已启动, %1 个目标活跃")
                .arg(m_outputManager->enabledTargetCount()));
        }
    }
    
    // 6. 重置状态
    m_recordedFrames = 0;
    m_lastFrameIndex = -1;
    m_universeCache.clear();
    m_prevUniverseSnapshot.clear();
    
    // 7. 预览定时器 — 现在只做状态输出和 shouldStop 检查
    m_previewTimer = new QTimer(this);
    m_previewTimer->setInterval(config.previewIntervalMs);
    connect(m_previewTimer, &QTimer::timeout,
            this, &RecorderEngine::onPreviewTick);
    m_previewTimer->start();
    
    // 8. 进入录制状态
    setState(RecordState::Recording);
    
    LOG_INFO(QString("录制已开始 (时钟=%1, 起始时间: %2)")
        .arg(m_syncClock->clockName())
        .arg(QDateTime::currentDateTime().toString("hh:mm:ss.zzz")));
    
    return true;
}

// ============================================================
// stop / cleanup
// ============================================================

void RecorderEngine::stop() {
    if (m_state == RecordState::Idle || m_state == RecordState::Stopped) return;
    
    qint64 totalDurationUs = currentDurationUs();
    qint64 totalFrames = m_recordedFrames;
    
    if (m_writer && m_writer->isOpen()) {
        QString mediaPath = m_config.mediaFilePath;
        m_writer->finalize(totalFrames, totalDurationUs, mediaPath);
    }
    
    LOG_INFO(QString("录制已停止: %1 帧, %2 ms (时钟=%3)")
        .arg(totalFrames)
        .arg(totalDurationUs / 1000)
        .arg(m_syncClock ? m_syncClock->clockName() : "null"));
    
    cleanup();
    
    setState(RecordState::Stopped);
    emit recordingFinished(totalFrames, totalDurationUs);
}

void RecorderEngine::cleanup() {
    if (m_syncClock) {
        m_syncClock->stop();
        delete m_syncClock;
        m_syncClock = nullptr;
    }
    
    if (m_outputManager) {
        m_outputManager->stop();
        delete m_outputManager;
        m_outputManager = nullptr;
    }
    
    if (m_previewTimer) {
        m_previewTimer->stop();
        delete m_previewTimer;
        m_previewTimer = nullptr;
    }
    
    if (m_receiver) {
        m_receiver->stop();
        delete m_receiver;
        m_receiver = nullptr;
    }
    
    if (m_writer) {
        m_writer->close();
        delete m_writer;
        m_writer = nullptr;
    }
    
    if (m_decoder) {
        m_decoder->close();
        delete m_decoder;
        m_decoder = nullptr;
    }
    
    m_universeCache.clear();
    m_prevUniverseSnapshot.clear();
    m_state = RecordState::Idle;
}

// ============================================================
// pause / resume — 控制录制状态, 时钟本身不暂停 (ArtNet/MTC 被动源)
// ============================================================

void RecorderEngine::pause() {
    if (m_state != RecordState::Recording) return;
    setState(RecordState::Paused);
    LOG_INFO("录制已暂停 (时钟继续跑, 但暂停写帧)");
}

void RecorderEngine::resume() {
    if (m_state != RecordState::Paused) return;
    setState(RecordState::Recording);
    LOG_INFO("录制已恢复");
}

void RecorderEngine::injectMidiMtcQuarterFrame(quint8 nibble) {
    if (!m_syncClock) return;
    auto *mtc = dynamic_cast<MidiMTCClock*>(m_syncClock);
    if (mtc) {
        mtc->onMtcQuarterFrame(nibble);
    }
}

void RecorderEngine::setState(RecordState state) {
    m_state = state;
    emit stateChanged(state);
}

// ============================================================
// 时间查询 — 全部委托给 ISyncClock
// ============================================================

qint64 RecorderEngine::currentDurationUs() const {
    if (!m_syncClock) return 0;
    qint64 ts = m_syncClock->currentTimestampUs();
    return qMax(0LL, ts);
}

TimeCode RecorderEngine::currentTimeCode() const {
    if (!m_syncClock) return TimeCode();
    return m_syncClock->currentTimeCode();
}

qint64 RecorderEngine::getCurrentTimestampUs() {
    if (!m_syncClock) return -1;
    return m_syncClock->currentTimestampUs();
}

// ============================================================
// ISyncClock frameTick → 写帧
// ============================================================

void RecorderEngine::onSyncFrameTick(qint64 frameIndex, qint64 timestampUs) {
    if (m_state != RecordState::Recording) return;
    if (!m_writer || !m_writer->isOpen()) return;

    // 时间戳异常 — 跳过 (某些时钟可能 emit ts=-1)
    if (timestampUs < 0) return;

    // 帧回退 — 跳过 (时钟循环时 frameIndex 会回 0)
    if (frameIndex <= m_lastFrameIndex) return;

    writeCurrentFrame(frameIndex, timestampUs);
    m_lastFrameIndex = frameIndex;
}

void RecorderEngine::writeCurrentFrame(qint64 frameIndex, qint64 timestampUs) {
    QMutexLocker locker(&m_cacheMutex);

    bool dataChanged = false;
    QMap<quint16, QByteArray> frameData;

    for (auto universe : m_config.universes) {
        auto it = m_universeCache.find(universe);
        if (it != m_universeCache.end()) {
            frameData[universe] = it.value();
            if (dmxDataChanged(universe)) dataChanged = true;
        }
    }

    m_writer->writeFrame(frameIndex, timestampUs, frameData, dataChanged);

    // 边录边转发
    if (m_outputManager && !frameData.isEmpty()) {
        m_outputManager->sendMultipleUniverses(frameData);
    }

    m_prevUniverseSnapshot = m_universeCache;

    m_recordedFrames++;

    emit frameRecorded(frameIndex, timestampUs);
    m_lastTimeCode = m_syncClock ? m_syncClock->currentTimeCode() : TimeCode();
}

void RecorderEngine::onSyncClockFinished() {
    LOG_INFO("同步时钟已到达末尾 (clockFinished)");
    stop();
}

// ============================================================
// DMX 数据接收 (不变)
// ============================================================

void RecorderEngine::onDmxFrameReceived(const ArtNetDmxFrame &frame) {
    QMutexLocker locker(&m_cacheMutex);
    m_universeCache[frame.universe] = QByteArray(
        reinterpret_cast<const char*>(frame.dmxData),
        frame.length);
}

// ============================================================
// 预览定时器 — 现在只做状态输出 + shouldStop
// ============================================================

void RecorderEngine::onPreviewTick() {
    if (m_state != RecordState::Recording) return;

    // 检查停止条件
    if (shouldStop()) {
        stop();
        return;
    }

    // 定期输出状态
    qint64 durMs = currentDurationUs() / 1000;
    qint64 h = durMs / 3600000;
    qint64 m = (durMs % 3600000) / 60000;
    qint64 s = (durMs % 60000) / 1000;

    QString status = QString("REC %1 帧 | %2:%3:%4 | 时钟=%5")
        .arg(m_recordedFrames)
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'))
        .arg(m_syncClock ? m_syncClock->clockName() : "null");

    emit statusUpdate(status);

    if (m_config.verbose) {
        LOG_INFO(status);
    }
}

// ============================================================
// 辅助方法 (不变)
// ============================================================

qint64 RecorderEngine::frameIndexToUs(qint64 frameIndex) const {
    if (m_config.frameRate <= 0) return 0;
    double d = frameIndex / double(m_config.frameRate);
    return qint64(d * 1000000.0 + 0.5);
}

bool RecorderEngine::dmxDataChanged(quint16 universe) const {
    auto currentIt = m_universeCache.find(universe);
    auto prevIt = m_prevUniverseSnapshot.find(universe);
    
    if (currentIt == m_universeCache.end()) return false;
    if (prevIt == m_prevUniverseSnapshot.end()) return true;
    
    const QByteArray &cur = currentIt.value();
    const QByteArray &prev = prevIt.value();
    
    int cmpLen = qMin(cur.size(), prev.size());
    if (cur.size() != prev.size()) return true;
    
    return std::memcmp(cur.constData(), prev.constData(), cmpLen) != 0;
}

bool RecorderEngine::shouldStop() {
    if (m_config.maxDurationUs > 0) {
        if (currentDurationUs() >= m_config.maxDurationUs) {
            LOG_INFO("达到最大录制时长");
            return true;
        }
    }
    return false;
}

QList<UniverseEntry> RecorderEngine::buildUniverseEntries() const {
    QList<UniverseEntry> entries;
    for (auto u : m_config.universes) {
        entries.append(UniverseEntry(u, ArtNet::DMX_CHANNEL_COUNT));
    }
    return entries;
}
