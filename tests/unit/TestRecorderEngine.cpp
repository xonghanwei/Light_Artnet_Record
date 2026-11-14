// TestRecorderEngine.cpp - RecorderEngine 端到端集成测试 (Phase 5)
// 精简版: 先验证基础启停 + 状态机, 再加 UDP 自环
#include <QtTest>
#include <QUdpSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <cstring>
#include <QDateTime>

#include "core/Common.h"
#include "recorder/RecorderEngine.h"
#include "fileformat/ArtRecReader.h"
#include "sync/SyncTypes.h"
#include "sync/SyncClockFactory.h"
#include "sync/clocks/MidiMTCClock.h"
#include "storage/StorageFactory.h"
#include "storage/IStorageFormat.h"
#include "storage/StorageTypes.h"
#include <QTcpSocket>
#include <QUdpSocket>
#include <QSignalSpy>
#include <QTemporaryFile>

using namespace ArtNet;

class TestRecorderEngine : public QObject {
    Q_OBJECT
private:
    QTemporaryDir *m_dir = nullptr;

private slots:
    void initTestCase() {
        m_dir = new QTemporaryDir();
        QVERIFY(m_dir->isValid());
    }
    void cleanupTestCase() { delete m_dir; }

    // ===== 测试 1: 基本启停 (不发任何 DMX) =====
    void test_startStop() {
        RecorderEngine engine;
        QSignalSpy spyState(&engine, &RecorderEngine::stateChanged);

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/t1.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
        cfg.universes = { encodeUniverse(0, 0, 0) };
        cfg.artNetPort = 26671;
        cfg.previewIntervalMs = 100;

        QVERIFY(engine.start(cfg));
        QCOMPARE(engine.state(), RecordState::Recording);
        engine.stop();
        QCOMPARE(engine.state(), RecordState::Stopped);
        QVERIFY(spyState.count() >= 1);
    }

    // ===== 测试 2: 状态机完整路径 =====
    void test_stateMachine() {
        RecorderEngine engine;
        QCOMPARE(engine.state(), RecordState::Idle);

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/t2.artrec";
        cfg.frameRate = 30;
        cfg.universes = { encodeUniverse(0, 0, 0) };
        cfg.artNetPort = 26672;

        QVERIFY(engine.start(cfg));
        QCOMPARE(engine.state(), RecordState::Recording);

        engine.pause();
        QCOMPARE(engine.state(), RecordState::Paused);

        engine.resume();
        QCOMPARE(engine.state(), RecordState::Recording);

        engine.stop();
        QCOMPARE(engine.state(), RecordState::Stopped);

        // 停止后再 stop 不应 crash
        engine.stop();
    }

    // ===== 测试 3: 端口被占用 =====
    void test_portBusy() {
        RecorderEngine e1, e2;
        RecorderEngine::Config cfg;
        cfg.frameRate = 30;
        cfg.universes = { encodeUniverse(0, 0, 0) };
        cfg.artNetPort = 26673;
        cfg.outputFilePath = m_dir->path() + "/pb1.artrec";
        QVERIFY(e1.start(cfg));

        cfg.outputFilePath = m_dir->path() + "/pb2.artrec";
        QVERIFY2(!e2.start(cfg), "端口占用应返回 false");

        e1.stop();
    }

    // ===== 测试 4: UDP 自环端到端 =====
    void test_udpRoundTrip() {
        RecorderEngine engine;
        QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/rt.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 26674;
        cfg.previewIntervalMs = 30; // 快点触发

        QVERIFY(engine.start(cfg));

        // 发 ArtNet ArtDMX 包
        QUdpSocket sender;
        sender.bind();
        quint16 testUni = encodeUniverse(0, 0, 1);

        QByteArray dmx(512, 0);
        dmx[0] = 100; dmx[1] = 200;

        // 快速发 N 帧 (UDP 不保证顺序, 多发几个)
        for (int i = 0; i < 10; i++) {
            QByteArray pkt(530, 0);
            std::memcpy(pkt.data(), "Art-Net\0", 8);
            pkt[8] = 0x00; pkt[9] = 0x50; pkt[10] = 14;
            pkt[12] = quint8(i + 1);
            pkt[14] = quint8(testUni & 0xFF);
            pkt[15] = quint8((testUni >> 8) & 0xFF);
            pkt[16] = quint8(512 & 0xFF);
            pkt[17] = quint8((512 >> 8) & 0xFF);
            std::memcpy(pkt.data() + 18, dmx.constData(), 512);
            sender.writeDatagram(pkt, QHostAddress("127.0.0.1"), 26674);
        }

        // 让 UDP 数据先到达 receiver cache (事件循环处理 readyRead)
        // 然后 timer tick 时 cache 里就有数据了
        QVERIFY2(spyFrame.wait(2000), "frameRecorded 应在 2s 内触发");
        qint64 frames = engine.recordedFrames();
        QVERIFY2(frames >= 1, QString("至少 1 帧, 实际 %1").arg(frames).toStdString().c_str());

        // 等更多帧录完 (让 timer tick 多轮, 确保最后一帧有正确 DMX)
        QThread::msleep(150);
        qApp->processEvents();

        engine.stop();

        // 读文件验证 —— 遍历所有帧找正确数据
        ArtRecReader reader;
        QVERIFY(reader.open(cfg.outputFilePath));
        QCOMPARE(reader.header().frameRate, quint16(30));
        QCOMPARE(reader.header().universeCount, quint8(1));

        qint64 fi, tu;
        QMap<int, QByteArray> frame;
        bool found = false;
        int frameCount = 0;
        while (reader.readNextFrame(fi, tu, frame)) {
            frameCount++;
            for (auto it = frame.constBegin(); it != frame.constEnd(); ++it) {
                QByteArray data = it.value();
                qDebug() << "frame[" << fi << "] key=" << it.key() 
                         << "size=" << data.size()
                         << "data[0]=" << (data.size() > 0 ? quint8(data[0]) : 0)
                         << "data[1]=" << (data.size() > 1 ? quint8(data[1]) : 0);
                if (data.size() >= 2 && quint8(data[0]) == 100 && quint8(data[1]) == 200) {
                    found = true;
                }
            }
        }
        qDebug() << "Total frames read:" << frameCount;
        QVERIFY2(found, "应找到 DMX 数据正确的帧");
        reader.close();
    }

    // ===== 测试 5: Universe 过滤 =====
    void test_universeFilter() {
        RecorderEngine engine;
        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/uf.artrec";
        cfg.frameRate = 30;
        cfg.universes = { encodeUniverse(0, 0, 1) }; // 只录 uni1
        cfg.artNetPort = 26675;
        cfg.previewIntervalMs = 30;

        QVERIFY(engine.start(cfg));

        QUdpSocket sender;
        sender.bind();
        quint16 uni0 = encodeUniverse(0, 0, 0);
        quint16 uni1 = encodeUniverse(0, 0, 1);

        // 发 uni0 (应被过滤) + uni1 (应被录)
        auto send = [&](quint16 u, quint8 v) {
            QByteArray pkt(530, 0);
            std::memcpy(pkt.data(), "Art-Net\0", 8);
            pkt[8] = 0; pkt[9] = 0x50; pkt[10] = 14;
            pkt[14] = quint8(u & 0xFF);
            pkt[15] = quint8((u >> 8) & 0xFF);
            pkt[16] = quint8(512 & 0xFF);
            pkt[17] = quint8((512 >> 8) & 0xFF);
            QByteArray d(512, v);
            std::memcpy(pkt.data() + 18, d.constData(), 512);
            sender.writeDatagram(pkt, QHostAddress("127.0.0.1"), 26675);
        };
        for (int i = 0; i < 10; i++) {
            send(uni0, 55);
            send(uni1, 77);
        }

        // 用事件循环等待 ~500ms (不能用 msleep 阻塞 timer!)
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 500) {
            qApp->processEvents();
            QThread::msleep(10);
        }

        engine.stop();

        ArtRecReader reader;
        QVERIFY(reader.open(cfg.outputFilePath));
        QCOMPARE(reader.header().universeCount, quint8(1));

        qint64 fi, tu;
        QMap<int, QByteArray> frame;
        bool found77 = false;
        while (reader.readNextFrame(fi, tu, frame)) {
            QByteArray data = frame.value(0);
            if (data.size() >= 1 && quint8(data[0]) == 77) {
                found77 = true;
                break;
            }
        }
        QVERIFY2(found77, "应找到 uni1 的 DMX 数据 (0x4D=77)");
        reader.close();
    }

    // ==================== v1.3 新增: ISyncClock 集成测试 ====================

    // ---- 测试 6: SyncSource → SyncClockType 枚举映射 ----
    void test_syncSourceToClockType() {
        // 基本映射
        QCOMPARE(RecorderEngine::syncSourceToClockType(
                     RecorderEngine::SyncSource::InternalClock),
                 SyncClockType::InternalTimer);
        QCOMPARE(RecorderEngine::syncSourceToClockType(
                     RecorderEngine::SyncSource::ArtNetTimeCode),
                 SyncClockType::ArtNetTimeCode);
        QCOMPARE(RecorderEngine::syncSourceToClockType(
                     RecorderEngine::SyncSource::MidiMTC),
                 SyncClockType::MidiMTC);
        QCOMPARE(RecorderEngine::syncSourceToClockType(
                     RecorderEngine::SyncSource::TimecodeFile),
                 SyncClockType::TimecodeFile);
        QCOMPARE(RecorderEngine::syncSourceToClockType(
                     RecorderEngine::SyncSource::VideoFile),
                 SyncClockType::VideoFile);
        QCOMPARE(RecorderEngine::syncSourceToClockType(
                     RecorderEngine::SyncSource::AudioFile),
                 SyncClockType::AudioFile);

        // 每个 mapped type 在 SyncClockFactory 里都能 create (除了 VideoFile/AudioFile 没 FFmpeg)
        auto *cInt = SyncClockFactory::instance().create(SyncClockType::InternalTimer);
        QVERIFY(cInt != nullptr); delete cInt;
        auto *cArt = SyncClockFactory::instance().create(SyncClockType::ArtNetTimeCode);
        QVERIFY(cArt != nullptr); delete cArt;
        auto *cMtc = SyncClockFactory::instance().create(SyncClockType::MidiMTC);
        QVERIFY(cMtc != nullptr); delete cMtc;
        auto *cTc  = SyncClockFactory::instance().create(SyncClockType::TimecodeFile);
        QVERIFY(cTc != nullptr); delete cTc;
    }

    // ---- 测试 7: InternalClock 模式 → ISyncClock 驱动写帧 (验证 refactor 后仍工作) ----
    void test_internalClock_writesFrames() {
        RecorderEngine engine;

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/v13_int.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 26680;

        QVERIFY(engine.start(cfg));
        // 用 qWait 等足够多帧 (1000ms × 30fps ≈ 30 帧, 远超过 5)
        QTest::qWait(1200);
        engine.stop();

        qint64 frames = engine.recordedFrames();
        QVERIFY2(frames >= 10, QString("30fps 录 1.2s 应该 >= 10 帧, 实际=%1").arg(frames).toUtf8());

        // 读文件验证
        ArtRecReader reader;
        QVERIFY(reader.open(cfg.outputFilePath));
        QCOMPARE(reader.header().frameRate, quint16(30));
        QVERIFY(reader.header().totalFrames > 0);
        reader.close();
    }

    // ---- 测试 8: ArtNetTimeCode 模式 — 发 opcode 0x0D 包 → RecorderEngine 写帧 ----
    void test_artNetTC_mode_drivesRecording() {
        RecorderEngine engine;

        // 先用 UDP 探测一个空闲端口给 ArtNetTimeCodeClock
        QUdpSocket probe;
        QVERIFY(probe.bind(quint16(0)));
        quint16 port = probe.localPort();
        probe.close();

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/v13_arttc.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::ArtNetTimeCode;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = port;

        QVERIFY(engine.start(cfg));

        // 构造 ArtNet TimeCode 数据包 (opcode 0x0D00)
        auto makeArtNetTC = [](quint8 h, quint8 m, quint8 s, quint8 f) -> QByteArray {
            QByteArray pkt(13, char(0));
            quint8 *d = reinterpret_cast<quint8*>(pkt.data());
            d[0] = 0x00; d[1] = 0x0D;  // opcode 0x0D00
            d[2] = 0x00; d[3] = 0x01;   // protocol version
            d[4] = 0x00; d[5] = 0x00;
            d[6] = (3 << 4);            // smpte type 3 = 30fps
            d[7] = h & 0x3F;
            d[8] = m & 0x3F;
            d[9] = s & 0x3F;
            d[10] = f & 0x1F;
            return pkt;
        };

        // 发 ArtNetTC 包 (递增时间) — 发多组让 ArtNetTimeCodeClock emit 多个 frameTick
        QUdpSocket sender;
        QVERIFY(sender.bind(quint16(0)));
        for (int f = 0; f < 20; ++f) {
            QByteArray pkt = makeArtNetTC(0, 0, 1, quint8(f % 30));
            sender.writeDatagram(pkt, QHostAddress::LocalHost, port);
            QTest::qWait(15);  // 给 ArtNetTimeCodeClock 处理时间
        }

        // 等足够多帧
        QTest::qWait(500);
        engine.stop();

        qint64 frames = engine.recordedFrames();
        QVERIFY2(frames >= 5, QString("ArtNetTC 模式应录至少 5 帧, 实际=%1").arg(frames).toUtf8());
    }

    // ---- 测试 9: TimecodeFile 模式 — .tc 文件逐行驱动写帧 ----
    void test_timecodeFile_mode() {
        // 构造 .tc 文件 — 加大文件避免 clockFinished 太早触发
        QString tcPath = m_dir->path() + "/v13_tcfile.tc";
        QFile tf(tcPath);
        QVERIFY(tf.open(QIODevice::WriteOnly | QIODevice::Text));
        tf.write("#FPS=30\n");
        for (int i = 0; i < 30; ++i) {
            tf.write(QString("00:00:00:%1\n").arg(i, 2, 10, QChar('0')).toUtf8());
        }
        tf.close();

        RecorderEngine engine;

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/v13_tcfile.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
        cfg.mediaFilePath = tcPath;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 26682;

        QSignalSpy spyFinish(&engine, &RecorderEngine::recordingFinished);
        QVERIFY(engine.start(cfg));

        // TimecodeFileClock 每帧 emit 间隔 ~33ms, 30 帧 ≈ 1s
        // 等 clockFinished 自动触发 engine.stop() — 固定 qWait 在系统负载下会不足
        QVERIFY2(spyFinish.wait(4000), "TimecodeFile 30 帧读完应触发 recordingFinished");

        qint64 frames = engine.recordedFrames();
        QVERIFY2(frames >= 25, QString("TimecodeFile 30帧文件应录 >= 25 帧, 实际=%1").arg(frames).toUtf8());

        // 验证时间戳精确性 — 第 i 帧 ts ≈ i × 33333 (30fps)
        ArtRecReader reader;
        QVERIFY(reader.open(cfg.outputFilePath));
        qint64 prevTs = -1;
        qint64 fi, tu;
        QMap<int, QByteArray> frame;
        int count = 0;
        while (reader.readNextFrame(fi, tu, frame)) {
            if (prevTs >= 0) {
                qint64 expectedStep = 1000000LL / 30;  // 33333
                qint64 diff = tu - prevTs;
                QVERIFY2(qAbs(diff - expectedStep) <= 2000,
                         QString("帧 %1: ts step=%2 expected=%3").arg(count).arg(diff).arg(expectedStep).toUtf8());
            }
            prevTs = tu;
            count++;
        }
        reader.close();
        QVERIFY2(count >= 25, QString("文件应读回 >= 25 帧, 实际=%1").arg(count).toUtf8());
    }

    // ---- 测试 10: pause/resume 不影响时钟 (ISyncClock 继续跑) ----
    void test_pauseResume_doesNotPauseClock() {
        RecorderEngine engine;
        QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/v13_pr.artrec";
        cfg.frameRate = 100;  // 快 fps, 更容易观测
        cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 26683;

        QVERIFY(engine.start(cfg));

        // 先录一批帧
        QTest::qWait(100);
        qint64 beforePause = engine.recordedFrames();
        QVERIFY2(beforePause >= 3, QString("pause 前应至少 3 帧, 实际=%1").arg(beforePause).toUtf8());

        // pause — 此时不应再录新帧
        engine.pause();
        QCOMPARE(engine.state(), RecordState::Paused);

        // 暂停期间等够久 (ISyncClock 还在跑, 但 frameTick 被 state check 挡掉)
        QThread::msleep(150);
        qApp->processEvents();
        qint64 duringPause = engine.recordedFrames();
        QVERIFY2(duringPause <= beforePause,
                 QString("pause 期间不应有新帧, before=%1 during=%2").arg(beforePause).arg(duringPause).toUtf8());

        // resume — 继续录
        engine.resume();
        QCOMPARE(engine.state(), RecordState::Recording);
        QTest::qWait(100);
        qint64 afterResume = engine.recordedFrames();
        QVERIFY2(afterResume > duringPause,
                 QString("resume 后应有新帧, during=%1 after=%2").arg(duringPause).arg(afterResume).toUtf8());

        engine.stop();
    }

    // ---- 测试 11: TimecodeFile loopOnEnd=true → clockLooped 信号 ----
    void test_tcFile_loopSignal() {
        QString tcPath = m_dir->path() + "/v13_loop.tc";
        QFile tf(tcPath);
        QVERIFY(tf.open(QIODevice::WriteOnly | QIODevice::Text));
        tf.write("#FPS=30\n");
        for (int i = 0; i < 3; ++i) {
            tf.write(QString("00:00:00:%1\n").arg(i, 2, 10, QChar('0')).toUtf8());
        }
        tf.close();

        // 直接测 TimecodeFileClock 的 loop 行为 (通过 SyncClockFactory)
        auto *clk = SyncClockFactory::instance().create(SyncClockType::TimecodeFile);
        QVERIFY(clk != nullptr);
        QSignalSpy spyLoop(clk, &ISyncClock::clockLooped);

        SyncClockConfig cfg;
        cfg.clockType = SyncClockType::TimecodeFile;
        cfg.frameRate = 30;
        cfg.mediaFilePath = tcPath;
        cfg.loopOnEnd = true;

        QVERIFY(clk->start(cfg));

        // 等至少一个 loop (3 帧 @30fps = 100ms per loop)
        QVERIFY2(spyLoop.wait(2000), "loopOnEnd=true 应 emit clockLooped");
        clk->stop();
        delete clk;
    }

    // ---- 测试 12: RecorderEngine → Storage 跨格式管道 (artrec→json→xml 时间戳保真) ----
    void test_recorder_crossFormat_preservesTimestamps() {
        // Step 1: RecorderEngine 录 artrec (InternalTimerClock 驱动)
        QString artrecPath = m_dir->path() + "/pipeline.artrec";
        RecorderEngine engine;
        RecorderEngine::Config cfg;
        cfg.outputFilePath = artrecPath;
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 26690;

        QVERIFY(engine.start(cfg));
        QTest::qWait(1200);  // 录 1.2s ≈ 36 帧
        engine.stop();
        qint64 frames = engine.recordedFrames();
        QVERIFY2(frames >= 15, QString("应录 >= 15 帧, 实际=%1").arg(frames).toUtf8());

        // Step 2: 用 StorageFactory 读 artrec → 写 json
        QString jsonPath = m_dir->path() + "/pipeline.json";
        auto *readerArtRec = StorageFactory::instance().createByFilePath(artrecPath);
        QVERIFY(readerArtRec != nullptr);
        QVERIFY(readerArtRec->open(artrecPath));

        StorageMeta meta = readerArtRec->meta();
        QList<StorageUniverse> universes = readerArtRec->universes();

        auto *writerJson = StorageFactory::instance().createByFilePath(jsonPath);
        QVERIFY(writerJson != nullptr);
        QVERIFY(writerJson->create(jsonPath, meta, universes));

        StorageFrame fr;
        int countArtRec = 0;
        qint64 sumTsArtRec = 0;
        while (readerArtRec->readNextFrame(fr)) {
            sumTsArtRec += fr.timestampUs;
            countArtRec++;
            QVERIFY(writerJson->writeFrame(fr));
        }
        writerJson->finalize(countArtRec, sumTsArtRec, "");
        delete readerArtRec;
        delete writerJson;
        QVERIFY2(countArtRec >= 15, QString("artrec 应读回 >= 15 帧, 实际=%1").arg(countArtRec).toUtf8());

        // Step 3: 读 json → 写 xml
        QString xmlPath = m_dir->path() + "/pipeline.xml";
        auto *readerJson = StorageFactory::instance().createByFilePath(jsonPath);
        QVERIFY(readerJson != nullptr);
        QVERIFY(readerJson->open(jsonPath));

        StorageMeta metaJson = readerJson->meta();
        QList<StorageUniverse> universesJson = readerJson->universes();

        auto *writerXml = StorageFactory::instance().createByFilePath(xmlPath);
        QVERIFY(writerXml != nullptr);
        QVERIFY(writerXml->create(xmlPath, metaJson, universesJson));

        int countJson = 0;
        qint64 sumTsJson = 0;
        while (readerJson->readNextFrame(fr)) {
            sumTsJson += fr.timestampUs;
            countJson++;
            QVERIFY(writerXml->writeFrame(fr));
        }
        writerXml->finalize(countJson, sumTsJson, "");
        delete readerJson;
        delete writerXml;
        QVERIFY2(countJson == countArtRec,
                 QString("json 帧数应 = artrec 帧数, artrec=%1 json=%2")
                     .arg(countArtRec).arg(countJson).toUtf8());

        // Step 4: 验证 xml → 时间戳链路保真
        auto *readerXml = StorageFactory::instance().createByFilePath(xmlPath);
        QVERIFY(readerXml != nullptr);
        QVERIFY(readerXml->open(xmlPath));

        int countXml = 0;
        qint64 sumTsXml = 0;
        qint64 prevTs = -1;
        while (readerXml->readNextFrame(fr)) {
            sumTsXml += fr.timestampUs;
            countXml++;
            if (prevTs >= 0) {
                QVERIFY2(fr.timestampUs > prevTs,
                         QString("xml 帧 %1 ts=%2 应 > prevTs=%3")
                             .arg(countXml).arg(fr.timestampUs).arg(prevTs).toUtf8());
            }
            prevTs = fr.timestampUs;
        }
        delete readerXml;

        QVERIFY2(countXml == countArtRec,
                 QString("xml 帧数应 = artrec 帧数, artrec=%1 xml=%2")
                     .arg(countArtRec).arg(countXml).toUtf8());

        // 全链路时间戳总和应相等 (允许极小差异 due to 舍入)
        qint64 diff = qAbs(sumTsXml - sumTsArtRec);
        QVERIFY2(diff <= 1000 * countArtRec,  // 每帧容差 1ms
                 QString("跨格式 ts 总和差=%1 (每帧容差 1ms), sumArtRec=%2 sumXml=%3")
                     .arg(diff).arg(sumTsArtRec).arg(sumTsXml).toUtf8());
    }

    // ---- 测试 13: RecorderEngine 错误路径: ArtNetReceiver bind 失败 ----
    void test_recorder_error_artNetBindFail() {
        // 先用 QUdpSocket 占住一个 0.0.0.0 端口 (和 ArtNetReceiver 相同 bind 地址)
        QUdpSocket blocker;
        QVERIFY(blocker.bind(QHostAddress("0.0.0.0"), quint16(0)));
        quint16 blockedPort = blocker.localPort();

        RecorderEngine engine;
        QSignalSpy spyErr(&engine, &RecorderEngine::errorOccurred);

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/err.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = blockedPort;

        QVERIFY2(!engine.start(cfg), QString("端口 %1 被占时 start 应返回 false").arg(blockedPort).toUtf8());
        QVERIFY2(spyErr.count() >= 1, "start 失败应 emit errorOccurred");
        QVERIFY2(spyErr.at(0).at(0).toString().contains("绑定"),
                 QString("错误消息应提到绑定, 实际=%1")
                     .arg(spyErr.at(0).at(0).toString()).toUtf8());
    }

    // ---- 测试 14: MidiMTC 模式驱动 RecorderEngine 录制 (端到端集成) ----
    void test_midiMTC_mode_drivesRecording() {
        RecorderEngine engine;
        QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/v13_mtc.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::MidiMTC;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 26684;

        QVERIFY(engine.start(cfg));

        // 复用 TestSyncClockIntegration 里的正确 quarter-frame 构造逻辑
        auto makeMtcQF = [](quint8 hours, quint8 minutes, quint8 seconds, quint8 frames) -> QList<quint8> {
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
        };

        // 喂 5 组 quarter-frame (每组 8 个 → 一帧完整时间码)
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
                engine.injectMidiMtcQuarterFrame(q);
            }
        }

        // 等所有 frameTick 被处理 (MidiMTCClock 的 timeoutTimer 是 2s, 所以要快)
        QTest::qWait(300);
        engine.stop();

        // 验证: QSignalSpy 应收到 5 个 frameRecorded 信号
        QCOMPARE(spyFrame.count(), 5);
        QVERIFY2(engine.recordedFrames() >= 5,
                 QString("MidiMTC 模式应录至少 5 帧, 实际=%1").arg(engine.recordedFrames()).toUtf8());
    }

    // ---- 测试 15: TimecodeFile 不存在 → SyncClock start 失败 → RecorderEngine start 失败 ----
    void test_timecodeFile_notFound_errorPath() {
        RecorderEngine engine;
        QSignalSpy spyErr(&engine, &RecorderEngine::errorOccurred);

        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/err_tcfile.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::TimecodeFile;
        cfg.mediaFilePath = m_dir->path() + "/nonexistent.tc";  // 不存在的文件
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 26685;

        QVERIFY2(!engine.start(cfg), "不存在的 TimecodeFile 应导致 start 返回 false");
        QVERIFY2(spyErr.count() >= 1, "SyncClock 启动失败应 emit errorOccurred");
        // 错误消息应包含时钟类型描述
        QVERIFY2(spyErr.at(0).at(0).toString().contains("Timecode") ||
                 spyErr.at(0).at(0).toString().contains("同步时钟"),
                 QString("错误消息应提到时钟/Timecode, 实际=%1")
                     .arg(spyErr.at(0).at(0).toString()).toUtf8());
    }
};

QTEST_MAIN(TestRecorderEngine)
#include "TestRecorderEngine.moc"
