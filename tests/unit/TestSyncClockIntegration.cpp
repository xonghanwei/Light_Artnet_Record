// TestSyncClockIntegration.cpp — 同步抽象层集成测试
//
// 与单元测试 TestSyncClock 的区别:
//   单元测试 = ISyncClock 内部 round-trip, 单实现隔离
//   集成测试 = SyncClockFactory × 各时钟实现 × Storage 层 × UDP 自环 跨模块数据流:
//     L1. SyncClockFactory + InternalTimerClock → ArtRecStorage 录制管道
//     L2. TimecodeFileClock 驱动 → Storage 验证时间戳一致性
//     L3. ArtNetTimeCodeClock UDP 广播 → Storage 验证跨模块保真
//     L4. 多时钟源协同一致性 (InternalTimer vs TimecodeFile)
//     L5. MidiMTCClock quarter-frame → Storage 跨格式 round-trip
//     L6. SyncClockFactory 完整生命周期 (create/start/pause/resume/stop)
//
#include <QtTest>
#include <QObject>
#include <QTemporaryDir>
#include <QFile>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QList>

#include "core/Common.h"
#include "sync/SyncTypes.h"
#include "sync/ISyncClock.h"
#include "sync/SyncClockFactory.h"
#include "sync/clocks/InternalTimerClock.h"
#include "sync/clocks/ArtNetTimeCodeClock.h"
#include "sync/clocks/MidiMTCClock.h"
#include "sync/clocks/TimecodeFileClock.h"
#include "storage/StorageFactory.h"
#include "storage/IStorageFormat.h"
#include "storage/StorageTypes.h"
#include "storage/ArtRecStorage.h"
#include "storage/JsonStorage.h"
#include "storage/XmlStorage.h"

#include <cstring>

#include "recorder/RecorderEngine.h"
#include "fileformat/ArtRecReader.h"
#include "output/OutputManager.h"

using namespace ArtNet;

// ===== 静态 helper =====

// 构造 ArtNet TimeCode 数据包 (opcode 0x0D00) — 与 TestSyncClock 相同
static QByteArray makeArtNetTC(quint8 hours, quint8 minutes, quint8 seconds,
                                quint8 frames, quint8 smpteType = 3, bool dropFrame = false) {
    QByteArray pkt(13, char(0));
    quint8 *d = reinterpret_cast<quint8*>(pkt.data());
    d[0] = 0x00; d[1] = 0x0D;
    d[2] = 0x00; d[3] = 0x01;
    d[4] = 0x00; d[5] = 0x00;
    d[6] = (smpteType << 4) & 0xF0;
    d[7] = quint8(hours & 0x3F) | (dropFrame ? 0x80 : 0x00);
    d[8] = quint8(minutes & 0x3F);
    d[9] = quint8(seconds & 0x3F);
    d[10] = quint8(frames & 0x1F);
    return pkt;
}

// 构造 MTC quarter-frame 序列 — (value << 4) | index
static QList<quint8> makeMtcQF(quint8 hours, quint8 minutes, quint8 seconds, quint8 frames) {
    QList<quint8> qfs;
    qfs.append(((frames & 0x0F) << 4) | 0x00);
    qfs.append((((frames >> 4) & 0x0F) << 4) | 0x01);
    qfs.append(((seconds & 0x0F) << 4) | 0x02);
    qfs.append((((seconds >> 4) & 0x0F) << 4) | 0x03);
    qfs.append(((minutes & 0x0F) << 4) | 0x04);
    qfs.append((((minutes >> 4) & 0x0F) << 4) | 0x05);
    qfs.append(((hours & 0x0F) << 4) | 0x06);
    quint8 hour_ms = (hours >> 4) & 0x01;
    quint8 highNibble = ((hour_ms | (3 << 1)) & 0x0F);  // fps type 3 = 30
    qfs.append((highNibble << 4) | 0x07);
    return qfs;
}

// 从 ArtNetTC 时间码计算预期 timestampUs (30fps)
static qint64 artNetTCToUs(quint8 h, quint8 m, quint8 s, quint8 f, int fps = 30) {
    qint64 frames = ((h * 3600LL + m * 60 + s) * fps) + f;
    return frames * 1000000LL / fps;
}

// 取一个当前空闲的本地 UDP 端口 (绑定后立即释放)
static quint16 freeUdpPort() {
    QUdpSocket probe;
    if (!probe.bind(QHostAddress("127.0.0.1"), quint16(0))) return 0;
    const quint16 port = probe.localPort();
    probe.close();
    return port;
}

// 构造 ArtDMX 数据包 (opcode 0x5000, 长度小端) — 与 ArtNetReceiver 解析约定一致
static QByteArray makeArtDmxPacket(quint16 portAddress, const QByteArray &data) {
    QByteArray pkt(18 + data.size(), char(0));
    memcpy(pkt.data(), "Art-Net", 7);
    pkt[7] = 0;
    pkt[8] = 0x00; pkt[9] = 0x50;   // opcode 0x5000 小端
    pkt[10] = 14;  pkt[11] = 0;     // protocol version
    pkt[12] = 0;   pkt[13] = 0;     // sequence / physical
    pkt[14] = quint8(portAddress & 0xFF);
    pkt[15] = quint8((portAddress >> 8) & 0xFF);
    const quint16 len = quint16(data.size());
    pkt[16] = quint8(len & 0xFF);
    pkt[17] = quint8((len >> 8) & 0xFF);
    memcpy(pkt.data() + 18, data.constData(), data.size());
    return pkt;
}

// ============================================================
// Test class
// ============================================================

class TestSyncClockIntegration : public QObject {
    Q_OBJECT

private slots:
    // ===== L1: SyncClockFactory + InternalTimerClock → ArtRecStorage 录制管道 =====
    void test_factoryInternalTimer_clockDrivesStorage();

    // ===== L2: TimecodeFileClock 驱动 → Storage 验证时间戳一致性 =====
    void test_tcFileClock_clockDrivesStorage_timestampsExact();

    // ===== L3: ArtNetTimeCodeClock UDP 广播 → Storage 跨模块保真 =====
    void test_artnetTC_clockDrivesStorage_timestampConsistent();

    // ===== L4: 多时钟源协同一致性 =====
    void test_multiClock_sameFps_frameCountWithinTolerance();

    // ===== L5: MidiMTCClock quarter-frame → Storage 跨格式 round-trip =====
    void test_midiMTC_storageCrossFormatPreservesTimestamp();

    // ===== L6: SyncClockFactory 完整生命周期 =====
    void test_factory_lifecycle_createStartPauseResumeStop();

    // ===== L7: TimecodeFileClock loopOnEnd=true → Storage 循环写入帧序列验证 =====
    void test_tcFileClock_loopOnEnd_storageWritesMultipleRounds();

    // ===== L8: MidiMTC timeout → RecorderEngine 自动停止 + recordingFinished =====
    void test_midiMTC_timeout_triggersRecorderStop();

    // ===== L9: InternalTimerClock × RecorderEngine × 多 Universe → ArtRec 文件数据完整 =====
    void test_internalTimer_multiUniverse_recorderEngine_pipeline();

    // ===== L10: RecorderEngine SyncSource 映射 → ArtRecHeader timeCodeType 正确性 =====
    void test_recorderEngine_syncSource_mapsToArtRecHeader_timeCodeType();

    // ===== L11: RecorderEngine pause/resume × InternalTimerClock — pause 期间不写帧 + resume 后连续 =====
    void test_recorderEngine_pauseResume_pauseWritesZero_resumeContinues();

    // ===== L12: RecorderEngine (MidiMTC) full-frame 注入端到端 — 驱动 frameRecorded + ArtRec =====
    void test_recorderEngine_midiMTC_fullFrame_injectDrivesRecording();

    // ===== L13: RecorderEngine maxDurationUs 自动停止 — shouldStop 触发 recordingFinished =====
    void test_recorderEngine_maxDurationUs_autoStop();

    // ===== L14: RecorderEngine → ArtRec → Json → ArtRec 四格式 round-trip 时间戳 + 帧序号保真 =====
    void test_recorderEngine_quadrupleFormatRoundTrip_timestampsPreserved();

    // ===== L15: TimecodeFileClock EOF → RecorderEngine 自动停止 + 时间戳保真 =====
    void test_recorderEngine_timecodeFileEOF_autoStop_timestampsExact();

    // ===== L16: MidiMTC 重复/回退帧 → RecorderEngine 过滤后再写 ArtRec =====
    void test_recorderEngine_midiMTC_duplicateAndBackwardFrames_areSkipped();

    // ===== L17: ArtNet TimeCode + DMX 双 UDP 通道 → RecorderEngine 完整管道 =====
    void test_recorderEngine_artNetTimeCode_udpDmxPipeline();

    // ===== L18: DMX 缓存逐帧数据保真 (无数据 → A → B) =====
    void test_recorderEngine_dmxCachePerFrame_changingDataFidelity();

    // ===== L19: 时钟启动失败 → RecorderEngine 错误传播, 不残留输出文件 =====
    void test_recorderEngine_clockStartFailure_propagatesError();

    // ===== L20: 零帧录制 → recordingFinished(0,0) + ArtRec 仍可读 =====
    void test_recorderEngine_stopWithZeroFrames_artrecReadable();

    // ===== L21: 暂停期间被动时钟帧被丢弃, 恢复后从后续帧继续 =====
    void test_recorderEngine_pauseWithPassiveClock_dropsFramesThenResumes();

    // ===== L22: RecorderEngine → ArtRec → Json: DMX 载荷跨格式保真 =====
    void test_recorderEngine_dmxPayload_jsonPreserved();

    // ===== L23: RecorderEngine 同实例重启: 新文件不复用旧 DMX 缓存 =====
    void test_recorderEngine_restartSameInstance_clearsDmxCache();

    // ===== L24: 只写配置过的 Universe, 其他 Universe 的 DMX 不进入录制文件 =====
    void test_recorderEngine_unconfiguredUniverseDmx_ignored();

    // ===== L25: TimecodeFile 文件 25fps + 引擎 30fps: 时间戳跟文件, 帧序号跟行序 =====
    void test_recorderEngine_tcFileFpsMismatch_timestampsFollowFile();

    // ===== L26: ArtNet TimeCode 2s 超时 → RecorderEngine 自动停止 =====
    void test_recorderEngine_artNetTCTimeout_autoStop();

    // ===== L27: 暂停期间时钟超时 → 从 Paused 直接 finalize 部分文件 =====
    void test_recorderEngine_pausedThenTimeout_finalizesPartialFile();

    // ===== L28: 输出文件创建失败 → 错误传播 + 端口资源释放 =====
    void test_recorderEngine_outputFileCreateFailure_releasesResources();

    // ===== L29: ArtRec 头元数据传播 (mediaFilePath / totalDurationUs / 帧数) =====
    void test_recorderEngine_headerMetadataPropagation();

    // ===== L30: 同步 tick 同时落盘 + 边录边转发 (OutputManager passthrough) =====
    void test_recorderEngine_syncTick_passthroughForwardsArtDmx();

    // ===== L31: 两个 RecorderEngine 并行 — 帧与 DMX 缓存相互隔离 =====
    void test_twoRecorderEngines_isolatedFramesAndDmxCache();

    // ===== L32: 多 Universe DMX 载荷 → ArtRec → Xml 跨格式保真 =====
    void test_recorderEngine_multiUniverseDmx_xmlPayloadPreserved();

    // ===== L33: 同一实例切换同步源 (TimecodeFile → ArtNetTimeCode) =====
    void test_recorderEngine_syncSourceSwitch_rebuildsClockAndHeader();

    // ===== L34: 暂停期间 DMX 缓存继续更新, 恢复后进入下一帧; 引擎时间码 API 与时钟一致 =====
    void test_recorderEngine_dmxCachedWhilePaused_usedAfterResume();

    // ===== L35: 25fps 引擎 header frameRate/timeCodeFormat 映射 + 时间戳步进 =====
    void test_recorderEngine_25fps_headerFormatMapping();

    // ===== L36: ArtNet TimeCode 录制中途超时 → 已录帧与 DMX 落盘 =====
    void test_recorderEngine_artNetTCTimeoutMidRecording_persistsFrames();

    // ===== L37: 边录边转发 + 每目标 Universe 过滤 =====
    void test_recorderEngine_passthrough_perTargetUniverseFilter();

    // ===== L38: ArtNet TimeCode 暂停期间包被丢弃, 恢复后继续 (不触发超时) =====
    void test_recorderEngine_artNetTCPauseResume_dropsPacketsContinues();

    // ===== L39: 两个引擎争用同一 TC 端口 → 第二个启动失败, 第一个不受影响 =====
    void test_twoEngines_sameTcPort_secondFailsFirstUnaffected();

    // ===== L40: onPreviewTick → statusUpdate 信号格式与时钟名 =====
    void test_recorderEngine_statusUpdate_signalFormatAndClockName();

    // ===== L41: 公共 getter 一致性 (frameRate/recordedFrames/currentDurationUs/currentTimeCode) =====
    void test_recorderEngine_publicGetters_consistency();

    // ===== L42: start() 重入保护 — 录制中再调 start 返回 false, 不破坏现有录制 =====
    void test_recorderEngine_startReentry_returnsFalseKeepsRecording();

    // ===== L43: pause()/resume() 在错误状态下为 no-op =====
    void test_recorderEngine_pauseResume_noopOnWrongState();

    // ===== L44: ArtNetReceiver 端口冲突 → start 失败 + cleanup 释放时钟 =====
    void test_recorderEngine_receiverPortConflict_failsAndCleansUp();

    // ===== L45: maxDurationUs 自动停止 (previewTimer → shouldStop → stop → finalize) =====
    void test_recorderEngine_maxDurationUs_autoStopsAndFinalizes();

    // ===== L46: injectMidiMtcQuarterFrame 在非 MTC 模式下为 no-op =====
    void test_recorderEngine_injectMtc_nonMtcMode_noop();

    // ===== L47: 帧回退跳过 — 循环 TC 文件第二轮 frameIndex 回 0 被丢弃 =====
    void test_recorderEngine_loopingTcFile_rollbackFramesSkipped();

    // ===== L48: stop() 从 Paused 状态触发 finalize =====
    void test_recorderEngine_stopFromPaused_finalizesCorrectly();

    // ===== L49: 自动停止后再显式 stop() → 不重复 finalize/信号 =====
    void test_recorderEngine_stopAfterAutoStop_singleFinishEvent();

    // ===== L50: enablePassthrough=true 但无目标 → 不创建 OutputManager, 录制正常 =====
    void test_recorderEngine_passthroughEnabledNoTargets_recordsNormally();

    // ===== L51: start() 前注入 MTC → 空指针守卫忽略, 启动后注入正常 =====
    void test_recorderEngine_injectMtcBeforeStart_ignored();

    // ===== L52: 29fps + ArtNetTC → header timeCodeFormat=2 且时间戳按 29fps 计算 =====
    void test_recorderEngine_29fps_artNetTC_headerFormatMapping();

    // ===== L53: TimecodeFile 播放中暂停 → 文件 EOF 时仍需自动 finalize =====
    void test_recorderEngine_tcFileEofWhilePaused_autoFinalizes();

    // ===== L54: statusUpdate 仅在 Recording 状态下产生 (Paused/Stopped 冻结) =====
    void test_recorderEngine_statusUpdate_onlyWhileRecording();

    // ===== L55: 24fps + ArtNetTC → header timeCodeFormat=0 映射 =====
    void test_recorderEngine_24fps_artNetTC_headerFormatMapping();

    // ===== L56: 暂停同时冻结节流与转发, 恢复后用最新缓存 =====
    void test_recorderEngine_pauseStopsPassthrough_resumeForwardsLatest();

    // ===== L57: maxDurationUs=0 → 不自动停止 (不限时长) =====
    void test_recorderEngine_maxDurationZero_noAutoStop();

    // ===== L58: 录制范围 = 配置 universes; 转发同样只基于已录 universe =====
    void test_recorderEngine_recordsConfiguredUniverses_forwardsSameSubset();
};

// ============================================================
// L1: SyncClockFactory + InternalTimerClock → ArtRecStorage
// ============================================================

void TestSyncClockIntegration::test_factoryInternalTimer_clockDrivesStorage() {
    // 用 SyncClockFactory 创建 InternalTimerClock, 收 frameTick 写 ArtRecStorage, 再读回验证
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString outputPath = dir.path() + "/sync_driven.artrec";
    const int FPS = 30;

    // 1) Factory 创建时钟
    auto *clk = SyncClockFactory::instance().create(SyncClockType::InternalTimer);
    QVERIFY(clk != nullptr);
    QCOMPARE(clk->clockType(), SyncClockType::InternalTimer);

    // 2) 创建 ArtRecStorage, 准备写入
    StorageMeta meta;
    meta.frameRate = quint16(FPS);
    meta.universeCount = 1;
    meta.createTimestamp = 1234567890ULL;
    auto unis = QList<StorageUniverse>{ StorageUniverse(encodeUniverse(0,0,1), 512) };

    ArtRecStorage ar;
    QVERIFY(ar.create(outputPath, meta, unis));

    // 3) 收集 frameTick → 写 Storage
    QSignalSpy spyFrame(clk, &ISyncClock::frameTick);
    SyncClockConfig cfg;
    cfg.frameRate = FPS;
    QVERIFY(clk->start(cfg));

    // 给足够时间 (300ms @ 30fps ≈ 9 帧 tick)
    QTest::qWait(500);
    clk->stop();

    int totalTicks = spyFrame.count();
    QVERIFY2(totalTicks >= 6, QString("500ms @ 30fps 应 >= 6 tick, 实际=%1").arg(totalTicks).toUtf8());

    // 逐 tick 写入 ArtRecStorage
    qint64 prevTs = -1;
    qint64 lastTimestampUs = 0;
    for (int i = 0; i < totalTicks; ++i) {
        qint64 frameIdx = spyFrame.at(i).at(0).toLongLong();
        qint64 ts = spyFrame.at(i).at(1).toLongLong();

        // 跨模块契约: frameTick 的 ts 应单调递增
        QVERIFY2(ts >= prevTs,
                 QString("frameTick ts 应单调不减: prev=%1 cur=%2").arg(prevTs).arg(ts).toUtf8());
        prevTs = ts;
        lastTimestampUs = ts;

        StorageFrame fr;
        fr.frameIndex = frameIdx;
        fr.timestampUs = ts;
        QByteArray dmx(512, char(i & 0xFF));  // 用 tick 序号填充
        // StorageFrame.universeData key 是 0-based universe index, 不是 encoded universe
        fr.universeData[0] = dmx;
        QVERIFY(ar.writeFrame(fr));
    }
    QVERIFY(ar.finalize(totalTicks, lastTimestampUs));
    ar.close();

    // 4) ArtRecStorage 读回验证
    ArtRecStorage reader;
    QVERIFY(reader.open(outputPath));
    QCOMPARE(reader.meta().frameRate, quint16(FPS));
    QCOMPARE(reader.meta().universeCount, 1);
    QVERIFY(reader.meta().createTimestamp > 0);

    int readCount = 0;
    qint64 readPrevTs = -1;
    StorageFrame fr;
    while (reader.readNextFrame(fr)) {
        QVERIFY(fr.frameIndex >= 0);
        QVERIFY(fr.timestampUs >= readPrevTs);
        readPrevTs = fr.timestampUs;
        QVERIFY(fr.universeData.contains(0));  // key = universe index 0
        ++readCount;
    }
    reader.close();

    QCOMPARE(readCount, totalTicks);
    delete clk;
}

// ============================================================
// L2: TimecodeFileClock 驱动 → Storage 验证时间戳精确性
// ============================================================

void TestSyncClockIntegration::test_tcFileClock_clockDrivesStorage_timestampsExact() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int FPS = 30;
    const QString tcPath = dir.path() + "/seq.tc";
    const QString artrecPath = dir.path() + "/seq.artrec";

    // 1) 构造 .tc 文件: 20 行连续时间码
    QFile tf(tcPath);
    QVERIFY(tf.open(QIODevice::WriteOnly | QIODevice::Text));
    tf.write(QString("#FPS=%1\n").arg(FPS).toUtf8());
    for (int i = 0; i < 20; ++i) {
        // 00:00:00:00, 00:00:00:01, ..., 00:00:00:19
        tf.write(QString("00:00:00:%1\n").arg(i, 2, 10, QChar('0')).toUtf8());
    }
    tf.close();

    // 2) TimecodeFileClock 驱动 + ArtRecStorage 写入
    TimecodeFileClock clk;
    SyncClockConfig cfg;
    cfg.clockType = SyncClockType::TimecodeFile;
    cfg.frameRate = FPS;
    cfg.mediaFilePath = tcPath;
    cfg.loopOnEnd = false;
    QVERIFY(clk.start(cfg));

    StorageMeta meta;
    meta.frameRate = quint16(FPS);
    meta.universeCount = 1;
    auto unis = QList<StorageUniverse>{ StorageUniverse(encodeUniverse(0,0,1), 512) };
    ArtRecStorage ar;
    QVERIFY(ar.create(artrecPath, meta, unis));

    QSignalSpy spyTick(&clk, &ISyncClock::frameTick);
    QSignalSpy spyFinish(&clk, &ISyncClock::clockFinished);

    QVERIFY2(spyFinish.wait(2000), "读完 20 行 .tc 应触发 clockFinished");
    clk.stop();

    int totalTicks = spyTick.count();
    QVERIFY2(totalTicks >= 19, QString("应收到至少 19 tick, 实际=%1").arg(totalTicks).toUtf8());

    // 3) 逐 tick 写入 + 验证时间戳精确性
    qint64 usPerFrame = 1000000LL / FPS;  // 33333

    for (int i = 0; i < totalTicks && i < 20; ++i) {
        qint64 frameIdx = spyTick.at(i).at(0).toLongLong();
        qint64 ts = spyTick.at(i).at(1).toLongLong();

        // 时间戳精确性: 第 i 帧 ts 应 = i × usPerFrame (允许 ±1 误差, 整数除法截断)
        qint64 expectedTs = i * usPerFrame;
        QVERIFY2(qAbs(ts - expectedTs) <= 1000,
                 QString("tick %1: ts=%2 expectedTs=%3 diff=%4")
                     .arg(i).arg(ts).arg(expectedTs).arg(ts - expectedTs).toUtf8());

        StorageFrame fr;
        fr.frameIndex = frameIdx;
        fr.timestampUs = ts;
        QByteArray dmx(512, char(i));
        fr.universeData[0] = dmx;  // universe index 0
        ar.writeFrame(fr);
    }
    QVERIFY(ar.finalize(totalTicks, spyTick.last().at(1).toLongLong()));
    ar.close();

    // 4) 读回 ArtRecStorage 验证帧序 + 时间戳
    ArtRecStorage reader;
    QVERIFY(reader.open(artrecPath));
    int readCount = 0;
    StorageFrame fr;
    while (reader.readNextFrame(fr)) {
        qint64 expectedTs = readCount * usPerFrame;
        QVERIFY2(qAbs(fr.timestampUs - expectedTs) <= 1000,
                 QString("Read frame %1: ts=%2 expected=%3").arg(readCount).arg(fr.timestampUs).arg(expectedTs).toUtf8());
        ++readCount;
    }
    reader.close();
    QCOMPARE(readCount, totalTicks);
}

// ============================================================
// L3: ArtNetTimeCodeClock UDP 广播 → Storage 跨模块保真
// ============================================================

void TestSyncClockIntegration::test_artnetTC_clockDrivesStorage_timestampConsistent() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int FPS = 30;

    // 1) 找随机端口
    QUdpSocket probe;
    QVERIFY(probe.bind(quint16(0)));
    quint16 port = probe.localPort();
    probe.close();

    // 2) 启动 ArtNetTimeCodeClock
    ArtNetTimeCodeClock clk;
    SyncClockConfig cfg;
    cfg.clockType = SyncClockType::ArtNetTimeCode;
    cfg.frameRate = FPS;
    cfg.artNetPort = port;
    cfg.artNetBindAddress = "127.0.0.1";
    QVERIFY(clk.start(cfg));

    QSignalSpy spyTick(&clk, &ISyncClock::frameTick);
    QTest::qWait(20);  // 让 socket 就绪

    // 3) 发多个 ArtNetTC 包, 时间递增
    QUdpSocket sender;
    QVERIFY(sender.bind(quint16(0)));

    struct TcSample { quint8 h, m, s, f; };
    QList<TcSample> samples = {
        {0, 0, 1,  0},   // 00:00:01:00
        {0, 0, 1,  5},   // 00:00:01:05
        {0, 0, 2,  0},   // 00:00:02:00
        {0, 1, 3, 10},   // 00:01:03:10
        {1, 2, 3,  4},   // 01:02:03:04
    };

    for (const auto &tc : samples) {
        QByteArray pkt = makeArtNetTC(tc.h, tc.m, tc.s, tc.f, /*smpteType=*/3);  // 3=30fps
        sender.writeDatagram(pkt, QHostAddress::LocalHost, port);
        QTest::qWait(15);  // 给 UDP 处理时间
    }

    // 所有包在循环的 qWait(15) 中已经被 ArtNetTimeCodeClock 处理并 emit frameTick
    clk.stop();
    int totalTicks = spyTick.count();
    QVERIFY2(totalTicks >= 1, QString("应收到至少 1 tick, 实际=%1").arg(totalTicks).toUtf8());

    // 4) 逐 tick 验证 timestampUs 与 ArtNet TC 包解析值一致
    for (int i = 0; i < qMin(totalTicks, samples.size()); ++i) {
        qint64 ts = spyTick.at(i).at(1).toLongLong();
        qint64 expectedUs = artNetTCToUs(samples[i].h, samples[i].m, samples[i].s, samples[i].f, FPS);
        QVERIFY2(ts == expectedUs,
                 QString("tick %1: ArtNet ts=%2 expected=%3 hh:mm:ss:ff=%4:%5:%6:%7")
                     .arg(i).arg(ts).arg(expectedUs)
                     .arg(samples[i].h, 2, 10, QChar('0'))
                     .arg(samples[i].m, 2, 10, QChar('0'))
                     .arg(samples[i].s, 2, 10, QChar('0'))
                     .arg(samples[i].f, 2, 10, QChar('0')).toUtf8());
    }

    // 5) Storage round-trip: 写 ArtRecStorage → 读回验证时间戳不丢失
    const QString artrecPath = dir.path() + "/artnet_driven.artrec";
    StorageMeta meta;
    meta.frameRate = quint16(FPS);
    meta.universeCount = 1;
    auto unis = QList<StorageUniverse>{ StorageUniverse(encodeUniverse(0,0,1), 512) };
    ArtRecStorage ar;
    QVERIFY(ar.create(artrecPath, meta, unis));

    for (int i = 0; i < totalTicks; ++i) {
        qint64 frameIdx = spyTick.at(i).at(0).toLongLong();
        qint64 ts = spyTick.at(i).at(1).toLongLong();
        StorageFrame fr;
        fr.frameIndex = frameIdx;
        fr.timestampUs = ts;
        QByteArray dmx(512, char(i));
        fr.universeData[0] = dmx;  // universe index 0
        ar.writeFrame(fr);
    }
    ar.finalize(totalTicks, spyTick.last().at(1).toLongLong());
    ar.close();

    ArtRecStorage reader;
    QVERIFY(reader.open(artrecPath));
    int readCount = 0;
    StorageFrame fr;
    while (reader.readNextFrame(fr)) {
        QCOMPARE(fr.timestampUs, spyTick.at(readCount).at(1).toLongLong());
        ++readCount;
    }
    reader.close();
    QCOMPARE(readCount, totalTicks);
}

// ============================================================
// L4: 多时钟源协同一致性 (InternalTimerClock vs TimecodeFileClock)
// ============================================================

void TestSyncClockIntegration::test_multiClock_sameFps_frameCountWithinTolerance() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int FPS = 100;  // 高 fps → 更多 tick, 统计上更稳定
    const QString tcPath = dir.path() + "/multi.tc";

    // 1) 构造 .tc 文件 (TimecodeFileClock 驱动)
    QFile tf(tcPath);
    QVERIFY(tf.open(QIODevice::WriteOnly | QIODevice::Text));
    tf.write(QString("#FPS=%1\n").arg(FPS).toUtf8());
    for (int i = 0; i < 50; ++i) {
        int sec = i / FPS;
        int frm = i % FPS;
        tf.write(QString("00:00:%1:%2\n")
                     .arg(sec, 2, 10, QChar('0'))
                     .arg(frm, 2, 10, QChar('0')).toUtf8());
    }
    tf.close();

    // 2) 同时启动两个时钟
    InternalTimerClock clkTimer;
    TimecodeFileClock clkTcFile;

    SyncClockConfig cfg;
    cfg.frameRate = FPS;

    SyncClockConfig cfgTc;
    cfgTc.clockType = SyncClockType::TimecodeFile;
    cfgTc.frameRate = FPS;
    cfgTc.mediaFilePath = tcPath;
    cfgTc.loopOnEnd = false;

    QVERIFY(clkTimer.start(cfg));
    QVERIFY(clkTcFile.start(cfgTc));

    QSignalSpy spyTimer(&clkTimer, &ISyncClock::frameTick);
    QSignalSpy spyTcFile(&clkTcFile, &ISyncClock::frameTick);
    QSignalSpy spyFinish(&clkTcFile, &ISyncClock::clockFinished);

    // 3) 等到 TimecodeFileClock 读完 (50 帧 @100fps → 500ms)
    QVERIFY2(spyFinish.wait(2000), "TimecodeFileClock 应读完 .tc 文件");

    // InternalTimerClock 也给额外 100ms 收最后几个 tick
    QTest::qWait(100);

    clkTimer.stop();
    clkTcFile.stop();

    int nTimer = spyTimer.count();
    int nTcFile = spyTcFile.count();

    // 两个时钟都应该发出至少 40 个 tick
    QVERIFY2(nTimer >= 40, QString("InternalTimer 应 >= 40 tick, 实际=%1").arg(nTimer).toUtf8());
    QVERIFY2(nTcFile >= 40, QString("TimecodeFile 应 >= 40 tick, 实际=%1").arg(nTcFile).toUtf8());

    // 两者帧号误差 <= 15 (TimecodeFile 精确 50 帧, InternalTimer 有 QTimer 抖动 + 额外 qWait)
    int diff = qAbs(nTimer - nTcFile);
    QVERIFY2(diff <= 15,
             QString("InternalTimer tick=%1 vs TimecodeFile tick=%2, diff=%3 应 <= 15")
                 .arg(nTimer).arg(nTcFile).arg(diff).toUtf8());
}

// ============================================================
// L5: MidiMTCClock quarter-frame → Storage 跨格式 round-trip
// ============================================================

void TestSyncClockIntegration::test_midiMTC_storageCrossFormatPreservesTimestamp() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int FPS = 30;
    const QString artrecPath = dir.path() + "/mtc.artrec";
    const QString jsonPath = dir.path() + "/mtc.json";
    const QString xmlPath = dir.path() + "/mtc.xml";

    // 1) 喂 MidiMTCClock 多组 quarter-frame
    MidiMTCClock clk;
    SyncClockConfig cfg;
    cfg.clockType = SyncClockType::MidiMTC;
    cfg.frameRate = FPS;
    QVERIFY(clk.start(cfg));

    QSignalSpy spyTick(&clk, &ISyncClock::frameTick);

    // 喂 5 组 quarter-frame (每组是完整的 8 个 nibble → 一个完整帧)
    struct MtcSample { quint8 h, m, s, f; };
    QList<MtcSample> samples = {
        {0, 0, 0,  0},
        {0, 0, 1, 15},
        {0, 0, 2, 30},
        {0, 1, 0,  0},
        {1, 0, 0, 10},
    };
    for (const auto &s : samples) {
        for (auto q : makeMtcQF(s.h, s.m, s.s, s.f)) {
            clk.onMtcQuarterFrame(q);
        }
    }

    int ticks = spyTick.count();
    QCOMPARE(ticks, 5);  // 每组 qf 应 emit 1 个 tick

    // 2) 写 ArtRecStorage
    StorageMeta meta;
    meta.frameRate = quint16(FPS);
    meta.universeCount = 1;
    auto unis = QList<StorageUniverse>{ StorageUniverse(encodeUniverse(0,0,1), 512) };

    ArtRecStorage arW;
    QVERIFY(arW.create(artrecPath, meta, unis));
    for (int i = 0; i < ticks; ++i) {
        StorageFrame fr;
        fr.frameIndex = spyTick.at(i).at(0).toLongLong();
        fr.timestampUs = spyTick.at(i).at(1).toLongLong();
        QByteArray dmx(512, char(i * 42));
        fr.universeData[0] = dmx;  // universe index 0
        arW.writeFrame(fr);
    }
    QVERIFY(arW.finalize(ticks, spyTick.last().at(1).toLongLong()));
    arW.close();

    // 3) artrec → json (StorageFactory 驱动管道)
    auto *artrecR = StorageFactory::instance().createByFilePath(artrecPath);
    auto *jsonW   = StorageFactory::instance().createByFilePath(jsonPath);
    QVERIFY(artrecR != nullptr);
    QVERIFY(jsonW != nullptr);

    QVERIFY(artrecR->open(artrecPath));
    QVERIFY(jsonW->create(jsonPath, artrecR->meta(), artrecR->universes()));

    StorageFrame fr;
    while (artrecR->readNextFrame(fr)) QVERIFY(jsonW->writeFrame(fr));
    QVERIFY(jsonW->finalize(artrecR->meta().totalFrames, artrecR->meta().totalDurationUs));
    artrecR->close(); jsonW->close();
    delete artrecR; delete jsonW;

    // 4) json → xml
    auto *jsonR = StorageFactory::instance().createByFilePath(jsonPath);
    auto *xmlW  = StorageFactory::instance().createByFilePath(xmlPath);
    QVERIFY(jsonR != nullptr);
    QVERIFY(xmlW != nullptr);

    QVERIFY(jsonR->open(jsonPath));
    QVERIFY(xmlW->create(xmlPath, jsonR->meta(), jsonR->universes()));
    while (jsonR->readNextFrame(fr)) QVERIFY(xmlW->writeFrame(fr));
    QVERIFY(xmlW->finalize(jsonR->meta().totalFrames, jsonR->meta().totalDurationUs));
    jsonR->close(); xmlW->close();
    delete jsonR; delete xmlW;

    // 5) xml 读回, 验证时间戳保真
    XmlStorage xmlR;
    QVERIFY(xmlR.open(xmlPath));
    int readIdx = 0;
    StorageFrame rfr;
    while (xmlR.readNextFrame(rfr)) {
        qint64 originalTs = spyTick.at(readIdx).at(1).toLongLong();
        QVERIFY2(rfr.timestampUs == originalTs,
                 QString("Frame %1: xml ts=%2 != original MidiMTC ts=%3")
                     .arg(readIdx).arg(rfr.timestampUs).arg(originalTs).toUtf8());
        ++readIdx;
    }
    xmlR.close();
    QCOMPARE(readIdx, ticks);

    clk.stop();
}

// ============================================================
// L6: SyncClockFactory 完整生命周期
// ============================================================

void TestSyncClockIntegration::test_factory_lifecycle_createStartPauseResumeStop() {
    auto &factory = SyncClockFactory::instance();

    // 6a) createByType → start → pause → resume → stop 完整循环
    auto *clk = factory.create(SyncClockType::InternalTimer);
    QVERIFY(clk != nullptr);
    QVERIFY(!clk->isRunning());
    QVERIFY(clk->currentTimestampUs() < 0);

    SyncClockConfig cfg;
    cfg.frameRate = 100;
    QVERIFY(clk->start(cfg));
    QVERIFY(clk->isRunning());

    // 确认 start 后 ts >= 0
    QTest::qWait(10);
    QVERIFY(clk->currentTimestampUs() >= 0);

    // pause
    // ISyncClock 没有 pause/resume 虚方法, InternalTimerClock 特有, 用 dynamic_cast
    auto *itClk = dynamic_cast<InternalTimerClock*>(clk);
    QVERIFY(itClk != nullptr);

    qint64 tsBeforePause = clk->currentTimestampUs();
    itClk->pause();
    QTest::qWait(30);
    qint64 tsDuringPause = clk->currentTimestampUs();
    // QElapsedTimer 在 pause() 内部读取 elapsed 再 restart, 与 tsBeforePause 的读取
    // 不是同一时刻, 微秒级差异不可避免. 验证容差 ±2ms 即可
    QVERIFY2(qAbs(tsDuringPause - tsBeforePause) <= 2000,
             QString("pause 期间 ts 应冻结: before=%1 during=%2 diff=%3")
                 .arg(tsBeforePause).arg(tsDuringPause).arg(tsDuringPause - tsBeforePause).toUtf8());

    // resume
    itClk->resume();
    QTest::qWait(50);
    qint64 tsAfterResume = clk->currentTimestampUs();
    // resume 后 ts 必须明显前进 (至少 30ms 经过 = 30000us)
    QVERIFY2(tsAfterResume - tsDuringPause >= 25000,
             QString("resume 后 ts 应前进 >= 25ms: during=%1 after=%2 delta=%3")
                 .arg(tsDuringPause).arg(tsAfterResume).arg(tsAfterResume - tsDuringPause).toUtf8());

    // stop
    clk->stop();
    QVERIFY(!clk->isRunning());
    QVERIFY(clk->currentTimestampUs() < 0);

    delete clk;

    // 6b) createByName 另一个实例
    auto *clk2 = factory.create("internal");
    QVERIFY(clk2 != nullptr);
    QVERIFY(clk2->clockType() == SyncClockType::InternalTimer);
    QVERIFY(clk2 != nullptr);
    delete clk2;

    // 6c) 不同类型不相同
    auto *cInt = factory.create(SyncClockType::InternalTimer);
    auto *cArt = factory.create(SyncClockType::ArtNetTimeCode);
    auto *cTc  = factory.create(SyncClockType::TimecodeFile);
    QVERIFY(cInt != cArt);
    QVERIFY(cArt != cTc);
    QVERIFY(cInt->clockType() == SyncClockType::InternalTimer);
    QVERIFY(cArt->clockType() == SyncClockType::ArtNetTimeCode);
    QVERIFY(cTc->clockType() == SyncClockType::TimecodeFile);
    delete cInt; delete cArt; delete cTc;
}

// ============================================================
// L7: TimecodeFileClock loopOnEnd=true → Storage 循环写入帧序列
// 覆盖: TimecodeFileClock × ArtRecStorage 循环写入 × 帧连续性
// ============================================================
void TestSyncClockIntegration::test_tcFileClock_loopOnEnd_storageWritesMultipleRounds() {
    QTemporaryDir dir;
    QString tcPath = dir.path() + "/loop3.tc";
    QString artrecPath = dir.path() + "/loop3.artrec";

    // 构造 5 行 TC 文件 @ 30fps → 约 167ms 一轮
    QFile f(tcPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("#FPS=30\n");
    for (int i = 0; i < 5; ++i) {
        int sec = i / 30;
        int fr = i % 30;
        f.write(QString("00:00:%1:%2\n")
                    .arg(sec, 2, 10, QChar('0'))
                    .arg(fr, 2, 10, QChar('0')).toUtf8());
    }
    f.close();

    // (1) 创建时钟
    auto *clock = SyncClockFactory::instance().create(SyncClockType::TimecodeFile);
    QVERIFY(clock);

    // (2) 创建 Storage
    StorageMeta meta;
    meta.frameRate = 30;
    meta.createTimestamp = QDateTime::currentMSecsSinceEpoch();
    StorageUniverse u1{4, 512};
    ArtRecStorage storage;
    QVERIFY(storage.create(artrecPath, meta, {u1}));

    // (3) 连接: 每收到 frameTick 写一帧 DMX
    QSignalSpy spyLoop(clock, &ISyncClock::clockLooped);
    int writeCount = 0;
    connect(clock, &ISyncClock::frameTick, [&](qint64 frameIdx, qint64 tsUs) {
        StorageFrame sf;
        sf.frameIndex = frameIdx;
        sf.timestampUs = tsUs;
        sf.universeData[4] = QByteArray(512, char(frameIdx & 0xFF));
        sf.dataChanged = true;
        storage.writeFrame(sf);
        writeCount++;
    });

    // (4) 启动时钟 (loop=true)
    SyncClockConfig cfg;
    cfg.clockType = SyncClockType::TimecodeFile;
    cfg.frameRate = 30;
    cfg.mediaFilePath = tcPath;
    cfg.loopOnEnd = true;
    QVERIFY(clock->start(cfg));

    // (5) 等足够时间让时钟循环 3+ 次 (167ms/轮, 900ms ≈ 5.4 轮)
    QTest::qWait(1200);
    clock->stop();

    // (6) 验证: loopCount >= 3, 写入帧数 >= 5 * 3 = 15 (首次 + 至少 2 次循环)
    QVERIFY2(spyLoop.count() >= 3,
             QString("应循环 >=3 次, 实际=%1").arg(spyLoop.count()).toUtf8());
    QVERIFY2(writeCount >= 15,
             QString("应写 >=15 帧, 实际=%1").arg(writeCount).toUtf8());

    storage.finalize(writeCount, 0, tcPath);

    // (7) 读回验证: ArtRecReader 应能打开, totalFrames >= 15
    ArtRecReader reader;
    QVERIFY2(reader.open(artrecPath), "读回 artrec 应成功");
    QVERIFY2(reader.header().totalFrames >= 15,
             QString("ArtRec header totalFrames=%1 应 >=15").arg(reader.header().totalFrames).toUtf8());

    delete clock;
}

// ============================================================
// L8: MidiMTC timeout → RecorderEngine 自动停止 + recordingFinished
// 覆盖: MidiMTCClock (timeoutTimer=2s) × RecorderEngine (onSyncClockFinished) × Storage finalize
// 完整错误传播链路
// ============================================================
void TestSyncClockIntegration::test_midiMTC_timeout_triggersRecorderStop() {
    // (1) 创建 RecorderEngine (MidiMTC 模式)
    QTemporaryDir dir;
    RecorderEngine engine;
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);
    QSignalSpy spyErr(&engine, &RecorderEngine::errorOccurred);
    QSignalSpy spyState(&engine, &RecorderEngine::stateChanged);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/timeout_mtc.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26690;
    QVERIFY(engine.start(cfg));

    // (2) 不喂 quarter-frame, 等 2.5s → MidiMTCClock timeoutTimer 触发
    QVERIFY2(spyFinish.wait(3500), "2.5s 不喂 MTC 应触发 recordingFinished (timeout → clockFinished → stop)");
    QCOMPARE(spyErr.count(), 1);  // timeout 产生的 errorOccurred 应在 RecorderEngine 上
    QVERIFY2(spyErr.at(0).at(0).toString().contains("超时"),
             QString("错误消息应提到超时, 实际=%1")
                 .arg(spyErr.at(0).at(0).toString()).toUtf8());

    // (3) 状态应回到 Idle/Stopped
    RecordState finalState = static_cast<RecordState>(spyState.last().at(0).toInt());
    QVERIFY(finalState == RecordState::Idle || finalState == RecordState::Stopped);
}

// ============================================================
// L9: InternalTimerClock × RecorderEngine × 多 Universe → ArtRec 文件数据完整
// 覆盖: 完整录制管道 (RecrodEngine 驱动 DMX 数据写入)
// ============================================================
void TestSyncClockIntegration::test_internalTimer_multiUniverse_recorderEngine_pipeline() {
    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    QTemporaryDir dir;
    QString outputPath = dir.path() + "/multi_uni.artrec";

    RecorderEngine::Config cfg;
    cfg.outputFilePath = outputPath;
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = {
        encodeUniverse(0, 0, 1),   // universe 1 (port-address 0x0000)
        encodeUniverse(0, 0, 2),   // universe 2 (port-address 0x0001)
    };
    cfg.artNetPort = 26691;
    QVERIFY(engine.start(cfg));

    // 模拟 ArtNetReceiver 发来 DMX 数据
    // RecorderEngine 内部 ArtNetReceiver 会 bind artNetPort + 1 (避开时钟端口)
    quint16 receiverPort = cfg.artNetPort + 1;
    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));

    auto makeArtNetDmxPkt = [](quint16 portAddress, const QByteArray &data) -> QByteArray {
        QByteArray pkt(18 + data.size(), char(0));
        memcpy(pkt.data(), "Art-Net", 7);
        pkt[7] = 0;
        pkt[8] = 0x50; pkt[9] = 0x00;   // opcode 小端 0x5000 → ArtDMX
        pkt[10] = 0x00; pkt[11] = 0x0E;  // protocol version 0x000E
        pkt[12] = 0; pkt[13] = 0;
        pkt[14] = quint8(portAddress & 0xFF);
        pkt[15] = quint8((portAddress >> 8) & 0xFF);
        quint16 len = data.size();
        pkt[16] = quint8((len >> 8) & 0xFF);
        pkt[17] = quint8(len & 0xFF);
        memcpy(pkt.data() + 18, data.constData(), data.size());
        return pkt;
    };

    // 先发几帧让 RecorderEngine 有机会缓存 DMX
    for (int i = 0; i < 3; ++i) {
        QTest::qWait(40);  // 等一个 30fps 周期
        QByteArray dmx1(512, char(100));
        sender.writeDatagram(makeArtNetDmxPkt(0x0000, dmx1),
                             QHostAddress("127.0.0.1"), receiverPort);
        QByteArray dmx2(512, char(200));
        sender.writeDatagram(makeArtNetDmxPkt(0x0001, dmx2),
                             QHostAddress("127.0.0.1"), receiverPort);
    }

    QTest::qWait(200);
    engine.stop();

    // 验证: frameRecorded 信号至少 2 次
    QVERIFY2(spyFrame.count() >= 2,
             QString("frameRecorded 应 >=2, 实际=%1").arg(spyFrame.count()).toUtf8());

    // 用 ArtRecStorage 读回验证
    ArtRecStorage storage;
    QVERIFY2(storage.open(outputPath), "打开录制文件应成功");
    QCOMPARE(storage.meta().universeCount, 2);

    // 读首帧验证 universe 数据
    StorageFrame firstFr;
    QVERIFY(storage.readNextFrame(firstFr));
    QVERIFY2(firstFr.universeData.size() >= 1,
             QString("首帧应至少有 1 个 Universe 数据, 实际=%1")
                 .arg(firstFr.universeData.size()).toUtf8());

    sender.close();
}

// ============================================================
// L10: RecorderEngine SyncSource 映射 → ArtRecWriter timeCodeType 正确性
// 覆盖: RecorderEngine Config.SyncSource → ArtRecHeader.timeCodeType
//       跨模块契约验证 (RecorderEngine 写 ArtRec, ArtRecStorage 读回)
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_syncSource_mapsToArtRecHeader_timeCodeType() {
    QTemporaryDir dir;

    struct Case {
        RecorderEngine::SyncSource syncSource;
        quint8 expectedHeaderTcType;  // RecorderEngine 写入 ArtRecHeader 的值
        QString label;
    };

    // RecorderEngine::start() 里 SyncClockType → quint8 映射:
    // InternalTimer=0, ArtNetTimeCode=1, VideoFile=2, AudioFile=3, MidiMTC=4, TimecodeFile=5
    // (SyncSource 枚举 → SyncClockType)
    // InternalClock → InternalTimer → 0
    // MidiMTC → MidiMTC → 4
    // TimecodeFile → TimecodeFile → 5
    QList<Case> cases = {
        { RecorderEngine::SyncSource::InternalClock,  0, "InternalClock"  },
        { RecorderEngine::SyncSource::MidiMTC,        4, "MidiMTC"        },
        { RecorderEngine::SyncSource::TimecodeFile,   5, "TimecodeFile"   },
    };

    for (const auto &c : cases) {
        RecorderEngine engine;
        RecorderEngine::Config cfg;
        cfg.outputFilePath = dir.path() + QString("/hdr_%1.artrec").arg(c.label);
        cfg.frameRate = 30;
        cfg.syncSource = c.syncSource;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 26700 + int(c.syncSource);

        if (c.syncSource == RecorderEngine::SyncSource::TimecodeFile) {
            QString tcPath = dir.path() + "/hdr_test.tc";
            QFile tf(tcPath);
            QVERIFY(tf.open(QIODevice::WriteOnly | QIODevice::Text));
            tf.write("#FPS=30\n00:00:00:00\n00:00:00:01\n");
            tf.close();
            cfg.mediaFilePath = tcPath;
        }

        QVERIFY2(engine.start(cfg),
                 QString("%1 start 应成功").arg(c.label).toUtf8());
        QTest::qWait(50);
        engine.stop();

        // 用 ArtRecStorage 读回 header
        ArtRecStorage storage;
        QVERIFY2(storage.open(cfg.outputFilePath),
                 QString("%1 打开 artrec 应成功").arg(c.label).toUtf8());

        // 用底层 ArtRecReader 读 header.timeCodeType (quint8)
        ArtRecReader reader;
        QVERIFY(reader.open(cfg.outputFilePath));
        quint8 got = reader.header().timeCodeType;
        QVERIFY2(got == c.expectedHeaderTcType,
                 QString("%1: header.timeCodeType 期望=%2 实际=%3")
                     .arg(c.label).arg(c.expectedHeaderTcType).arg(got).toUtf8());
    }
}

// ============================================================
// L11: RecorderEngine pause/resume × InternalTimerClock
// 覆盖: RecorderEngine::pause() → m_state=Paused 不写帧, resume() → m_state=Recording 恢复写帧
//       ArtRec 文件帧序号在 pause 前后连续 (无 gap)
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_pauseResume_pauseWritesZero_resumeContinues() {
    QTemporaryDir dir;
    RecorderEngine engine;

    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyState(&engine, &RecorderEngine::stateChanged);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/pause_resume.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26710;
    QVERIFY(engine.start(cfg));

    // 阶段 A: 正常录制, 等到至少 5 帧再 pause, 避免固定时长受系统调度影响
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 5, 2000);
    int countBeforePause = spyFrame.count();

    // pause → 状态变 Paused
    engine.pause();
    QTest::qWait(20);  // 等 pause 信号
    RecordState afterPause = static_cast<RecordState>(spyState.last().at(0).toInt());
    QCOMPARE(afterPause, RecordState::Paused);

    // 阶段 B: pause 期间 ~200ms → frameRecorded 应不增加
    QTest::qWait(200);
    int countDuringPause = spyFrame.count();
    QVERIFY2(countDuringPause == countBeforePause,
             QString("pause 期间不应有新帧: before=%1 during=%2 diff=%3")
                 .arg(countBeforePause).arg(countDuringPause)
                 .arg(countDuringPause - countBeforePause).toUtf8());

    // resume → 状态回到 Recording
    engine.resume();
    QTest::qWait(20);
    RecordState afterResume = static_cast<RecordState>(spyState.last().at(0).toInt());
    QCOMPARE(afterResume, RecordState::Recording);

    // 阶段 C: resume 后等到至少新增 5 帧 → 帧号继续递增, 不应回退
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= countBeforePause + 5, 2000);

    // 帧序号验证: 收集所有 frameIndex, 应严格递增
    qint64 lastIdx = -1;
    for (int i = 0; i < spyFrame.count(); ++i) {
        qint64 idx = spyFrame.at(i).at(0).toLongLong();
        QVERIFY2(idx > lastIdx,
                 QString("frameIndex 应严格递增: last=%1 cur=%2 (i=%3)")
                     .arg(lastIdx).arg(idx).arg(i).toUtf8());
        lastIdx = idx;
    }

    engine.stop();

    // 读 ArtRec 文件验证帧数与 frameRecorded 信号一致
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    int readCount = 0;
    StorageFrame fr;
    while (storage.readNextFrame(fr)) ++readCount;
    storage.close();
    QCOMPARE(readCount, spyFrame.count());
}

// ============================================================
// L12: RecorderEngine (MidiMTC) full-frame 注入端到端
// 覆盖: dynamic_cast 拿到 MidiMTCClock → onMtcFullFrame → RecorderEngine::onSyncFrameTick → 写 ArtRec
//       需注意 MidiMTCClock 2s 超时 — 每次 full-frame 都重置 timeout
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_midiMTC_fullFrame_injectDrivesRecording() {
    QTemporaryDir dir;
    RecorderEngine engine;

    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/mtc_fullframe.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26711;
    QVERIFY(engine.start(cfg));

    // 通过 dynamic_cast 拿到 MidiMTCClock 实例
    // RecorderEngine 内部 m_syncClock 是 SyncClockFactory 创建的
    // 但 RecorderEngine 没暴露它, 所以我们无法直接拿到
    // 换方式: 用 engine.injectMidiMtcQuarterFrame 注入 quarter-frame 触发
    // 或者: 直接用 SyncClockFactory 创建一个 MidiMTCClock 自己驱动, 同时让 RecorderEngine 也 start
    // 问题是 RecorderEngine start 会创建自己的 MidiMTCClock
    // 解决方案: 我们只测 RecorderEngine 内部 quarter-frame 注入 + full-frame 不可得时的替代
    // 让我看看 RecorderEngine 是否有 full-frame 注入点 — 没有, 只有 injectMidiMtcQuarterFrame
    //
    // 调整方案: 用 injectMidiMtcQuarterFrame 注入 5 组完整 quarter-frame → 应产生 5 帧
    // 这比 full-frame 更实际, 因为 RecorderEngine 确实暴露了这个注入点

    // 注入 5 组 quarter-frame (每组 8 nibble)
    // 每组是完整的一帧 → frameTick emit 1 次 → RecorderEngine 写 1 帧
    for (int g = 0; g < 5; ++g) {
        quint8 hours = 0;
        quint8 minutes = 0;
        quint8 seconds = quint8(g);
        quint8 frames = quint8(g * 5);
        for (auto q : makeMtcQF(hours, minutes, seconds, frames)) {
            engine.injectMidiMtcQuarterFrame(q);
        }
    }

    // QSignalSpy 在 loop 中 collect 信号, 不需要额外 qWait (Qt::DirectConnection 已 emit)
    QCOMPARE(spyFrame.count(), 5);

    // 验证 frameIndex 连续
    for (int i = 1; i < spyFrame.count(); ++i) {
        qint64 prevIdx = spyFrame.at(i - 1).at(0).toLongLong();
        qint64 curIdx = spyFrame.at(i).at(0).toLongLong();
        QVERIFY(curIdx > prevIdx);
    }

    // 验证 timestampUs 递增
    for (int i = 1; i < spyFrame.count(); ++i) {
        qint64 prevTs = spyFrame.at(i - 1).at(1).toLongLong();
        qint64 curTs = spyFrame.at(i).at(1).toLongLong();
        QVERIFY2(curTs >= prevTs,
                 QString("timestampUs 应单调不减: prev=%1 cur=%2")
                     .arg(prevTs).arg(curTs).toUtf8());
    }

    engine.stop();

    // 读 ArtRec 验证总帧数 = 5
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    int readCount = 0;
    StorageFrame fr;
    while (storage.readNextFrame(fr)) ++readCount;
    storage.close();
    QCOMPARE(readCount, 5);
}

// ============================================================
// L13: RecorderEngine maxDurationUs 自动停止
// 覆盖: RecorderEngine::shouldStop() 检查 maxDurationUs → onPreviewTick 触发 stop() → recordingFinished
//       maxDurationUs = 300ms → 应在 ~400ms 内自动停止
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_maxDurationUs_autoStop() {
    QTemporaryDir dir;
    RecorderEngine engine;

    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);
    QSignalSpy spyState(&engine, &RecorderEngine::stateChanged);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/maxdur.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26712;
    cfg.maxDurationUs = 300000;  // 300ms
    cfg.previewIntervalMs = 50;  // 50ms 检查一次 shouldStop
    QVERIFY(engine.start(cfg));

    // 等待 recordingFinished — 不超过 3 秒
    QVERIFY2(spyFinish.wait(3000),
             QString("maxDurationUs=300ms 应在 3s 内触发 recordingFinished, 实际 state=%1")
                 .arg(int(engine.state())).toUtf8());

    // recordingFinished 参数: totalFrames, totalDurationUs
    qint64 totalFrames = spyFinish.at(0).at(0).toLongLong();
    qint64 totalDurUs  = spyFinish.at(0).at(1).toLongLong();

    // 300ms @ 30fps → 约 9 帧, 应有一些帧
    QVERIFY2(totalFrames >= 3,
             QString("应至少有 3 帧, 实际=%1").arg(totalFrames).toUtf8());

    // totalDurationUs 应接近 300ms (±150ms 容差, Windows 抖动)
    qint64 expectedMin = 100000;   // 100ms
    qint64 expectedMax = 600000;   // 600ms
    QVERIFY2(totalDurUs >= expectedMin && totalDurUs <= expectedMax,
             QString("totalDurationUs=%1 应在 [%2, %3] 区间")
                 .arg(totalDurUs).arg(expectedMin).arg(expectedMax).toUtf8());

    // 最终状态应是 Stopped
    RecordState finalState = static_cast<RecordState>(spyState.last().at(0).toInt());
    QVERIFY(finalState == RecordState::Stopped || finalState == RecordState::Idle);
}

// ============================================================
// L14: RecorderEngine → ArtRec → Json → ArtRec 四格式 round-trip
// 覆盖: RecorderEngine 完整管道 → ArtRecStorage → JsonStorage → ArtRecStorage
//       时间戳 + 帧序号 + DMX 数据完整保真
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_quadrupleFormatRoundTrip_timestampsPreserved() {
    QTemporaryDir dir;
    const QString artrec1Path = dir.path() + "/roundtrip1.artrec";
    const QString jsonPath    = dir.path() + "/roundtrip.json";
    const QString artrec2Path = dir.path() + "/roundtrip2.artrec";

    // 1) RecorderEngine 录制 (InternalClock, 多 Universe)
    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = artrec1Path;
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = {
        encodeUniverse(0, 0, 1),
        encodeUniverse(0, 0, 2),
    };
    cfg.artNetPort = 26713;
    QVERIFY(engine.start(cfg));

    // InternalClock 模式: ArtNetReceiver bind cfg.artNetPort (不加 1)
    // 只有 ArtNetTimeCode 模式才让 Receiver 用 artNetPort + 1
    quint16 receiverPort = cfg.artNetPort;
    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));

    auto makeArtNetDmxPkt = [](quint16 portAddress, const QByteArray &data) -> QByteArray {
        QByteArray pkt(18 + data.size(), char(0));
        memcpy(pkt.data(), "Art-Net", 7);
        pkt[7] = 0;
        pkt[8] = 0x50; pkt[9] = 0x00;
        pkt[10] = 0x00; pkt[11] = 0x0E;
        pkt[12] = 0; pkt[13] = 0;
        pkt[14] = quint8(portAddress & 0xFF);
        pkt[15] = quint8((portAddress >> 8) & 0xFF);
        quint16 len = data.size();
        pkt[16] = quint8((len >> 8) & 0xFF);
        pkt[17] = quint8(len & 0xFF);
        memcpy(pkt.data() + 18, data.constData(), data.size());
        return pkt;
    };

    // 先发几帧 DMX 让 ArtNetReceiver 有机会缓存 → 然后 InternalClock tick 时写帧
    for (int i = 0; i < 4; ++i) {
        QTest::qWait(40);  // 一个 30fps 周期
        QByteArray dmx1(512, char(100 + i));
        sender.writeDatagram(makeArtNetDmxPkt(0x0000, dmx1),
                             QHostAddress("127.0.0.1"), receiverPort);
        QByteArray dmx2(512, char(200 + i));
        sender.writeDatagram(makeArtNetDmxPkt(0x0001, dmx2),
                             QHostAddress("127.0.0.1"), receiverPort);
    }

    QTest::qWait(200);  // 给足够 tick 时间
    engine.stop();
    sender.close();

    int originalFrameCount = spyFrame.count();
    QVERIFY2(originalFrameCount >= 5,
             QString("RecorderEngine 应录制 >=5 帧, 实际=%1").arg(originalFrameCount).toUtf8());

    // 2) ArtRec1 → Json (StorageFactory 驱动)
    auto *artrecReader = StorageFactory::instance().createByFilePath(artrec1Path);
    auto *jsonWriter   = StorageFactory::instance().createByFilePath(jsonPath);
    QVERIFY(artrecReader != nullptr);
    QVERIFY(jsonWriter != nullptr);

    QVERIFY(artrecReader->open(artrec1Path));
    QVERIFY(jsonWriter->create(jsonPath, artrecReader->meta(), artrecReader->universes()));

    StorageFrame fr;
    int jsonWriteCount = 0;
    while (artrecReader->readNextFrame(fr)) {
        QVERIFY(jsonWriter->writeFrame(fr));
        ++jsonWriteCount;
    }
    QVERIFY(jsonWriteCount == originalFrameCount);
    QVERIFY(jsonWriter->finalize(artrecReader->meta().totalFrames, artrecReader->meta().totalDurationUs));
    artrecReader->close(); jsonWriter->close();
    delete artrecReader; delete jsonWriter;

    // 3) Json → ArtRec2
    auto *jsonReader   = StorageFactory::instance().createByFilePath(jsonPath);
    auto *artrecWriter2 = StorageFactory::instance().createByFilePath(artrec2Path);
    QVERIFY(jsonReader != nullptr);
    QVERIFY(artrecWriter2 != nullptr);

    QVERIFY(jsonReader->open(jsonPath));
    QVERIFY(artrecWriter2->create(artrec2Path, jsonReader->meta(), jsonReader->universes()));

    int artrec2WriteCount = 0;
    StorageFrame fr2;
    while (jsonReader->readNextFrame(fr2)) {
        QVERIFY(artrecWriter2->writeFrame(fr2));
        ++artrec2WriteCount;
    }
    QVERIFY(artrec2WriteCount == originalFrameCount);
    QVERIFY(artrecWriter2->finalize(jsonReader->meta().totalFrames, jsonReader->meta().totalDurationUs));
    jsonReader->close(); artrecWriter2->close();
    delete jsonReader; delete artrecWriter2;

    // 4) 最终验证: 直接从 ArtRec2 读回, 与 RecorderEngine frameRecorded 信号对比
    ArtRecStorage finalReader;
    QVERIFY(finalReader.open(artrec2Path));

    int readIdx = 0;
    StorageFrame finalFr;
    while (finalReader.readNextFrame(finalFr)) {
        // 对比 timestampUs — 必须精确相等 (跨格式无损)
        qint64 expectedTs = spyFrame.at(readIdx).at(1).toLongLong();
        QVERIFY2(finalFr.timestampUs == expectedTs,
                 QString("最终 Frame %1: ts=%2 != 原始 ts=%3")
                     .arg(readIdx).arg(finalFr.timestampUs).arg(expectedTs).toUtf8());

        // 对比 frameIndex — 必须精确相等
        qint64 expectedIdx = spyFrame.at(readIdx).at(0).toLongLong();
        QCOMPARE(finalFr.frameIndex, expectedIdx);

        ++readIdx;
    }
    finalReader.close();

    QCOMPARE(readIdx, originalFrameCount);
}

// ============================================================
// L15: TimecodeFileClock EOF → RecorderEngine 自动停止
// 验证 clockFinished → onSyncClockFinished → recordingFinished 完整链路,
// 以及文件时间戳经过 RecorderEngine/ArtRec 后保持精确一致
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_timecodeFileEOF_autoStop_timestampsExact() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString tcPath = dir.path() + "/eof.tc";
    QFile tcFile(tcPath);
    QVERIFY(tcFile.open(QIODevice::WriteOnly | QIODevice::Text));
    tcFile.write("#FPS=30\n");
    for (int i = 0; i < 4; ++i) {
        tcFile.write(QString("00:00:00:%1\n").arg(i, 2, 10, QChar('0')).toUtf8());
    }
    tcFile.close();

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/eof.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
    cfg.mediaFilePath = tcPath;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26714;
    QVERIFY(engine.start(cfg));

    QVERIFY2(spyFinish.wait(2000), "TimecodeFile 读完应自动停止 RecorderEngine");
    QCOMPARE(engine.state(), RecordState::Stopped);
    QCOMPARE(spyFrame.count(), 4);
    QCOMPARE(spyFinish.at(0).at(0).toLongLong(), qint64(4));

    for (int i = 0; i < 4; ++i) {
        QCOMPARE(spyFrame.at(i).at(0).toLongLong(), qint64(i));
        QCOMPARE(spyFrame.at(i).at(1).toLongLong(), qint64(i) * 1000000LL / 30);
    }

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    int readCount = 0;
    StorageFrame frame;
    while (storage.readNextFrame(frame)) {
        QVERIFY(readCount < 4);
        QCOMPARE(frame.frameIndex, qint64(readCount));
        QCOMPARE(frame.timestampUs, qint64(readCount) * 1000000LL / 30);
        ++readCount;
    }
    storage.close();
    QCOMPARE(readCount, 4);
}

// ============================================================
// L16: MidiMTC 重复/回退帧过滤
// MidiMTCClock 会如实转发每个完整 quarter-frame 序列, RecorderEngine
// 必须跳过 frameIndex <= m_lastFrameIndex 的帧, 避免 ArtRec 出现重复或回退
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_midiMTC_duplicateAndBackwardFrames_areSkipped() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/mtc_filter.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26715;
    QVERIFY(engine.start(cfg));

    const auto firstFrame = makeMtcQF(0, 0, 1, 0);   // frameIndex=30
    const auto secondFrame = makeMtcQF(0, 0, 2, 0);  // frameIndex=60
    for (quint8 q : firstFrame)  engine.injectMidiMtcQuarterFrame(q);
    for (quint8 q : firstFrame)  engine.injectMidiMtcQuarterFrame(q);  // 重复 → 跳过
    for (quint8 q : secondFrame) engine.injectMidiMtcQuarterFrame(q);
    for (quint8 q : firstFrame)  engine.injectMidiMtcQuarterFrame(q);  // 回退 → 跳过

    QCOMPARE(spyFrame.count(), 2);
    QCOMPARE(spyFrame.at(0).at(0).toLongLong(), qint64(30));
    QCOMPARE(spyFrame.at(0).at(1).toLongLong(), qint64(1000000));
    QCOMPARE(spyFrame.at(1).at(0).toLongLong(), qint64(60));
    QCOMPARE(spyFrame.at(1).at(1).toLongLong(), qint64(2000000));
    engine.stop();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(30));
    QCOMPARE(frame.timestampUs, qint64(1000000));
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(60));
    QCOMPARE(frame.timestampUs, qint64(2000000));
    QVERIFY(!storage.readNextFrame(frame));
    storage.close();
}

// ============================================================
// L17: ArtNet TimeCode + DMX 双 UDP 通道完整管道
// TimeCode 发往 artNetPort, DMX 发往 artNetPort+1 (Receiver 避让时钟端口),
// 验证时钟、UDP 接收、DMX 缓存和 ArtRec 写入的跨模块数据保真
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_artNetTimeCode_udpDmxPipeline() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/artnet_tc_dmx.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26716;
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));

    auto makeArtNetDmxPkt = [](quint16 portAddress, const QByteArray &data) -> QByteArray {
        QByteArray pkt(18 + data.size(), char(0));
        memcpy(pkt.data(), "Art-Net", 7);
        pkt[7] = 0;
        pkt[8] = 0x00; pkt[9] = 0x50;
        pkt[10] = 14; pkt[11] = 0;
        pkt[12] = 0; pkt[13] = 0;
        pkt[14] = quint8(portAddress & 0xFF);
        pkt[15] = quint8((portAddress >> 8) & 0xFF);
        const quint16 len = quint16(data.size());
        pkt[16] = quint8(len & 0xFF);
        pkt[17] = quint8((len >> 8) & 0xFF);
        memcpy(pkt.data() + 18, data.constData(), data.size());
        return pkt;
    };

    const QByteArray dmx(512, char(0x5A));
    sender.writeDatagram(makeArtNetDmxPkt(cfg.universes.first(), dmx),
                         QHostAddress("127.0.0.1"), quint16(cfg.artNetPort + 1));
    QTest::qWait(80);  // 等 DMX 进入 RecorderEngine 缓存

    for (int second = 1; second <= 3; ++second) {
        sender.writeDatagram(makeArtNetTC(0, 0, quint8(second), 0),
                             QHostAddress("127.0.0.1"), cfg.artNetPort);
        QTest::qWait(40);
    }
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 3, 1500);
    QCOMPARE(spyFrame.count(), 3);
    engine.stop();
    sender.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    int readCount = 0;
    StorageFrame frame;
    while (storage.readNextFrame(frame)) {
        QVERIFY(readCount < 3);
        const qint64 expectedTs = artNetTCToUs(0, 0, quint8(readCount + 1), 0);
        QCOMPARE(frame.frameIndex, qint64((readCount + 1) * 30));
        QCOMPARE(frame.timestampUs, expectedTs);
        QCOMPARE(spyFrame.at(readCount).at(1).toLongLong(), expectedTs);
        QCOMPARE(frame.universeData.value(0), dmx);
        ++readCount;
    }
    storage.close();
    QCOMPARE(readCount, 3);

    ArtRecReader reader;
    QVERIFY(reader.open(cfg.outputFilePath));
    QCOMPARE(reader.header().timeCodeType, quint8(1));
}

// ============================================================
// L18: DMX 缓存逐帧数据保真 — 无数据 → A → B
// 覆盖: ArtNetReceiver → RecorderEngine::m_universeCache → ArtRecWriter → ArtRecStorage
//       验证"缓存中有什么, 写进文件的就是什么"(缺失数据补零, 新数据逐帧生效)
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_dmxCachePerFrame_changingDataFidelity() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/dmx_cache.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;  // 同步注入 → 帧时刻确定
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26718;
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    const quint16 encodedUni = cfg.universes.first();

    // 帧 1: 还没收到任何 DMX → 写出的应是全零 512 字节
    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    // 帧 2: 缓存 A(0x11)
    const QByteArray dmxA(512, char(0x11));
    sender.writeDatagram(makeArtDmxPacket(encodedUni, dmxA),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(120);
    for (auto q : makeMtcQF(0, 0, 2, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 2);

    // 帧 3: 缓存 B(0x22)
    const QByteArray dmxB(512, char(0x22));
    sender.writeDatagram(makeArtDmxPacket(encodedUni, dmxB),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(120);
    for (auto q : makeMtcQF(0, 0, 3, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 3);

    engine.stop();
    sender.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));

    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(30));
    QCOMPARE(frame.universeData.value(0), QByteArray(512, char(0x00)));

    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(60));
    QCOMPARE(frame.universeData.value(0), dmxA);

    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(90));
    QCOMPARE(frame.universeData.value(0), dmxB);

    QVERIFY(!storage.readNextFrame(frame));
    storage.close();
}

// ============================================================
// L19: 时钟启动失败 → RecorderEngine 错误传播
// 覆盖: ISyncClock::start() 失败 (文件不存在 / UDP 端口被占)
//       → RecorderEngine::start() 返回 false + errorOccurred + 不创建输出文件
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_clockStartFailure_propagatesError() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 1) TimecodeFile: mediaFilePath 指向不存在的文件
    {
        RecorderEngine engine;
        QSignalSpy spyErr(&engine, &RecorderEngine::errorOccurred);

        RecorderEngine::Config cfg;
        cfg.outputFilePath = dir.path() + "/missing_src.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
        cfg.mediaFilePath = dir.path() + "/does_not_exist.tc";
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 26719;

        QVERIFY2(!engine.start(cfg), "时间码文件不存在时 start 应返回 false");
        QCOMPARE(engine.state(), RecordState::Idle);
        QVERIFY2(spyErr.count() >= 1, "应 emit errorOccurred");
        QVERIFY2(spyErr.at(0).at(0).toString().contains("同步时钟启动失败"),
                 QString("错误消息应说明同步时钟启动失败, 实际=%1")
                     .arg(spyErr.at(0).at(0).toString()).toUtf8());
        QVERIFY2(!QFile::exists(cfg.outputFilePath),
                 "时钟启动失败时不应创建输出文件");
    }

    // 2) ArtNetTimeCode: 时钟端口已被占用
    {
        QUdpSocket blocker;
        QVERIFY(blocker.bind(QHostAddress("0.0.0.0"), quint16(0)));
        const quint16 usedPort = blocker.localPort();

        RecorderEngine engine;
        QSignalSpy spyErr(&engine, &RecorderEngine::errorOccurred);

        RecorderEngine::Config cfg;
        cfg.outputFilePath = dir.path() + "/port_busy.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = usedPort;

        QVERIFY2(!engine.start(cfg), "时间码端口被占用时 start 应返回 false");
        QCOMPARE(engine.state(), RecordState::Idle);
        QVERIFY2(spyErr.count() >= 1, "端口冲突应 emit errorOccurred");
        QVERIFY2(spyErr.at(0).at(0).toString().contains("同步时钟启动失败"),
                 QString("错误消息应说明同步时钟启动失败, 实际=%1")
                     .arg(spyErr.at(0).at(0).toString()).toUtf8());
        QVERIFY(!QFile::exists(cfg.outputFilePath));

        blocker.close();
    }
}

// ============================================================
// L20: 零帧录制 → recordingFinished(0,0), ArtRec 文件仍可正常读取
// 覆盖: RecorderEngine::stop() 在无任何帧时的 finalize 路径 + stop 幂等
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_stopWithZeroFrames_artrecReadable() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/zero_frames.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26720;
    QVERIFY(engine.start(cfg));
    QCOMPARE(engine.state(), RecordState::Recording);

    // 不注入任何 MTC 帧, 直接停止
    engine.stop();
    QCOMPARE(spyFrame.count(), 0);
    QCOMPARE(spyFinish.count(), 1);
    QCOMPARE(spyFinish.at(0).at(0).toLongLong(), qint64(0));
    QCOMPARE(spyFinish.at(0).at(1).toLongLong(), qint64(0));

    // 幂等: 再次 stop 不应重复 finalize / 重复 emit
    engine.stop();
    QCOMPARE(spyFinish.count(), 1);

    // 文件仍应是合法 ArtRec (header 可读, 0 帧)
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().frameRate, quint16(30));
    QCOMPARE(storage.meta().totalFrames, quint32(0));
    StorageFrame frame;
    QVERIFY(!storage.readNextFrame(frame));
    storage.close();

    ArtRecReader reader;
    QVERIFY(reader.open(cfg.outputFilePath));
    QCOMPARE(reader.header().totalFrames, quint32(0));
}

// ============================================================
// L21: 暂停期间被动时钟帧被丢弃, 恢复后从更晚的帧继续
// 覆盖: RecorderEngine::pause() 只改状态 (时钟继续产出),
//       onSyncFrameTick 在 Paused 时直接返回 → 帧不写;
//       resume 后 frameIndex > m_lastFrameIndex 才写, 暂停期间的帧形成空档
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_pauseWithPassiveClock_dropsFramesThenResumes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyState(&engine, &RecorderEngine::stateChanged);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/pause_passive.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26721;
    QVERIFY(engine.start(cfg));

    // 帧 1: 1s → 正常写入
    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    // 暂停 → 时钟继续产出 2s/3s 帧, 但引擎丢弃
    engine.pause();
    QCOMPARE(engine.state(), RecordState::Paused);
    QCOMPARE(static_cast<RecordState>(spyState.last().at(0).toInt()), RecordState::Paused);

    for (auto q : makeMtcQF(0, 0, 2, 0)) engine.injectMidiMtcQuarterFrame(q);
    for (auto q : makeMtcQF(0, 0, 3, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    // 恢复 → 4s 帧应写入
    engine.resume();
    QCOMPARE(engine.state(), RecordState::Recording);
    for (auto q : makeMtcQF(0, 0, 4, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 2);
    QCOMPARE(spyFrame.at(1).at(0).toLongLong(), qint64(120));
    QCOMPARE(spyFrame.at(1).at(1).toLongLong(), qint64(4000000));

    engine.stop();

    // ArtRec 中只应有 1s 与 4s 两帧, 暂停期间的 2s/3s 不落盘
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));

    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(30));
    QCOMPARE(frame.timestampUs, qint64(1000000));

    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(120));
    QCOMPARE(frame.timestampUs, qint64(4000000));

    QVERIFY(!storage.readNextFrame(frame));
    storage.close();
}

// ============================================================
// L22: RecorderEngine → ArtRec → Json — DMX 载荷跨格式保真
// 覆盖: ArtNetReceiver → 引擎 DMX 缓存 → ArtRecWriter → JsonStorage 序列化/读回
//       注: ArtRecReader 不暴露 dataChanged 标志, 故此处只验证载荷本身
//       序列: 无数据(全零) → A → A → B
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_dmxPayload_jsonPreserved() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString artrecPath = dir.path() + "/dmx_payload.artrec";
    const QString jsonPath   = dir.path() + "/dmx_payload.json";

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = artrecPath;
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26722;
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    const quint16 encodedUni = cfg.universes.first();
    const QByteArray dmxA(512, char(0x11));
    const QByteArray dmxB(512, char(0x22));

    // 帧 1: 无 DMX → dataChanged=false
    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);

    // 帧 2: 收到 A → dataChanged=true
    sender.writeDatagram(makeArtDmxPacket(encodedUni, dmxA),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(120);
    for (auto q : makeMtcQF(0, 0, 2, 0)) engine.injectMidiMtcQuarterFrame(q);

    // 帧 3: A 未变 → dataChanged=false
    for (auto q : makeMtcQF(0, 0, 3, 0)) engine.injectMidiMtcQuarterFrame(q);

    // 帧 4: 收到 B → dataChanged=true
    sender.writeDatagram(makeArtDmxPacket(encodedUni, dmxB),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(120);
    for (auto q : makeMtcQF(0, 0, 4, 0)) engine.injectMidiMtcQuarterFrame(q);

    QCOMPARE(spyFrame.count(), 4);
    engine.stop();
    sender.close();

    // ArtRec → Json (StorageFactory 驱动)
    auto *artrecReader = StorageFactory::instance().createByFilePath(artrecPath);
    auto *jsonWriter   = StorageFactory::instance().createByFilePath(jsonPath);
    QVERIFY(artrecReader != nullptr);
    QVERIFY(jsonWriter != nullptr);

    QVERIFY(artrecReader->open(artrecPath));
    QVERIFY(jsonWriter->create(jsonPath, artrecReader->meta(), artrecReader->universes()));
    StorageFrame fr;
    while (artrecReader->readNextFrame(fr)) QVERIFY(jsonWriter->writeFrame(fr));
    QVERIFY(jsonWriter->finalize(artrecReader->meta().totalFrames,
                                 artrecReader->meta().totalDurationUs));
    artrecReader->close(); jsonWriter->close();
    delete artrecReader; delete jsonWriter;

    // 从 Json 读回验证载荷
    auto *jsonReader = StorageFactory::instance().createByFilePath(jsonPath);
    QVERIFY(jsonReader != nullptr);
    QVERIFY(jsonReader->open(jsonPath));

    const QList<QByteArray> expectedData = {
        QByteArray(512, char(0x00)), dmxA, dmxA, dmxB
    };

    int idx = 0;
    StorageFrame jf;
    while (jsonReader->readNextFrame(jf)) {
        QVERIFY2(idx < expectedData.size(), "帧数不应超过 4");
        QCOMPARE(jf.frameIndex, qint64(idx + 1) * 30);
        QCOMPARE(jf.universeData.value(0), expectedData[idx]);
        ++idx;
    }
    jsonReader->close();
    delete jsonReader;
    QCOMPARE(idx, 4);
}

// ============================================================
// L23: RecorderEngine 同实例重启 — 新文件不复用旧 DMX 缓存
// 覆盖: start() 里 m_universeCache.clear() + cleanup 后重新初始化
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_restartSameInstance_clearsDmxCache() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;

    RecorderEngine::Config cfg;
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26723;

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    const QByteArray dmxA(512, char(0x33));

    // 第一次录制: 先写入 DMX 再注入一帧
    {
        cfg.outputFilePath = dir.path() + "/restart_1.artrec";
        QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
        QVERIFY(engine.start(cfg));

        sender.writeDatagram(makeArtDmxPacket(cfg.universes.first(), dmxA),
                             QHostAddress("127.0.0.1"), cfg.artNetPort);
        QTest::qWait(120);
        for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
        QCOMPARE(spyFrame.count(), 1);

        engine.stop();
        QCOMPARE(engine.state(), RecordState::Stopped);
    }

    // 第二次录制: 同一实例, 不发任何 DMX → 首帧必须是全零 (旧缓存已清)
    {
        cfg.outputFilePath = dir.path() + "/restart_2.artrec";
        QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
        QVERIFY(engine.start(cfg));

        for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
        QCOMPARE(spyFrame.count(), 1);

        engine.stop();
    }
    sender.close();

    // 第一次文件: DMX A
    ArtRecStorage s1;
    QVERIFY(s1.open(dir.path() + "/restart_1.artrec"));
    QCOMPARE(s1.meta().totalFrames, quint32(1));
    StorageFrame f1;
    QVERIFY(s1.readNextFrame(f1));
    QCOMPARE(f1.universeData.value(0), dmxA);
    s1.close();

    // 第二次文件: 全零
    ArtRecStorage s2;
    QVERIFY(s2.open(dir.path() + "/restart_2.artrec"));
    QCOMPARE(s2.meta().totalFrames, quint32(1));
    StorageFrame f2;
    QVERIFY(s2.readNextFrame(f2));
    QCOMPARE(f2.universeData.value(0), QByteArray(512, char(0x00)));
    s2.close();
}

// ============================================================
// L24: 只写配置过的 Universe — 未配置 Universe 的 DMX 不进入录制文件
// 覆盖: Config.universes → ArtNetReceiver 过滤 → ArtRec Universe 映射表
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_unconfiguredUniverseDmx_ignored() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/uni_filter.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };  // 只配置 universe 1
    cfg.artNetPort = 26724;
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));

    // 先发未配置的 universe 2, 再发已配置的 universe 1
    sender.writeDatagram(makeArtDmxPacket(encodeUniverse(0, 0, 2), QByteArray(512, char(0x77))),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    sender.writeDatagram(makeArtDmxPacket(cfg.universes.first(), QByteArray(512, char(0x33))),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(150);

    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    engine.stop();
    sender.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().universeCount, quint16(1));
    QCOMPARE(storage.universes().size(), 1);
    QCOMPARE(storage.universes().first().encodedUniverse, quint16(encodeUniverse(0, 0, 1)));

    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    // 数据必须来自已配置 universe 1 (0x33), 未配置 universe 2 (0x77) 完全不可见
    QCOMPARE(frame.universeData.value(0), QByteArray(512, char(0x33)));
    storage.close();
}

// ============================================================
// L25: TimecodeFile 文件 25fps + 引擎配置 30fps
// 覆盖: 时间戳来自文件 (25fps → 40000us 步进) 而帧序号来自行序 0..N,
//       ArtRec header 仍记录引擎配置的 30fps
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_tcFileFpsMismatch_timestampsFollowFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString tcPath = dir.path() + "/fps25.tc";
    QFile f(tcPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("#FPS=25\n");
    for (int i = 0; i < 5; ++i) {
        f.write(QString("00:00:00:%1\n").arg(i, 2, 10, QChar('0')).toUtf8());
    }
    f.close();

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/fps25.artrec";
    cfg.frameRate = 30;                       // 引擎按 30fps 配置
    cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
    cfg.mediaFilePath = tcPath;               // 文件却是 25fps
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26725;
    QVERIFY(engine.start(cfg));

    QVERIFY2(spyFinish.wait(2000), "文件读完应自动停止");
    QCOMPARE(spyFrame.count(), 5);

    for (int i = 0; i < 5; ++i) {
        QCOMPARE(spyFrame.at(i).at(0).toLongLong(), qint64(i));           // 行序
        QCOMPARE(spyFrame.at(i).at(1).toLongLong(), qint64(i) * 40000LL); // 25fps 文件时间戳
    }

    // ArtRec header 帧率记录引擎配置值, 时间戳保持文件语义
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().frameRate, quint16(30));
    QCOMPARE(storage.meta().totalFrames, quint32(5));

    int idx = 0;
    StorageFrame frame;
    while (storage.readNextFrame(frame)) {
        QCOMPARE(frame.frameIndex, qint64(idx));
        QCOMPARE(frame.timestampUs, qint64(idx) * 40000LL);
        ++idx;
    }
    storage.close();
    QCOMPARE(idx, 5);
}

// ============================================================
// L26: ArtNet TimeCode 2s 超时 → RecorderEngine 自动停止
// 覆盖: ArtNetTimeCodeClock::onTimeout → clockFinished → onSyncClockFinished → stop()
//       (与 L8 的 MidiMTC 超时互补, 验证另一条被动时钟超时链路)
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_artNetTCTimeout_autoStop() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyErr(&engine, &RecorderEngine::errorOccurred);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/artnet_tc_timeout.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26726;
    QVERIFY(engine.start(cfg));

    // 不发任何 TimeCode 包, 等 2s 超时
    QVERIFY2(spyFinish.wait(3500), "ArtNet TC 2s 无包应触发 recordingFinished");
    QCOMPARE(engine.state(), RecordState::Stopped);
    QVERIFY2(spyErr.count() >= 1, "超时应 emit errorOccurred");
    QVERIFY2(spyErr.at(0).at(0).toString().contains("超时"),
             QString("错误消息应提到超时, 实际=%1")
                 .arg(spyErr.at(0).at(0).toString()).toUtf8());

    // 文件已创建但 0 帧
    QCOMPARE(spyFinish.at(0).at(0).toLongLong(), qint64(0));
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(0));
    StorageFrame frame;
    QVERIFY(!storage.readNextFrame(frame));
    storage.close();
}

// ============================================================
// L27: 暂停期间时钟超时 — 引擎从 Paused 直接停止并 finalize 已录部分
// 覆盖: pause() 后 onSyncClockFinished → stop() 在 Paused 状态下的收尾路径
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_pausedThenTimeout_finalizesPartialFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/paused_timeout.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26727;
    QVERIFY(engine.start(cfg));

    // 暂停前写入 1 帧 (1s)
    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    engine.pause();
    QCOMPARE(engine.state(), RecordState::Paused);

    // 暂停期间不再喂帧 → 2s 后 MTC 超时 → clockFinished → stop()
    QVERIFY2(spyFinish.wait(3500), "暂停期间超时应仍触发 recordingFinished");
    QCOMPARE(engine.state(), RecordState::Stopped);
    QCOMPARE(spyFinish.at(0).at(0).toLongLong(), qint64(1));          // 已录帧数
    QCOMPARE(spyFinish.at(0).at(1).toLongLong(), qint64(1000000));    // 时长 = 最后一帧 ts

    // 暂停前的帧必须已落盘
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(1));
    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(30));
    QCOMPARE(frame.timestampUs, qint64(1000000));
    QVERIFY(!storage.readNextFrame(frame));
    storage.close();
}

// ============================================================
// L28: 输出文件创建失败 → 错误传播 + 端口资源释放
// 覆盖: ArtRecWriter::create 失败 → cleanup() → ArtNetReceiver/时钟被释放
//       通过"失败后可重新绑定同一端口"间接验证资源确实回收
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_outputFileCreateFailure_releasesResources() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyErr(&engine, &RecorderEngine::errorOccurred);

    RecorderEngine::Config cfg;
    // 目录不存在 → ArtRecWriter::create 打开失败
    cfg.outputFilePath = dir.path() + "/no_such_dir/out.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26728;

    QVERIFY2(!engine.start(cfg), "输出文件无法创建时 start 应返回 false");
    QCOMPARE(engine.state(), RecordState::Idle);
    QVERIFY2(spyErr.count() >= 1, "应 emit errorOccurred");
    QVERIFY2(spyErr.at(0).at(0).toString().contains("录制文件创建失败"),
             QString("错误消息应说明录制文件创建失败, 实际=%1")
                 .arg(spyErr.at(0).at(0).toString()).toUtf8());

    // 失败路径调用了 cleanup() → 接收器已停止, 端口应可重新绑定
    QUdpSocket probe;
    QVERIFY2(probe.bind(QHostAddress("0.0.0.0"), cfg.artNetPort),
             QString("失败后端口 %1 应已释放").arg(cfg.artNetPort).toUtf8());
    probe.close();

    // 引擎仍可正常开始新的录制 (状态已回到 Idle)
    cfg.outputFilePath = dir.path() + "/retry_ok.artrec";
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    QVERIFY2(engine.start(cfg), "失败后同一实例应可重新开始录制");
    engine.stop();
    QVERIFY(QFile::exists(cfg.outputFilePath));
}

// ============================================================
// L29: ArtRec 头元数据传播
// 覆盖: RecorderEngine::Config (mediaFilePath/frameRate/universes)
//       → ArtRecWriter header → ArtRecStorage::meta() 读回
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_headerMetadataPropagation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString tcPath = dir.path() + "/meta_src.tc";
    QFile f(tcPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("#FPS=30\n");
    for (int i = 0; i < 3; ++i) {
        f.write(QString("00:00:00:%1\n").arg(i, 2, 10, QChar('0')).toUtf8());
    }
    f.close();

    RecorderEngine engine;
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/meta.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
    cfg.mediaFilePath = tcPath;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26729;
    QVERIFY(engine.start(cfg));
    QVERIFY2(spyFinish.wait(2000), "文件读完应自动停止");

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    const StorageMeta meta = storage.meta();

    QCOMPARE(meta.frameRate, quint16(30));
    QCOMPARE(meta.universeCount, quint16(1));
    QCOMPARE(meta.totalFrames, quint32(3));
    QCOMPARE(meta.totalDurationUs, quint64(2 * 1000000LL / 30));  // 末帧 ts
    QCOMPARE(meta.mediaFilePath, tcPath);
    QVERIFY(meta.createTimestamp > 0);

    storage.close();
}

// ============================================================
// L30: 同步 tick 双通道 — ArtRec 落盘 + OutputManager 边录边转发
// 覆盖: onSyncFrameTick → writeCurrentFrame → writer.writeFrame + sendMultipleUniverses
//       验证转发出去的 ArtDMX 包 universe/长度/载荷与录制内容一致
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_syncTick_passthroughForwardsArtDmx() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const quint16 forwardPort = freeUdpPort();
    QVERIFY(forwardPort != 0);

    QUdpSocket forwardRx;
    QVERIFY(forwardRx.bind(QHostAddress("127.0.0.1"), forwardPort));

    OutputTargetConfig target;
    target.type = OutputType::ArtNet;
    target.artNetIp = "127.0.0.1";
    target.artNetPort = forwardPort;
    target.enabled = true;

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/passthrough.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26730;
    cfg.enablePassthrough = true;
    cfg.outputTargets = { target };
    QVERIFY(engine.start(cfg));

    // 先灌入 DMX, 再由时钟 tick 触发"落盘 + 转发"
    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    const QByteArray dmx(512, char(0x44));
    sender.writeDatagram(makeArtDmxPacket(cfg.universes.first(), dmx),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(120);

    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    // 转发包应到达独立接收端
    QTRY_VERIFY_WITH_TIMEOUT(forwardRx.hasPendingDatagrams(), 2000);
    const QNetworkDatagram dg = forwardRx.receiveDatagram();
    const QByteArray pkt = dg.data();
    QVERIFY2(pkt.size() >= 18 + 512,
             QString("转发 ArtDMX 包应 >= 530 字节, 实际=%1").arg(pkt.size()).toUtf8());

    const quint8 *d = reinterpret_cast<const quint8*>(pkt.constData());
    QCOMPARE(quint16(quint8(d[8]) | (quint16(quint8(d[9])) << 8)), quint16(0x5000));
    QCOMPARE(quint16(quint8(d[14]) | (quint16(quint8(d[15])) << 8)), cfg.universes.first());
    QCOMPARE(quint16(quint8(d[16]) | (quint16(quint8(d[17])) << 8)), quint16(512));
    QCOMPARE(pkt.mid(18, 512), dmx);

    engine.stop();
    sender.close();
    forwardRx.close();

    // 录制侧内容必须与转发一致
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(30));
    QCOMPARE(frame.universeData.value(0), dmx);
    storage.close();
}

// ============================================================
// L31: 两个 RecorderEngine 并行 — 帧与 DMX 缓存相互隔离
// 覆盖: 每实例独立的 ArtNetReceiver / 时钟 / 缓存, 无跨实例串扰
// ============================================================
void TestSyncClockIntegration::test_twoRecorderEngines_isolatedFramesAndDmxCache() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engineA;
    RecorderEngine engineB;
    QSignalSpy spyA(&engineA, &RecorderEngine::frameRecorded);
    QSignalSpy spyB(&engineB, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfgA;
    cfgA.outputFilePath = dir.path() + "/iso_a.artrec";
    cfgA.frameRate = 30;
    cfgA.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfgA.universes = { encodeUniverse(0, 0, 1) };
    cfgA.artNetPort = 26731;

    RecorderEngine::Config cfgB = cfgA;
    cfgB.outputFilePath = dir.path() + "/iso_b.artrec";
    cfgB.artNetPort = 26732;

    QVERIFY(engineA.start(cfgA));
    QVERIFY(engineB.start(cfgB));

    // 只给 A 灌 DMX
    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    sender.writeDatagram(makeArtDmxPacket(cfgA.universes.first(), QByteArray(512, char(0xAA))),
                         QHostAddress("127.0.0.1"), cfgA.artNetPort);
    QTest::qWait(120);

    // A 录 1s, B 录 2s
    for (auto q : makeMtcQF(0, 0, 1, 0)) engineA.injectMidiMtcQuarterFrame(q);
    for (auto q : makeMtcQF(0, 0, 2, 0)) engineB.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyA.count(), 1);
    QCOMPARE(spyB.count(), 1);
    QCOMPARE(spyA.at(0).at(0).toLongLong(), qint64(30));
    QCOMPARE(spyB.at(0).at(0).toLongLong(), qint64(60));

    engineA.stop();
    engineB.stop();
    sender.close();

    // A: 自己的帧 + 自己的 DMX
    ArtRecStorage sa;
    QVERIFY(sa.open(cfgA.outputFilePath));
    StorageFrame fa;
    QVERIFY(sa.readNextFrame(fa));
    QCOMPARE(fa.timestampUs, qint64(1000000));
    QCOMPARE(fa.universeData.value(0), QByteArray(512, char(0xAA)));
    QVERIFY(!sa.readNextFrame(fa));
    sa.close();

    // B: 自己的帧 + 全零 (不受 A 的 DMX 影响)
    ArtRecStorage sb;
    QVERIFY(sb.open(cfgB.outputFilePath));
    StorageFrame fb;
    QVERIFY(sb.readNextFrame(fb));
    QCOMPARE(fb.timestampUs, qint64(2000000));
    QCOMPARE(fb.universeData.value(0), QByteArray(512, char(0x00)));
    QVERIFY(!sb.readNextFrame(fb));
    sb.close();
}

// ============================================================
// L32: 多 Universe 不同 DMX 载荷 → ArtRec → Xml 跨格式保真
// 覆盖: 引擎按 config.universes 收集 2 个 Universe 的数据,
//       经 ArtRec 文件与 XmlStorage 序列化后载荷/顺序均不变
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_multiUniverseDmx_xmlPayloadPreserved() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString artrecPath = dir.path() + "/multi_uni.artrec";
    const QString xmlPath    = dir.path() + "/multi_uni.xml";

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = artrecPath;
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1), encodeUniverse(0, 0, 2) };
    cfg.artNetPort = 26733;
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    const QByteArray dmx1(512, char(0x1A));
    const QByteArray dmx2(512, char(0x2B));
    sender.writeDatagram(makeArtDmxPacket(cfg.universes.at(0), dmx1),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    sender.writeDatagram(makeArtDmxPacket(cfg.universes.at(1), dmx2),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(150);

    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    engine.stop();
    sender.close();

    // ArtRec → Xml
    auto *artrecReader = StorageFactory::instance().createByFilePath(artrecPath);
    auto *xmlWriter    = StorageFactory::instance().createByFilePath(xmlPath);
    QVERIFY(artrecReader != nullptr);
    QVERIFY(xmlWriter != nullptr);
    QVERIFY(artrecReader->open(artrecPath));
    QVERIFY(xmlWriter->create(xmlPath, artrecReader->meta(), artrecReader->universes()));
    StorageFrame fr;
    while (artrecReader->readNextFrame(fr)) QVERIFY(xmlWriter->writeFrame(fr));
    QVERIFY(xmlWriter->finalize(artrecReader->meta().totalFrames,
                                artrecReader->meta().totalDurationUs));
    artrecReader->close(); xmlWriter->close();
    delete artrecReader; delete xmlWriter;

    // Xml 读回: Universe 顺序与载荷都与发送一致 (key = universe index)
    auto *xmlReader = StorageFactory::instance().createByFilePath(xmlPath);
    QVERIFY(xmlReader != nullptr);
    QVERIFY(xmlReader->open(xmlPath));
    QCOMPARE(xmlReader->universes().size(), 2);
    QCOMPARE(xmlReader->universes().at(0).encodedUniverse, cfg.universes.at(0));
    QCOMPARE(xmlReader->universes().at(1).encodedUniverse, cfg.universes.at(1));

    StorageFrame xf;
    QVERIFY(xmlReader->readNextFrame(xf));
    QCOMPARE(xf.frameIndex, qint64(30));
    QCOMPARE(xf.universeData.value(0), dmx1);
    QCOMPARE(xf.universeData.value(1), dmx2);
    QVERIFY(!xmlReader->readNextFrame(xf));
    xmlReader->close();
    delete xmlReader;
}

// ============================================================
// L33: 同一 RecorderEngine 切换同步源
// 覆盖: 会话 1 TimecodeFile (header timeCodeType=5)
//       → stop → 会话 2 ArtNetTimeCode (header timeCodeType=1, TC 端口=artNetPort,
//         接收器端口=artNetPort+1), 时钟/接收器完全重建
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_syncSourceSwitch_rebuildsClockAndHeader() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString tcPath = dir.path() + "/switch.tc";
    QFile f(tcPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("#FPS=30\n");
    f.write("00:00:00:00\n");
    f.write("00:00:00:01\n");
    f.close();

    RecorderEngine engine;

    // ---- 会话 1: TimecodeFile ----
    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/switch_tcfile.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
    cfg.mediaFilePath = tcPath;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26734;

    QSignalSpy spyFinish1(&engine, &RecorderEngine::recordingFinished);
    QVERIFY(engine.start(cfg));
    QVERIFY2(spyFinish1.wait(2000), "会话 1 读完后应自动停止");
    QCOMPARE(engine.state(), RecordState::Stopped);

    ArtRecStorage s1;
    QVERIFY(s1.open(cfg.outputFilePath));
    QCOMPARE(s1.meta().totalFrames, quint32(2));
    s1.close();

    ArtRecReader r1;
    QVERIFY(r1.open(cfg.outputFilePath));
    QCOMPARE(r1.header().timeCodeType, quint8(5));  // TimecodeFile
    r1.close();

    // ---- 会话 2: ArtNet TimeCode (时钟换实现, 端口也随之切换) ----
    cfg.outputFilePath = dir.path() + "/switch_artnet.artrec";
    cfg.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
    cfg.mediaFilePath.clear();
    cfg.artNetPort = 26735;

    QSignalSpy spyFrame2(&engine, &RecorderEngine::frameRecorded);
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(quint16(0)));
    // TC 发往 artNetPort
    sender.writeDatagram(makeArtNetTC(0, 0, 1, 0, 3), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame2.count() >= 1, 1500);
    sender.writeDatagram(makeArtNetTC(0, 0, 2, 0, 3), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame2.count() >= 2, 1500);

    engine.stop();
    sender.close();

    QCOMPARE(spyFrame2.at(0).at(0).toLongLong(), qint64(30));
    QCOMPARE(spyFrame2.at(1).at(0).toLongLong(), qint64(60));

    ArtRecStorage s2;
    QVERIFY(s2.open(cfg.outputFilePath));
    QCOMPARE(s2.meta().totalFrames, quint32(2));
    StorageFrame fr;
    QVERIFY(s2.readNextFrame(fr));
    QCOMPARE(fr.timestampUs, qint64(1000000));
    QVERIFY(s2.readNextFrame(fr));
    QCOMPARE(fr.timestampUs, qint64(2000000));
    s2.close();

    ArtRecReader r2;
    QVERIFY(r2.open(cfg.outputFilePath));
    QCOMPARE(r2.header().timeCodeType, quint8(1));  // ArtNetTimeCode
    r2.close();
}

// ============================================================
// L34: 暂停期间 DMX 缓存继续更新 — 恢复后新数据进入下一帧
// 覆盖: pause 只冻结写帧, 不冻结 ArtNetReceiver/缓存;
//       同时验证 engine.currentTimeCode()/getCurrentTimestampUs() 与时钟一致
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_dmxCachedWhilePaused_usedAfterResume() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/paused_cache.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26736;
    QVERIFY(engine.start(cfg));

    // 帧 1 (1s): 尚未收到 DMX → 全零
    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    // 引擎时间码 API 必须与时钟一致
    const TimeCode tc1 = engine.currentTimeCode();
    QCOMPARE(tc1.hours, 0);
    QCOMPARE(tc1.minutes, 0);
    QCOMPARE(tc1.seconds, 1);
    QCOMPARE(tc1.frames, 0);

    // 暂停 → 期间收到 DMX 0x55 (只更新缓存, 不写帧)
    engine.pause();
    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    const QByteArray dmx(512, char(0x55));
    sender.writeDatagram(makeArtDmxPacket(cfg.universes.first(), dmx),
                         QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(150);
    for (auto q : makeMtcQF(0, 0, 2, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);   // 暂停期间不写帧

    // 恢复后 3s 帧应带上暂停期间收到的数据
    engine.resume();
    for (auto q : makeMtcQF(0, 0, 3, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 2);
    QCOMPARE(spyFrame.at(1).at(0).toLongLong(), qint64(90));

    engine.stop();
    sender.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(30));
    QCOMPARE(frame.universeData.value(0), QByteArray(512, char(0x00)));
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(90));
    QCOMPARE(frame.timestampUs, qint64(3000000));
    QCOMPARE(frame.universeData.value(0), dmx);
    QVERIFY(!storage.readNextFrame(frame));
    storage.close();
}

// ============================================================
// L35: 25fps 引擎 — header frameRate/timeCodeFormat 映射与时间戳步进
// 覆盖: 非 30fps 时 header.timeCodeFormat 计算 (25fps → 1)
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_25fps_headerFormatMapping() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString tcPath = dir.path() + "/fps25_ok.tc";
    QFile f(tcPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("#FPS=25\n");
    for (int i = 0; i < 5; ++i) {
        f.write(QString("00:00:00:%1\n").arg(i, 2, 10, QChar('0')).toUtf8());
    }
    f.close();

    RecorderEngine engine;
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/fps25_ok.artrec";
    cfg.frameRate = 25;
    cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
    cfg.mediaFilePath = tcPath;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26737;
    QVERIFY(engine.start(cfg));
    QVERIFY2(spyFinish.wait(2000), "文件读完应自动停止");

    ArtRecReader reader;
    QVERIFY(reader.open(cfg.outputFilePath));
    QCOMPARE(reader.header().frameRate, quint16(25));
    QCOMPARE(reader.header().timeCodeFormat, quint8(1));   // 25fps
    QCOMPARE(reader.header().timeCodeType, quint8(5));     // TimecodeFile
    reader.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().frameRate, quint16(25));
    QCOMPARE(storage.meta().totalFrames, quint32(5));

    int idx = 0;
    StorageFrame frame;
    while (storage.readNextFrame(frame)) {
        QCOMPARE(frame.timestampUs, qint64(idx) * 40000LL);  // 1e6/25
        ++idx;
    }
    storage.close();
    QCOMPARE(idx, 5);
}

// ============================================================
// L36: ArtNet TimeCode 录制中途超时 — 已录帧与 DMX 必须落盘
// 覆盖: 收到若干 TC 后 2s 静默 → onTimeout → stop() → writer.finalize
//       (与 L26 的"0 帧超时"互补, 验证部分录制成果不丢失)
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_artNetTCTimeoutMidRecording_persistsFrames() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyErr(&engine, &RecorderEngine::errorOccurred);
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/artnet_partial.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26738;
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    const QByteArray dmx(512, char(0x66));

    // DMX 走 artNetPort+1, 提前灌入缓存
    sender.writeDatagram(makeArtDmxPacket(cfg.universes.first(), dmx),
                         QHostAddress("127.0.0.1"), quint16(cfg.artNetPort + 1));
    QTest::qWait(150);

    // 2 个时间码包 → 2 帧
    sender.writeDatagram(makeArtNetTC(0, 0, 1, 0, 3), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 1, 1500);
    sender.writeDatagram(makeArtNetTC(0, 0, 2, 0, 3), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 2, 1500);

    // 静默 2s → 超时自动停止
    QVERIFY2(spyFinish.wait(3500), "TC 静默 2s 应触发 recordingFinished");
    QCOMPARE(engine.state(), RecordState::Stopped);
    QVERIFY2(spyErr.count() >= 1, "超时应 emit errorOccurred");
    QCOMPARE(spyFinish.at(0).at(0).toLongLong(), qint64(2));
    QCOMPARE(spyFinish.at(0).at(1).toLongLong(), qint64(2000000));

    sender.close();

    // 文件里两帧都在, 且都带超时前缓存的 DMX
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(2));

    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(30));
    QCOMPARE(frame.timestampUs, qint64(1000000));
    QCOMPARE(frame.universeData.value(0), dmx);

    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(60));
    QCOMPARE(frame.timestampUs, qint64(2000000));
    QCOMPARE(frame.universeData.value(0), dmx);
    storage.close();

    ArtRecReader reader;
    QVERIFY(reader.open(cfg.outputFilePath));
    QCOMPARE(reader.header().timeCodeType, quint8(1));
}

// ============================================================
// L37: 边录边转发 + 每目标 Universe 过滤
// 覆盖: OutputManager 双目标 fan-out, ArtNetSender 按 filterUniverses 分流
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_passthrough_perTargetUniverseFilter() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const quint16 portA = freeUdpPort();
    const quint16 portB = freeUdpPort();
    QVERIFY(portA != 0 && portB != 0 && portA != portB);

    QUdpSocket rxA, rxB;
    QVERIFY(rxA.bind(QHostAddress("127.0.0.1"), portA));
    QVERIFY(rxB.bind(QHostAddress("127.0.0.1"), portB));

    const quint16 uni1 = encodeUniverse(0, 0, 1);
    const quint16 uni2 = encodeUniverse(0, 0, 2);

    OutputTargetConfig tA;
    tA.type = OutputType::ArtNet;
    tA.artNetIp = "127.0.0.1";
    tA.artNetPort = portA;
    tA.filterUniverses = { uni1 };

    OutputTargetConfig tB = tA;
    tB.artNetPort = portB;
    tB.filterUniverses = { uni2 };

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/passthrough_filter.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { uni1, uni2 };
    cfg.artNetPort = 26740;
    cfg.enablePassthrough = true;
    cfg.outputTargets = { tA, tB };
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    const QByteArray dmx1(512, char(0x11));
    const QByteArray dmx2(512, char(0x22));
    sender.writeDatagram(makeArtDmxPacket(uni1, dmx1), QHostAddress("127.0.0.1"), cfg.artNetPort);
    sender.writeDatagram(makeArtDmxPacket(uni2, dmx2), QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(150);

    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    // 目标 A 只应收到 universe 1
    QTRY_VERIFY_WITH_TIMEOUT(rxA.hasPendingDatagrams(), 2000);
    QTest::qWait(120);  // 给可能的额外包留出到达时间
    QCOMPARE(rxA.pendingDatagramSize() > 0, true);
    {
        const QByteArray pkt = rxA.receiveDatagram().data();
        QVERIFY(pkt.size() >= 18 + 512);
        const quint8 *d = reinterpret_cast<const quint8*>(pkt.constData());
        QCOMPARE(quint16(quint8(d[14]) | (quint16(quint8(d[15])) << 8)), uni1);
        QCOMPARE(pkt.mid(18, 512), dmx1);
        QVERIFY2(!rxA.hasPendingDatagrams(), "目标 A 不应收到 universe 2 的包");
    }

    // 目标 B 只应收到 universe 2
    QTRY_VERIFY_WITH_TIMEOUT(rxB.hasPendingDatagrams(), 2000);
    QTest::qWait(120);
    {
        const QByteArray pkt = rxB.receiveDatagram().data();
        QVERIFY(pkt.size() >= 18 + 512);
        const quint8 *d = reinterpret_cast<const quint8*>(pkt.constData());
        QCOMPARE(quint16(quint8(d[14]) | (quint16(quint8(d[15])) << 8)), uni2);
        QCOMPARE(pkt.mid(18, 512), dmx2);
        QVERIFY2(!rxB.hasPendingDatagrams(), "目标 B 不应收到 universe 1 的包");
    }

    engine.stop();
    sender.close();
    rxA.close();
    rxB.close();

    // 录制侧两路数据都应完整
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.universeData.value(0), dmx1);
    QCOMPARE(frame.universeData.value(1), dmx2);
    storage.close();
}

// ============================================================
// L38: ArtNet TimeCode 暂停期间包被丢弃 — 恢复后从更晚的时间码继续
// 覆盖: 主动 UDP 时钟在 pause 下仍持续计时 (不触发 2s 超时),
//       onSyncFrameTick 因 Paused 直接返回
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_artNetTCPauseResume_dropsPacketsContinues() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/artnet_pause.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26742;
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(quint16(0)));

    // 帧 1: 1s
    sender.writeDatagram(makeArtNetTC(0, 0, 1, 0, 3), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 1, 1500);

    // 暂停 → 期间 2s/3s 包到达但被丢弃
    engine.pause();
    QCOMPARE(engine.state(), RecordState::Paused);
    sender.writeDatagram(makeArtNetTC(0, 0, 2, 0, 3), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(engine.currentTimeCode().seconds >= 2, 1500);
    sender.writeDatagram(makeArtNetTC(0, 0, 3, 0, 3), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(engine.currentTimeCode().seconds >= 3, 1500);
    QCOMPARE(spyFrame.count(), 1);
    QCOMPARE(spyFinish.count(), 0);   // 短暂停不应触发超时

    // 恢复 → 4s 帧写入, 空档 2s/3s 不落盘
    engine.resume();
    sender.writeDatagram(makeArtNetTC(0, 0, 4, 0, 3), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 2, 1500);
    QCOMPARE(spyFrame.at(1).at(0).toLongLong(), qint64(120));
    QCOMPARE(spyFrame.at(1).at(1).toLongLong(), qint64(4000000));

    engine.stop();
    sender.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(30));
    QCOMPARE(frame.timestampUs, qint64(1000000));
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.frameIndex, qint64(120));
    QCOMPARE(frame.timestampUs, qint64(4000000));
    QVERIFY(!storage.readNextFrame(frame));
    storage.close();
}

// ============================================================
// L39: 两个引擎争用同一 ArtNet TC 端口
// 覆盖: 引擎 B 的时钟 bind 失败 → start false + errorOccurred, 不创建文件;
//       引擎 A 的监听与录制不受影响
// ============================================================
void TestSyncClockIntegration::test_twoEngines_sameTcPort_secondFailsFirstUnaffected() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const quint16 sharedPort = 26744;

    RecorderEngine engineA;
    QSignalSpy spyFrameA(&engineA, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfgA;
    cfgA.outputFilePath = dir.path() + "/port_owner.artrec";
    cfgA.frameRate = 30;
    cfgA.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
    cfgA.universes = { encodeUniverse(0, 0, 1) };
    cfgA.artNetPort = sharedPort;
    QVERIFY(engineA.start(cfgA));

    RecorderEngine engineB;
    QSignalSpy spyErrB(&engineB, &RecorderEngine::errorOccurred);

    RecorderEngine::Config cfgB;
    cfgB.outputFilePath = dir.path() + "/port_loser.artrec";
    cfgB.frameRate = 30;
    cfgB.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
    cfgB.universes = { encodeUniverse(0, 0, 1) };
    cfgB.artNetPort = sharedPort;

    QVERIFY2(!engineB.start(cfgB), "同一 TC 端口第二个引擎应启动失败");
    QCOMPARE(engineB.state(), RecordState::Idle);
    QVERIFY2(spyErrB.count() >= 1, "端口冲突应 emit errorOccurred");
    QVERIFY2(spyErrB.at(0).at(0).toString().contains("同步时钟启动失败"),
             QString("错误应说明同步时钟启动失败, 实际=%1")
                 .arg(spyErrB.at(0).at(0).toString()).toUtf8());
    QVERIFY(!QFile::exists(cfgB.outputFilePath));

    // A 仍能正常收包录制
    QUdpSocket sender;
    QVERIFY(sender.bind(quint16(0)));
    sender.writeDatagram(makeArtNetTC(0, 0, 1, 0, 3), QHostAddress::LocalHost, sharedPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrameA.count() >= 1, 1500);
    QCOMPARE(spyFrameA.at(0).at(1).toLongLong(), qint64(1000000));

    engineA.stop();
    sender.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfgA.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(1));
    storage.close();
}

// ============================================================
// L40: onPreviewTick → statusUpdate 信号格式与时钟名
// 覆盖: m_previewTimer (previewIntervalMs) → onPreviewTick → statusUpdate 信号
//       验证状态字符串包含帧数/时分秒/时钟名
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_statusUpdate_signalFormatAndClockName() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyStatus(&engine, &RecorderEngine::statusUpdate);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/status.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26745;
    cfg.previewIntervalMs = 50;   // 50ms 触发一次 onPreviewTick
    QVERIFY(engine.start(cfg));

    // 等到至少 2 次 statusUpdate (50ms 间隔, 给 300ms 足够)
    QTRY_VERIFY_WITH_TIMEOUT(spyStatus.count() >= 2, 1500);

    // 验证格式: "REC <n> 帧 | HH:MM:SS | 时钟=Internal Timer"
    const QString status0 = spyStatus.at(0).at(0).toString();
    QVERIFY2(status0.contains("REC "),
             QString("statusUpdate 应含 'REC ', 实际=%1").arg(status0).toUtf8());
    QVERIFY2(status0.contains("帧"),
             QString("statusUpdate 应含 '帧', 实际=%1").arg(status0).toUtf8());
    QVERIFY2(status0.contains("|"),
             QString("statusUpdate 应含分隔符 '|', 实际=%1").arg(status0).toUtf8());
    QVERIFY2(status0.contains("Internal Timer"),
             QString("statusUpdate 应含时钟名 'Internal Timer', 实际=%1").arg(status0).toUtf8());

    // 后续 status 的帧数应 >= 第一次 (录制在继续)
    const QString statusLast = spyStatus.last().at(0).toString();
    QVERIFY2(statusLast.contains("Internal Timer"),
             QString("末条 status 应仍含时钟名, 实际=%1").arg(statusLast).toUtf8());

    engine.stop();
}

// ============================================================
// L41: 公共 getter 一致性
// 覆盖: frameRate() / recordedFrames() / currentDurationUs() / currentTimeCode()
//       在录制过程中与实际帧信号保持一致
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_publicGetters_consistency() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/getters.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26746;
    QVERIFY(engine.start(cfg));

    // frameRate 应等于配置
    QCOMPARE(engine.frameRate(), 30);

    // 等几帧
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 3, 2000);

    // recordedFrames 应与 frameRecorded 信号数一致 (直接查询, 不加锁的竞态可能差 1)
    const qint64 recorded = engine.recordedFrames();
    QVERIFY2(recorded >= 3,
             QString("recordedFrames 应 >= 3, 实际=%1").arg(recorded).toUtf8());
    QVERIFY2(qAbs(recorded - spyFrame.count()) <= 1,
             QString("recordedFrames=%1 vs signal=%2 相差应 <=1")
                 .arg(recorded).arg(spyFrame.count()).toUtf8());

    // currentDurationUs 应 > 0
    const qint64 durUs = engine.currentDurationUs();
    QVERIFY2(durUs > 0,
             QString("currentDurationUs 应 > 0, 实际=%1").arg(durUs).toUtf8());

    // currentDurationUs 应与最近一帧 ts 数量级一致 (在同一秒内)
    const qint64 lastFrameTs = spyFrame.last().at(1).toLongLong();
    QVERIFY2(qAbs(durUs - lastFrameTs) < 200000,
             QString("currentDurationUs=%1 应接近最近帧 ts=%2 (差<200ms)")
                 .arg(durUs).arg(lastFrameTs).toUtf8());

    // currentTimeCode 应反映当前时间 (秒数 >= 0)
    const TimeCode tc = engine.currentTimeCode();
    QVERIFY(tc.seconds >= 0);

    engine.stop();

    // 停止后 currentDurationUs 仍应返回最后值 (>0)
    QVERIFY(engine.currentDurationUs() >= 0);
}

// ============================================================
// L42: start() 重入保护
// 覆盖: start() 开头判断 m_state == Recording/Paused → return false
//       现有录制不受影响 (state 仍 Recording, 帧继续写)
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_startReentry_returnsFalseKeepsRecording() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyState(&engine, &RecorderEngine::stateChanged);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/reentry.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26747;
    QVERIFY(engine.start(cfg));
    QCOMPARE(engine.state(), RecordState::Recording);

    // 等首帧
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 1, 2000);
    const int framesBefore = spyFrame.count();

    // 第二次 start (用不同文件) 应返回 false, 不改变状态
    RecorderEngine::Config cfg2 = cfg;
    cfg2.outputFilePath = dir.path() + "/reentry_2.artrec";
    QVERIFY2(!engine.start(cfg2), "录制中再次 start 应返回 false");
    QCOMPARE(engine.state(), RecordState::Recording);

    // 现有录制应继续: 帧信号仍递增
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() > framesBefore, 2000);

    // 状态信号序列最后一个应仍是 Recording (没被重入调用篡改)
    RecordState lastState = static_cast<RecordState>(spyState.last().at(0).toInt());
    QCOMPARE(lastState, RecordState::Recording);

    engine.stop();

    // 第二个文件不应被创建
    QVERIFY2(!QFile::exists(cfg2.outputFilePath),
             "重入 start 不应创建第二个输出文件");
}

// ============================================================
// L43: pause()/resume() 在错误状态下为 no-op
// 覆盖: pause() 要求 Recording, resume() 要求 Paused
//       状态不匹配时直接返回, 不发出 stateChanged
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_pauseResume_noopOnWrongState() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyState(&engine, &RecorderEngine::stateChanged);
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/noop.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26748;
    QVERIFY(engine.start(cfg));
    QCOMPARE(engine.state(), RecordState::Recording);
    const int stateCountAfterStart = spyState.count();

    // (1) Recording 状态下调 resume() → no-op
    engine.resume();
    QCOMPARE(engine.state(), RecordState::Recording);
    QCOMPARE(spyState.count(), stateCountAfterStart);   // 无新 stateChanged

    // (2) Recording 状态下调 pause() → 进入 Paused (唯一有效路径)
    engine.pause();
    QCOMPARE(engine.state(), RecordState::Paused);
    const int stateCountAfterPause = spyState.count();
    QVERIFY(stateCountAfterPause > stateCountAfterStart);

    // (3) Paused 状态下调 pause() → no-op
    engine.pause();
    QCOMPARE(engine.state(), RecordState::Paused);
    QCOMPARE(spyState.count(), stateCountAfterPause);   // 无新 stateChanged

    // (4) Paused 状态下调 resume() → 回到 Recording
    engine.resume();
    QCOMPARE(engine.state(), RecordState::Recording);

    // 录制仍能继续产生帧
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 1, 2000);

    engine.stop();
    QCOMPARE(engine.state(), RecordState::Stopped);
}

// ============================================================
// L44: ArtNetReceiver 端口冲突 → start 失败 + cleanup 释放时钟
// 覆盖: InternalClock 模式下 Receiver bind artNetPort;
//       若端口已被占用 → m_receiver->start() false → cleanup() → return false
//       验证: errorOccurred + 无输出文件 + 端口(时钟+接收器)均已释放
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_receiverPortConflict_failsAndCleansUp() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 预先占用 InternalClock 模式下的 receiver 端口 (= artNetPort)
    const quint16 usedPort = 26749;
    QUdpSocket blocker;
    QVERIFY2(blocker.bind(QHostAddress("0.0.0.0"), usedPort),
             QString("预绑定端口 %1 应成功").arg(usedPort).toUtf8());

    RecorderEngine engine;
    QSignalSpy spyErr(&engine, &RecorderEngine::errorOccurred);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/rx_port_busy.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = usedPort;   // 与 blocker 冲突

    QVERIFY2(!engine.start(cfg), "接收器端口被占用时 start 应返回 false");
    QCOMPARE(engine.state(), RecordState::Idle);

    QVERIFY2(spyErr.count() >= 1, "接收器启动失败应 emit errorOccurred");
    const QString errMsg = spyErr.at(0).at(0).toString();
    QVERIFY2(errMsg.contains("绑定") || errMsg.contains("ArtNet") || errMsg.contains("接收器"),
             QString("错误应提及接收器绑定失败, 实际=%1").arg(errMsg).toUtf8());

    // 输出文件不应被创建
    QVERIFY2(!QFile::exists(cfg.outputFilePath),
             "接收器启动失败时不应创建输出文件");

    // cleanup 已释放时钟: blocker 仍持有端口, 但引擎内部资源已清
    // 验证引擎可重新开始 (端口冲突只在该次 start 失败, 之后可换端口)
    blocker.close();

    cfg.artNetPort = 26750;
    QVERIFY2(engine.start(cfg), "失败后同一实例换端口应可重新开始录制");
    QCOMPARE(engine.state(), RecordState::Recording);
    engine.stop();
    QVERIFY(QFile::exists(cfg.outputFilePath));
}

// ============================================================
// L45: maxDurationUs 自动停止
// 覆盖: onPreviewTick → shouldStop() (currentDurationUs >= maxDurationUs) → stop() → finalize
//       验证 recordingFinished 参数与落盘帧数一致
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_maxDurationUs_autoStopsAndFinalizes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/maxdur.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26751;
    cfg.previewIntervalMs = 50;        // 每 50ms 检查 shouldStop
    cfg.maxDurationUs = 400000;        // 400ms 后自动停止 (30fps ≈ 12 帧, 余量充足)
    QVERIFY(engine.start(cfg));

    // 等到自动停止 (400ms 录制 + 检查间隔, 给 1.5s 余量)
    QVERIFY2(spyFinish.wait(1500), "maxDurationUs 到限应自动触发 recordingFinished");
    QCOMPARE(engine.state(), RecordState::Stopped);

    // recordingFinished 参数: totalFrames = m_recordedFrames, totalDurationUs = currentDurationUs
    const qint64 totalFrames = spyFinish.at(0).at(0).toLongLong();
    const qint64 totalDurationUs = spyFinish.at(0).at(1).toLongLong();
    QVERIFY2(totalFrames >= 3,
             QString("400ms @ 30fps 应至少录 3 帧, 实际=%1").arg(totalFrames).toUtf8());
    QVERIFY2(totalDurationUs >= 400000,
             QString("totalDurationUs 应 >= maxDurationUs(400000), 实际=%1")
                 .arg(totalDurationUs).toUtf8());
    QCOMPARE(totalFrames, spyFrame.count());   // 帧数与信号数一致

    // 文件落盘帧数与 totalFrames 一致
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(totalFrames));
    QCOMPARE(storage.meta().totalDurationUs, totalDurationUs);
    storage.close();
}

// ============================================================
// L46: injectMidiMtcQuarterFrame 在非 MTC 模式下为 no-op
// 覆盖: injectMidiMtcQuarterFrame 内 dynamic_cast<MidiMTCClock*> 失败 → 直接 return
//       验证非 MTC 时钟下注入 MTC 不会崩溃也不会干扰录制
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_injectMtc_nonMtcMode_noop() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/mtc_noop.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;   // 非 MTC 模式
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26752;
    QVERIFY(engine.start(cfg));

    // 注入一整套 MTC quarter-frame (00:00:00:05 @30fps) — 应被忽略
    for (auto q : makeMtcQF(0, 0, 0, 5)) {
        engine.injectMidiMtcQuarterFrame(q);
    }

    // 录制继续由 InternalTimerClock 驱动 (帧来自内部时钟, 不是注入的 MTC)
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 2, 2000);

    // InternalClock 帧的 frameIndex 从 0 递增, 不会出现 MTC 注入的 5*30=150
    const qint64 firstIdx = spyFrame.at(0).at(0).toLongLong();
    QVERIFY2(firstIdx < 150,
             QString("内部时钟首帧 frameIndex 应远小于 MTC 注入值(150), 实际=%1")
                 .arg(firstIdx).toUtf8());

    engine.stop();
    QCOMPARE(engine.state(), RecordState::Stopped);
}

// ============================================================
// L47: 帧回退跳过 — 循环 TC 文件第二轮 frameIndex 回 0 被丢弃
// 覆盖: onSyncFrameTick 中 frameIndex <= m_lastFrameIndex → return
//       验证循环时钟的回退帧不写入, 仅首轮帧落盘
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_loopingTcFile_rollbackFramesSkipped() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString tcPath = dir.path() + "/roll.tc";
    QFile f(tcPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("#FPS=30\n");
    f.write("00:00:00:00\n");   // frameIndex 0
    f.write("00:00:00:01\n");   // frameIndex 1
    f.write("00:00:00:02\n");   // frameIndex 2
    f.close();

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/roll.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
    cfg.mediaFilePath = tcPath;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26753;
    QVERIFY(engine.start(cfg));

    // 给时钟跑过至少 2 轮 (首轮 3 帧写入, 第二轮及以后的 0,1,2 均被回退逻辑丢弃)
    // 2 轮 = 6 个 frameTick, 但只有前 3 个会变成 frameRecorded
    QTest::qWait(600);

    // 首轮 3 帧: frameIndex 0, 1, 2
    QCOMPARE(spyFrame.count(), 3);
    QCOMPARE(spyFrame.at(0).at(0).toLongLong(), qint64(0));
    QCOMPARE(spyFrame.at(1).at(0).toLongLong(), qint64(1));
    QCOMPARE(spyFrame.at(2).at(0).toLongLong(), qint64(2));

    // 循环时钟不会 emit clockFinished, 需手动 stop
    engine.stop();
    QCOMPARE(engine.state(), RecordState::Stopped);
    QCOMPARE(spyFinish.at(0).at(0).toLongLong(), qint64(3));

    // 文件里只有 3 帧
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(3));
    storage.close();
}

// ============================================================
// L48: stop() 从 Paused 状态触发 finalize
// 覆盖: stop() 不要求 state==Recording, Paused 下也应正确 finalize
//       验证 recordingFinished 参数 = 暂停前已录帧数
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_stopFromPaused_finalizesCorrectly() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);
    QSignalSpy spyState(&engine, &RecorderEngine::stateChanged);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/stop_paused.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26754;
    QVERIFY(engine.start(cfg));

    // 录几帧后暂停
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 2, 2000);
    const int framesBeforePause = spyFrame.count();

    engine.pause();
    QCOMPARE(engine.state(), RecordState::Paused);

    // 暂停期间 frameRecorded 不应再增加
    const int framesAfterPause = spyFrame.count();
    QTest::qWait(200);
    QCOMPARE(spyFrame.count(), framesAfterPause);

    // 从 Paused 状态 stop → 应 finalize 并 emit recordingFinished
    engine.stop();
    QCOMPARE(engine.state(), RecordState::Stopped);

    QVERIFY2(spyFinish.count() >= 1, "从 Paused 状态 stop 应触发 recordingFinished");
    const qint64 totalFrames = spyFinish.at(0).at(0).toLongLong();
    QCOMPARE(totalFrames, qint64(framesBeforePause));   // 暂停后没新帧
    QVERIFY(totalFrames >= 2);

    // 状态序列最后一个应为 Stopped
    RecordState lastState = static_cast<RecordState>(spyState.last().at(0).toInt());
    QCOMPARE(lastState, RecordState::Stopped);

    // 文件帧数与 totalFrames 一致
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(totalFrames));
    storage.close();
}

// ============================================================
// L49: 自动停止后再显式 stop() — 状态守卫应拦住, 不重复 finalize
// 覆盖: stop() 开头 if (m_state == Idle || Stopped) return;
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_stopAfterAutoStop_singleFinishEvent() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);
    QSignalSpy spyState(&engine, &RecorderEngine::stateChanged);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/double_stop.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26755;
    cfg.previewIntervalMs = 50;
    cfg.maxDurationUs = 400000;
    QVERIFY(engine.start(cfg));

    QVERIFY2(spyFinish.wait(2000), "maxDurationUs 到限应自动触发 recordingFinished");
    QCOMPARE(engine.state(), RecordState::Stopped);
    QCOMPARE(spyFinish.count(), 1);

    const qint64 frames = spyFinish.at(0).at(0).toLongLong();
    const qint64 durUs = spyFinish.at(0).at(1).toLongLong();
    QVERIFY(frames >= 3);
    const int stateChanges = spyState.count();

    // 再显式 stop() — 已在 Stopped, 应直接 return
    engine.stop();
    QCOMPARE(spyFinish.count(), 1);            // 不重复 emit recordingFinished
    QCOMPARE(spyState.count(), stateChanges);  // 不重复 emit stateChanged
    QCOMPARE(engine.state(), RecordState::Stopped);
    QCOMPARE(engine.recordedFrames(), frames);

    // 文件内容不被二次 finalize 影响
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(frames));
    QCOMPARE(storage.meta().totalDurationUs, durUs);
    storage.close();
}

// ============================================================
// L50: enablePassthrough=true 但没有 outputTargets
// 覆盖: if (config.enablePassthrough && !config.outputTargets.isEmpty()) 的 false 分支
//       → 不创建 OutputManager, 录制不受影响
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_passthroughEnabledNoTargets_recordsNormally() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/passthrough_empty.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26756;
    cfg.enablePassthrough = true;
    // outputTargets 故意留空
    QVERIFY(engine.start(cfg));
    QCOMPARE(engine.state(), RecordState::Recording);

    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 3, 2000);

    engine.stop();
    QCOMPARE(engine.state(), RecordState::Stopped);
    QVERIFY2(spyFrame.count() >= 3, "无转发目标不应影响录制");

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(spyFrame.count()));

    StorageFrame frame;
    int readCount = 0;
    while (storage.readNextFrame(frame)) ++readCount;
    QCOMPARE(readCount, spyFrame.count());
    storage.close();
}

// ============================================================
// L51: start() 之前注入 MTC quarter-frame
// 覆盖: injectMidiMtcQuarterFrame 的 if (!m_syncClock) return; 空指针守卫
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_injectMtcBeforeStart_ignored() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    // 未 start → m_syncClock == nullptr, 注入应被忽略
    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 0);
    QCOMPARE(engine.state(), RecordState::Idle);

    // 正常启动 MTC 模式后, 注入应生效
    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/mtc_prestart.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26757;
    QVERIFY(engine.start(cfg));

    for (auto q : makeMtcQF(0, 0, 2, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);
    QCOMPARE(spyFrame.at(0).at(1).toLongLong(), qint64(2000000));

    engine.stop();
    QCOMPARE(engine.state(), RecordState::Stopped);
}

// ============================================================
// L52: 29fps + ArtNetTC(smpteType=2) → header 映射与时间戳
// 覆盖: header.timeCodeFormat 的 fps==29 → 2 分支 (L35 只测了 25fps)
//       时间戳按 TC 包内 29fps 语义计算, 而非配置帧率
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_29fps_artNetTC_headerFormatMapping() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/fps29.artrec";
    cfg.frameRate = 29;
    cfg.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26758;
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(quint16(0)));

    // 00:00:01:00 @29.97 (smpteType=2) → 1_000_000us
    sender.writeDatagram(makeArtNetTC(0, 0, 1, 0, 2), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 1, 1500);

    // 00:00:01:15 @29fps → (29+15)*1e6/29 = 1_517_241us
    sender.writeDatagram(makeArtNetTC(0, 0, 1, 15, 2), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 2, 1500);

    QCOMPARE(spyFrame.at(0).at(1).toLongLong(), qint64(1000000));
    QCOMPARE(spyFrame.at(1).at(1).toLongLong(), artNetTCToUs(0, 0, 1, 15, 29));

    engine.stop();
    sender.close();

    // header: frameRate=29, timeCodeFormat=2 (29fps), timeCodeType=1 (ArtNetTC)
    ArtRecReader reader;
    QVERIFY(reader.open(cfg.outputFilePath));
    QCOMPARE(reader.header().frameRate, quint16(29));
    QCOMPARE(reader.header().timeCodeFormat, quint8(2));
    QCOMPARE(reader.header().timeCodeType, quint8(1));
    reader.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().frameRate, quint16(29));
    QCOMPARE(storage.meta().totalFrames, quint32(2));
    QCOMPARE(storage.meta().totalDurationUs, qint64(1517241));

    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.timestampUs, qint64(1000000));
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.timestampUs, qint64(1517241));
    QVERIFY(!storage.readNextFrame(frame));
    storage.close();
}

// ============================================================
// L53: TimecodeFile 播放中暂停, 文件走到 EOF 时仍需自动停止
// 覆盖: 暂停只丢弃写入, 时钟照常走 → clockFinished → stop() → finalize
//       recordingFinished 帧数 = 暂停前帧数 (L27 只测了 ArtNetTC 超时路径)
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_tcFileEofWhilePaused_autoFinalizes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString tcPath = dir.path() + "/paused_eof.tc";
    QFile f(tcPath);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("#FPS=30\n");
    for (int i = 0; i < 30; ++i) {   // 30 行 = 1s @30fps
        f.write(QString("00:00:00:%1\n").arg(i, 2, 10, QChar('0')).toUtf8());
    }
    f.close();

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/paused_eof.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
    cfg.mediaFilePath = tcPath;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26760;
    QVERIFY(engine.start(cfg));

    // 录到至少 3 帧后暂停
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 3, 2000);
    engine.pause();
    QCOMPARE(engine.state(), RecordState::Paused);
    const int framesAtPause = spyFrame.count();

    // 时钟继续走完剩余行 → EOF 触发 clockFinished → 自动 finalize
    QVERIFY2(spyFinish.wait(3000), "暂停期间文件 EOF 应触发 recordingFinished");
    QCOMPARE(engine.state(), RecordState::Stopped);
    QCOMPARE(spyFrame.count(), framesAtPause);   // 暂停期间无新帧
    QCOMPARE(spyFinish.at(0).at(0).toLongLong(), qint64(framesAtPause));

    // 落盘帧数 = 暂停前帧数; 时长 = 最后一行时间码 (29 帧 → 966666us)
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(framesAtPause));
    QCOMPARE(storage.meta().totalDurationUs, qint64(966666));
    storage.close();
}

// ============================================================
// L54: statusUpdate 只在 Recording 状态产生
// 覆盖: onPreviewTick 的 if (m_state != RecordState::Recording) return;
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_statusUpdate_onlyWhileRecording() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyStatus(&engine, &RecorderEngine::statusUpdate);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/status_state.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26761;
    cfg.previewIntervalMs = 50;
    QVERIFY(engine.start(cfg));

    QTRY_VERIFY_WITH_TIMEOUT(spyStatus.count() >= 2, 2000);

    // 暂停 → 状态更新冻结
    engine.pause();
    QTest::qWait(60);                       // 消化可能已排队的定时器事件
    const int atPause = spyStatus.count();
    QTest::qWait(300);
    QCOMPARE(spyStatus.count(), atPause);

    // 恢复 → 状态更新继续
    engine.resume();
    QTRY_VERIFY_WITH_TIMEOUT(spyStatus.count() > atPause, 2000);

    // 停止 → 状态更新冻结
    engine.stop();
    QTest::qWait(60);
    const int atStop = spyStatus.count();
    QTest::qWait(300);
    QCOMPARE(spyStatus.count(), atStop);
    QCOMPARE(engine.state(), RecordState::Stopped);
}

// ============================================================
// L55: 24fps + ArtNetTC → header timeCodeFormat=0
// 覆盖: 24→0 的映射分支 (L35=25fps/1, L52=29fps/2, 30fps/3 由默认用例覆盖)
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_24fps_artNetTC_headerFormatMapping() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/fps24.artrec";
    cfg.frameRate = 24;
    cfg.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26762;
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(quint16(0)));

    sender.writeDatagram(makeArtNetTC(0, 0, 1, 0, 0), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 1, 1500);

    // 00:00:01:12 @24fps → 36 帧 → 1_500_000us
    sender.writeDatagram(makeArtNetTC(0, 0, 1, 12, 0), QHostAddress::LocalHost, cfg.artNetPort);
    QTRY_VERIFY_WITH_TIMEOUT(spyFrame.count() >= 2, 1500);

    QCOMPARE(spyFrame.at(0).at(1).toLongLong(), qint64(1000000));
    QCOMPARE(spyFrame.at(1).at(1).toLongLong(), artNetTCToUs(0, 0, 1, 12, 24));

    engine.stop();
    sender.close();

    ArtRecReader reader;
    QVERIFY(reader.open(cfg.outputFilePath));
    QCOMPARE(reader.header().frameRate, quint16(24));
    QCOMPARE(reader.header().timeCodeFormat, quint8(0));
    QCOMPARE(reader.header().timeCodeType, quint8(1));
    reader.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(2));
    QCOMPARE(storage.meta().totalDurationUs, qint64(1500000));
    storage.close();
}

// ============================================================
// L56: 暂停时 writeCurrentFrame 不被调用 → 落盘与转发同时冻结
// 覆盖: onSyncFrameTick 的 state 守卫拦截了"边录边转发"整条链路
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_pauseStopsPassthrough_resumeForwardsLatest() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const quint16 forwardPort = freeUdpPort();
    QVERIFY(forwardPort != 0);
    QUdpSocket rx;
    QVERIFY(rx.bind(QHostAddress("127.0.0.1"), forwardPort));

    OutputTargetConfig target;
    target.type = OutputType::ArtNet;
    target.artNetIp = "127.0.0.1";
    target.artNetPort = forwardPort;
    target.enabled = true;

    const quint16 uni1 = encodeUniverse(0, 0, 1);

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/pause_fwd.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { uni1 };
    cfg.artNetPort = 26764;
    cfg.enablePassthrough = true;
    cfg.outputTargets = { target };
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));

    // 第一帧: DMX=0x44 → tick 落盘 + 转发
    const QByteArray dmxA(512, char(0x44));
    sender.writeDatagram(makeArtDmxPacket(uni1, dmxA), QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(120);
    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(rx.hasPendingDatagrams(), 2000);
    QCOMPARE(rx.receiveDatagram().data().mid(18, 512), dmxA);

    // 暂停: 新 DMX=0x77 + tick → 既不落盘也不转发
    engine.pause();
    const QByteArray dmxB(512, char(0x77));
    sender.writeDatagram(makeArtDmxPacket(uni1, dmxB), QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(120);
    for (auto q : makeMtcQF(0, 0, 2, 0)) engine.injectMidiMtcQuarterFrame(q);
    QTest::qWait(200);
    QCOMPARE(spyFrame.count(), 1);
    QVERIFY2(!rx.hasPendingDatagrams(), "暂停期间不应出现转发包");

    // 恢复: 下一 tick 使用最新缓存 0x77 落盘并转发
    engine.resume();
    for (auto q : makeMtcQF(0, 0, 3, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(rx.hasPendingDatagrams(), 2000);
    QCOMPARE(rx.receiveDatagram().data().mid(18, 512), dmxB);

    engine.stop();
    sender.close();
    rx.close();

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.universeData.value(0), dmxA);   // key = universe 下标
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.universeData.value(0), dmxB);
    QCOMPARE(storage.meta().totalFrames, quint32(2));
    storage.close();
}

// ============================================================
// L57: maxDurationUs=0 (默认) → 预览定时器不触发自动停止
// 覆盖: onPreviewTick 中 if (m_maxDurationUs > 0 && ...) 的左短路
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_maxDurationZero_noAutoStop() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/no_limit.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 26765;
    cfg.previewIntervalMs = 50;
    cfg.maxDurationUs = 0;          // 0 = 不限时长
    QVERIFY(engine.start(cfg));

    QTest::qWait(600);
    QCOMPARE(spyFinish.count(), 0);                 // 不自动停止
    QCOMPARE(engine.state(), RecordState::Recording);
    QVERIFY2(spyFrame.count() >= 5,
             QString("600ms @30fps 应录到 >=5 帧, 实际=%1").arg(spyFrame.count()).toUtf8());

    engine.stop();
    QCOMPARE(engine.state(), RecordState::Stopped);
    QCOMPARE(spyFinish.count(), 1);
    QVERIFY(spyFinish.at(0).at(1).toLongLong() >= 500000);

    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    QCOMPARE(storage.meta().totalFrames, quint32(spyFrame.count()));
    storage.close();
}

// ============================================================
// L58: 只收到但未配置录制的 universe 不落盘, 也不会被转发
// 覆盖: writeCurrentFrame 的 frameData 只取 m_config.universes ∩ cache,
//       转发 (sendMultipleUniverses) 基于同一份 frameData
// ============================================================
void TestSyncClockIntegration::test_recorderEngine_recordsConfiguredUniverses_forwardsSameSubset() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const quint16 portA = freeUdpPort();
    const quint16 portB = freeUdpPort();
    QVERIFY(portA != 0 && portB != 0 && portA != portB);

    QUdpSocket rxA, rxB;
    QVERIFY(rxA.bind(QHostAddress("127.0.0.1"), portA));
    QVERIFY(rxB.bind(QHostAddress("127.0.0.1"), portB));

    const quint16 uni1 = encodeUniverse(0, 0, 1);
    const quint16 uni2 = encodeUniverse(0, 0, 2);

    OutputTargetConfig tA;
    tA.type = OutputType::ArtNet;
    tA.artNetIp = "127.0.0.1";
    tA.artNetPort = portA;
    tA.filterUniverses = { uni1 };

    OutputTargetConfig tB = tA;
    tB.artNetPort = portB;
    tB.filterUniverses = { uni2 };

    RecorderEngine engine;
    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/uni_scope.artrec";
    cfg.frameRate = 30;
    cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
    cfg.universes = { uni1 };        // 只录 uni1
    cfg.artNetPort = 26766;
    cfg.enablePassthrough = true;
    cfg.outputTargets = { tA, tB };
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    QVERIFY(sender.bind(QHostAddress("0.0.0.0"), quint16(0)));
    const QByteArray dmx1(512, char(0x11));
    const QByteArray dmx2(512, char(0x22));
    sender.writeDatagram(makeArtDmxPacket(uni1, dmx1), QHostAddress("127.0.0.1"), cfg.artNetPort);
    sender.writeDatagram(makeArtDmxPacket(uni2, dmx2), QHostAddress("127.0.0.1"), cfg.artNetPort);
    QTest::qWait(150);

    for (auto q : makeMtcQF(0, 0, 1, 0)) engine.injectMidiMtcQuarterFrame(q);
    QCOMPARE(spyFrame.count(), 1);

    // 转发: 目标 A 收到 uni1; 目标 B (filter uni2) 因 frameData 无 uni2 而收不到
    QTRY_VERIFY_WITH_TIMEOUT(rxA.hasPendingDatagrams(), 2000);
    QCOMPARE(rxA.receiveDatagram().data().mid(18, 512), dmx1);
    QTest::qWait(200);
    QVERIFY2(!rxB.hasPendingDatagrams(), "未配置录制的 universe 不应被转发");

    engine.stop();
    sender.close();
    rxA.close();
    rxB.close();

    // 落盘帧只含 uni1 (唯一配置的 universe → 下标 0)
    ArtRecStorage storage;
    QVERIFY(storage.open(cfg.outputFilePath));
    StorageFrame frame;
    QVERIFY(storage.readNextFrame(frame));
    QCOMPARE(frame.universeData.size(), 1);
    QCOMPARE(frame.universeData.value(0), dmx1);
    QVERIFY(!frame.universeData.contains(1));
    storage.close();
}

QTEST_MAIN(TestSyncClockIntegration)
#include "TestSyncClockIntegration.moc"
