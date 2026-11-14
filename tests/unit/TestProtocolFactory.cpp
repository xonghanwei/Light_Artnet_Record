// TestProtocolFactory.cpp - 协议工厂测试 (Phase 1)
//
// 测试范围: ProtocolFactory + ProtocolType + protocolTypeToString + ArtRecFileSource 完整重演
// 依赖: Qt Core + src/protocol/ProtocolFactory.h + src/fileformat
//
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QCoreApplication>
#include <QTest>

#include "core/Common.h"
#include "protocol/ProtocolFactory.h"
#include "protocol/ProtocolTypes.h"
#include "fileformat/ArtRecWriter.h"
#include "fileformat/ArtRecReader.h"
#include "fileformat/ArtRecTypes.h"
#include <cstring>

using namespace ArtNet;

// ===== helper: 写一个测试用 .artrec 文件 =====
static void writeTestArtrec(const QString &path,
                            quint16 fps = 30,
                            quint32 frameCount = 10,
                            quint16 universe1Enc = 0x001,
                            quint16 universe2Enc = 0x002) {
    struct ArtRecHeader hdr;
    hdr.reset();
    hdr.frameRate = fps;
    hdr.universeCount = 2;
    hdr.totalFrames = frameCount;
    hdr.totalDurationUs = (frameCount - 1) * quint64(1000000ULL / fps);
    
    QList<UniverseEntry> ues;
    ues.append(UniverseEntry(universe1Enc, 512));
    ues.append(UniverseEntry(universe2Enc, 512));
    
    ArtRecWriter w;
    QVERIFY(w.create(path, hdr, ues));
    
    for(quint32 fi = 0; fi < frameCount; ++fi) {
        qint64 ts = qint64(fi) * 1000000LL / fps;
        
        QMap<quint16, QByteArray> data;
        QByteArray d1(512, char(int(fi + 1) & 0xFF));
        QByteArray d2(512, char(int(100 - fi) & 0xFF));
        data[universe1Enc] = d1;
        data[universe2Enc] = d2;
        
        w.writeFrame(qint64(fi), ts, data, true);
    }
    
    w.finalize(frameCount, hdr.totalDurationUs);
    w.close();
}

class TestProtocolFactory : public QObject {
    Q_OBJECT

private slots:
    // ===== protocolTypeToString =====
    void test_protocolTypeToString() {
        QCOMPARE(protocolTypeToString(ProtocolType::ArtNet), QString("ArtNet"));
        QCOMPARE(protocolTypeToString(ProtocolType::Npu), QString("NPU (Ma-Net2)"));
        QCOMPARE(protocolTypeToString(ProtocolType::Dmx512), QString("DMX512"));
        QCOMPARE(protocolTypeToString(ProtocolType::ArtRecFile), QString("ArtRec File (离线录制)"));
        QCOMPARE(protocolTypeToString(ProtocolType::Sacn), QString("sACN"));
        QCOMPARE(protocolTypeToString(ProtocolType::OSC), QString("OSC"));
        QCOMPARE(protocolTypeToString(ProtocolType::Unknown), QString("Unknown"));
    }
    
    // ===== nameToType =====
    void test_nameToType() {
        // ArtNet 变体
        QCOMPARE(ProtocolFactory::nameToType("artnet"), ProtocolType::ArtNet);
        QCOMPARE(ProtocolFactory::nameToType("ArtNet"), ProtocolType::ArtNet);
        QCOMPARE(ProtocolFactory::nameToType("ARTNET"), ProtocolType::ArtNet);
        QCOMPARE(ProtocolFactory::nameToType("art-net"), ProtocolType::ArtNet);
        QCOMPARE(ProtocolFactory::nameToType("art_net"), ProtocolType::ArtNet);
        
        // NPU 变体
        QCOMPARE(ProtocolFactory::nameToType("npu"), ProtocolType::Npu);
        QCOMPARE(ProtocolFactory::nameToType("NPU"), ProtocolType::Npu);
        QCOMPARE(ProtocolFactory::nameToType("ma"), ProtocolType::Npu);
        QCOMPARE(ProtocolFactory::nameToType("ma-net"), ProtocolType::Npu);
        QCOMPARE(ProtocolFactory::nameToType("manet2"), ProtocolType::Npu);
        
        // DMX512 变体
        QCOMPARE(ProtocolFactory::nameToType("dmx512"), ProtocolType::Dmx512);
        QCOMPARE(ProtocolFactory::nameToType("DMX512"), ProtocolType::Dmx512);
        QCOMPARE(ProtocolFactory::nameToType("dmx"), ProtocolType::Dmx512);
        
        // ArtRecFile 变体 ← 新增
        QCOMPARE(ProtocolFactory::nameToType("artrecfile"), ProtocolType::ArtRecFile);
        QCOMPARE(ProtocolFactory::nameToType("ArtRecFile"), ProtocolType::ArtRecFile);
        QCOMPARE(ProtocolFactory::nameToType("artrec"), ProtocolType::ArtRecFile);
        QCOMPARE(ProtocolFactory::nameToType("ARTREC"), ProtocolType::ArtRecFile);
        QCOMPARE(ProtocolFactory::nameToType("file"), ProtocolType::ArtRecFile);
        QCOMPARE(ProtocolFactory::nameToType("recording"), ProtocolType::ArtRecFile);
        
        // sACN 变体
        QCOMPARE(ProtocolFactory::nameToType("sacn"), ProtocolType::Sacn);
        QCOMPARE(ProtocolFactory::nameToType("e1.31"), ProtocolType::Sacn);
        
        // OSC
        QCOMPARE(ProtocolFactory::nameToType("osc"), ProtocolType::OSC);
        
        // 未知
        QCOMPARE(ProtocolFactory::nameToType("blah"), ProtocolType::Unknown);
        QCOMPARE(ProtocolFactory::nameToType(""), ProtocolType::Unknown);
        QCOMPARE(ProtocolFactory::nameToType("  "), ProtocolType::Unknown);
    }
    
    // ===== createSource (按枚举) =====
    void test_createSource_byType() {
        IProtocolSource *s = ProtocolFactory::createSource(ProtocolType::ArtNet);
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::ArtNet);
        QCOMPARE(s->protocolName(), QString("ArtNet"));
        delete s;
        
        s = ProtocolFactory::createSource(ProtocolType::Npu);
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::Npu);
        delete s;
        
        s = ProtocolFactory::createSource(ProtocolType::Dmx512);
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::Dmx512);
        delete s;
        
        s = ProtocolFactory::createSource(ProtocolType::ArtRecFile);
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::ArtRecFile);
        delete s;
        
        // 未实现的协议 → nullptr
        s = ProtocolFactory::createSource(ProtocolType::Sacn);
        QVERIFY(s == nullptr);
        
        s = ProtocolFactory::createSource(ProtocolType::OSC);
        QVERIFY(s == nullptr);
        
        s = ProtocolFactory::createSource(ProtocolType::Unknown);
        QVERIFY(s == nullptr);
    }
    
    // ===== createSource (按字符串) =====
    void test_createSource_byName() {
        IProtocolSource *s = ProtocolFactory::createSource("artnet");
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::ArtNet);
        delete s;
        
        s = ProtocolFactory::createSource("npu");
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::Npu);
        delete s;
        
        s = ProtocolFactory::createSource("dmx512");
        QVERIFY(s != nullptr);
        delete s;
        
        // ArtRecFile ← 新增
        s = ProtocolFactory::createSource("artrecfile");
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::ArtRecFile);
        delete s;
        
        s = ProtocolFactory::createSource("artrec");
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::ArtRecFile);
        delete s;
        
        s = ProtocolFactory::createSource("unknown");
        QVERIFY(s == nullptr);
    }
    
    // ===== createSink (按枚举) =====
    void test_createSink_byType() {
        IProtocolSink *s = ProtocolFactory::createSink(ProtocolType::ArtNet);
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::ArtNet);
        delete s;
        
        s = ProtocolFactory::createSink(ProtocolType::Npu);
        QVERIFY(s != nullptr);
        QCOMPARE(s->protocolType(), ProtocolType::Npu);
        delete s;
        
        s = ProtocolFactory::createSink(ProtocolType::Dmx512);
        QVERIFY(s != nullptr);
        delete s;
        
        // ArtRecFile 没有 Sink → nullptr ← 新增
        s = ProtocolFactory::createSink(ProtocolType::ArtRecFile);
        QVERIFY(s == nullptr);
        
        s = ProtocolFactory::createSink(ProtocolType::Sacn);
        QVERIFY(s == nullptr);
    }
    
    // ===== supportedProtocols =====
    void test_supportedProtocols() {
        QStringList list = ProtocolFactory::supportedProtocols();
        QVERIFY(list.size() >= 4);  // 现在至少 4 个协议
        QVERIFY(list.contains("artnet"));
        QVERIFY(list.contains("npu"));
        QVERIFY(list.contains("dmx512"));
        QVERIFY(list.contains("artrecfile"));  // ← 新增
    }
    
    // ===== NpuProtocol 常量 =====
    void test_npuProtocolConstants() {
        QCOMPARE(NpuProtocol::DEFAULT_PORT, quint16(9999));
        QCOMPARE(NpuProtocol::OP_DMX, quint8(0x01));
        QCOMPARE(NpuProtocol::DMX_PACKET_SIZE, 2 + 1 + 1 + 512);  // 516
    }
    
    // ===== IProtocolSource/Sink 接口基础契约 =====
    void test_source_lifecycle() {
        IProtocolSource *s = ProtocolFactory::createSource(ProtocolType::ArtNet);
        QVERIFY(s != nullptr);
        
        ConnectionParams params;
        params.protocol = ProtocolType::ArtNet;
        params.localPort = 0;  // 随机端口, 避免冲突
        
        // start → isRunning → stop → isRunning == false
        bool started = s->start(params);
        QVERIFY(started);
        QVERIFY(s->isRunning());
        s->stop();
        QVERIFY(!s->isRunning());
        
        delete s;
    }
    
    void test_sink_lifecycle() {
        IProtocolSink *s = ProtocolFactory::createSink(ProtocolType::ArtNet);
        QVERIFY(s != nullptr);
        
        ConnectionParams params;
        params.protocol = ProtocolType::ArtNet;
        params.localPort = 0;
        
        bool started = s->start(params);
        QVERIFY(started);
        QVERIFY(s->isRunning());
        s->stop();
        QVERIFY(!s->isRunning());
        
        delete s;
    }
    
    // ===== ArtRecFileSource 功能性测试 =====
    
    // 1) 无 filePath → start 返回 false
    void test_artrecFile_noFilePath() {
        auto *src = ProtocolFactory::createSource(ProtocolType::ArtRecFile);
        QVERIFY(src);
        
        ConnectionParams p;
        p.playbackSpeed = 10.0;
        
        QSignalSpy spyErr(src, &IProtocolSource::errorOccurred);
        QVERIFY(!src->start(p));
        QCOMPARE(spyErr.count(), 1);
        QVERIFY(!src->isRunning());
        
        delete src;
    }
    
    // 2) 不存在的文件 → start 返回 false
    void test_artrecFile_fileNotFound() {
        auto *src = ProtocolFactory::createSource(ProtocolType::ArtRecFile);
        QVERIFY(src);
        
        ConnectionParams p;
        p.filePath = "C:/definitely_not_exist_12345.artrec";
        p.playbackSpeed = 100.0;
        
        QVERIFY(!src->start(p));
        QVERIFY(!src->isRunning());
        
        delete src;
    }
    
    // 3) 正常重演: 写 10 帧 × 2 universe, 用 100x 速度, 应吐出 20 个 LightFrame
    void test_artrecFile_normalPlayback() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + "/norm.artrec";
        
        writeTestArtrec(path, /*fps=*/30, /*frames=*/10);
        
        auto *src = ProtocolFactory::createSource(ProtocolType::ArtRecFile);
        QVERIFY(src);
        
        ConnectionParams p;
        p.filePath = path;
        p.playbackSpeed = 100.0;   // interval ~1ms, 10帧 → ~10ms
        
        QSignalSpy spy(src, &IProtocolSource::frameReceived);
        QVERIFY(src->start(p));
        QVERIFY(src->isRunning());
        
        // QTimer 间隔 ~1ms, 10 tick 应该很快吐完
        // 必须用 QTest::qWait (跑 event loop), 不能用 QThread::msleep (阻塞 event loop)
        QTest::qWait(200);
        
        src->stop();
        
        const int count = spy.count();
        QVERIFY2(count == 20, QString("10帧×2Universe = 20 LightFrame, 实际=%1").arg(count).toUtf8());
        
        // 检查每个 LightFrame 的 sourceProtocol 和关键属性
        QSet<quint16> seenUniverses;
        for(int i = 0; i < count; ++i) {
            LightFrame lf = spy.at(i).first().value<LightFrame>();
            QCOMPARE(lf.sourceProtocol, ProtocolType::ArtRecFile);
            QVERIFY(lf.dmxData.size() == 512);
            seenUniverses.insert(lf.universe);
        }
        
        // 应该看到 2 个不同的 universe
        QVERIFY2(seenUniverses.size() == 2, QString("应看到 2 个 universe, 实际=%1").arg(seenUniverses.size()).toUtf8());
        
        delete src;
    }
    
    // 4) Universe 过滤: 写 2 universe, 只看其中一个
    void test_artrecFile_universeFilter() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + "/filt.artrec";
        
        writeTestArtrec(path, 30, 10);
        
        auto *src = ProtocolFactory::createSource(ProtocolType::ArtRecFile);
        QVERIFY(src);
        
        ConnectionParams p;
        p.filePath = path;
        p.playbackSpeed = 100.0;
        p.filterUniverses = { quint16(0x001) };  // 只看第一个 universe (encoded 0x001)
        
        QSignalSpy spy(src, &IProtocolSource::frameReceived);
        QVERIFY(src->start(p));
        
        QTest::qWait(200);
        src->stop();
        
        const int count = spy.count();
        QCOMPARE(count, 10);  // 10 帧 × 1 个 universe
        
        // 所有 LightFrame 的 universe 都应该是 0x001
        for(int i = 0; i < count; ++i) {
            LightFrame lf = spy.at(i).first().value<LightFrame>();
            QCOMPARE(lf.universe, quint16(0x001));
        }
        
        delete src;
    }
    
    // 5) Loop 重演: 写 2 帧, loop=true, 等够长时间应吐出 >4 个帧
    void test_artrecFile_loopPlayback() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + "/loop.artrec";
        
        writeTestArtrec(path, 30, 2);  // 只 2 帧, 2 universe = 4 LightFrame per loop
        
        auto *src = ProtocolFactory::createSource(ProtocolType::ArtRecFile);
        QVERIFY(src);
        
        ConnectionParams p;
        p.filePath = path;
        p.playbackSpeed = 100.0;
        p.loopPlayback = true;
        
        QSignalSpy spy(src, &IProtocolSource::frameReceived);
        QVERIFY(src->start(p));
        QVERIFY(src->isRunning());
        
        // QTest::qWait 跑 event loop, QTimer 才能吐帧
        QTest::qWait(500);
        
        int countAfterFirst = spy.count();
        QVERIFY2(countAfterFirst > 4, QString("至少第一轮 >=4, 实际=%1").arg(countAfterFirst).toUtf8());
        
        // 再等一下, loop 应继续
        QTest::qWait(300);
        int countAfterMore = spy.count();
        QVERIFY2(countAfterMore > countAfterFirst,
                 QString("loop 应继续: before=%1 after=%2")
                     .arg(countAfterFirst).arg(countAfterMore).toUtf8());
        
        src->stop();
        delete src;
    }
};

QTEST_MAIN(TestProtocolFactory)
#include "TestProtocolFactory.moc"
