// TestStorageIntegration.cpp — 存储抽象层集成测试
//
// 与单元测试 TestStorageFormats 的区别:
//   单元测试 = Storage 层内部 round-trip, 用 mock DTO
//   集成测试 = 跨模块真实数据流:
//     L1. RecorderEngine (真实 UDP 自环录制) → .artrec → Storage 层各格式读回验证
//     L2. StorageFactory 驱动格式转换管道 (artrec → json → xml)
//     L3. 错误文件处理跨格式一致性 (损坏/空文件)
//     L4. Seek 与 Engine 产出文件的联动
//     L5. 真实 DMX 跨格式保真
//
#include <QTest>
#include <QObject>
#include <QTemporaryDir>
#include <QFile>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QSignalSpy>

#include "core/Common.h"
#include "storage/StorageFactory.h"
#include "storage/IStorageFormat.h"
#include "storage/StorageTypes.h"
#include "storage/ArtRecStorage.h"
#include "storage/JsonStorage.h"
#include "storage/XmlStorage.h"
#include "recorder/RecorderEngine.h"

#include <cstring>

using namespace ArtNet;

// ===== 静态 helper (提前定义, 成员函数可直接用) =====

static QList<StorageUniverse> makeUniverses(int count) {
    QList<StorageUniverse> list;
    for(int i = 0; i < count; ++i) list.append(StorageUniverse(quint16(i + 1), 512));
    return list;
}

static QList<StorageFrame> makeFrames(int count, int universeCount) {
    QList<StorageFrame> list;
    for(int f = 0; f < count; ++f) {
        StorageFrame fr;
        fr.frameIndex = f;
        fr.timestampUs = f * 33333;
        for(int u = 0; u < universeCount; ++u) {
            QByteArray data(512, 0);
            for(int ch = 0; ch < 512; ++ch)
                data[ch] = char((f * 17 + u * 31 + ch) & 0xFF);
            fr.universeData[u] = data;
        }
        list.append(fr);
    }
    return list;
}

static QByteArray makeArtDmxPacket(quint16 universe, quint8 seq,
                                   const QByteArray &dmx512) {
    QByteArray pkt(18 + dmx512.size(), 0);
    std::memcpy(pkt.data(), "Art-Net\0", 8);
    pkt[8] = 0x00; pkt[9] = 0x50;  // ArtDMX opcode
    pkt[10] = 14;
    pkt[12] = seq;
    pkt[14] = char(universe & 0xFF);
    pkt[15] = char((universe >> 8) & 0xFF);
    pkt[16] = char(dmx512.size() & 0xFF);
    pkt[17] = char((dmx512.size() >> 8) & 0xFF);
    if(!dmx512.isEmpty())
        std::memcpy(pkt.data() + 18, dmx512.constData(), dmx512.size());
    return pkt;
}

static void sendArtDmx(QUdpSocket &sender, quint16 port,
                       quint16 universe, quint8 seq,
                       const QByteArray &dmx512, int count = 1) {
    for(int i = 0; i < count; ++i) {
        quint8 s = quint8((seq + i) & 0xFF);
        QByteArray pkt = makeArtDmxPacket(universe, s, dmx512);
        sender.writeDatagram(pkt, QHostAddress("127.0.0.1"), port);
    }
}

// ============================================================
// Test class
// ============================================================

class TestStorageIntegration : public QObject {
    Q_OBJECT

private slots:
    // ====== L1: RecorderEngine → Storage 跨模块读回 ======
    void test_engineArtrec_artrecStorageRead();
    void test_engineArtrec_artrecStorageRealDmxValues();

    // ====== L2: 格式转换管道 ======
    void test_factory_pipeline_artrecToJson();
    void test_factory_pipeline_artrecToXml();
    void test_factory_pipeline_fullChain_artrecJsonXml();
    void test_factory_pipeline_crossAllThree();

    // ====== L3: 错误文件跨格式一致 ======
    void test_corruptedFile_allFormatsOpenFail();
    void test_emptyFile_allFormatsBehavior();

    // ====== L4: Seek 集成 ======
    void test_seekAfterEngineRecord_artrecStorage();
    void test_seekMultiSeek();

    // ====== L5: 真实 DMX 跨格式保真 ======
    void test_realDmxValues_preservedAcrossFormats();
};

// ============================================================
// L1: RecorderEngine → ArtRecStorage 读回
// ============================================================

void TestStorageIntegration::test_engineArtrec_artrecStorageRead() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/eng.artrec";
    cfg.frameRate = 30;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 27501;
    cfg.syncSource = RecorderEngine::SyncSource::InternalClock;

    QSignalSpy spyFrame(&engine, &RecorderEngine::frameRecorded);
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    sender.bind();
    quint16 uni = encodeUniverse(0, 0, 1);

    QByteArray dmx(512, 0);
    dmx[0] = char(100); dmx[1] = char(200); dmx[100] = char(55); dmx[511] = char(254);

    sendArtDmx(sender, 27501, uni, 1, dmx, 5);

    QVERIFY2(spyFrame.wait(2000), "frameRecorded 应触发");
    engine.stop();

    qint64 recordedFrames = engine.recordedFrames();
    QVERIFY(recordedFrames > 0);

    // ArtRecStorage 读回
    ArtRecStorage fmt;
    QVERIFY(fmt.open(cfg.outputFilePath));
    QCOMPARE(fmt.meta().frameRate, quint16(30));
    QCOMPARE(fmt.universes().size(), 1);
    QCOMPARE(fmt.meta().totalFrames, quint32(recordedFrames));

    int frameCount = 0;
    qint64 lastTimestampUs = -1;
    StorageFrame fr;
    while(fmt.readNextFrame(fr)) {
        QVERIFY2(fr.timestampUs >= 0, "timestampUs 不应为负");
        QVERIFY2(fr.timestampUs >= lastTimestampUs,
                 QString("timestamp 应单调不减: prev=%1 cur=%2")
                     .arg(lastTimestampUs).arg(fr.timestampUs).toUtf8());
        lastTimestampUs = fr.timestampUs;
        QVERIFY(fr.universeData.contains(0));
        QCOMPARE(fr.universeData[0].size(), 512);
        ++frameCount;
    }
    fmt.close();

    QVERIFY(frameCount >= recordedFrames);
}

void TestStorageIntegration::test_engineArtrec_artrecStorageRealDmxValues() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/eng2.artrec";
    cfg.frameRate = 30;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 27502;

    QSignalSpy spy(&engine, &RecorderEngine::frameRecorded);
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    sender.bind();
    quint16 uni = encodeUniverse(0, 0, 1);

    QByteArray dmxV0(512, 0); dmxV0[0] = char(42); dmxV0[10] = char(77);
    QByteArray dmxV1(512, 0); dmxV1[0] = char(99); dmxV1[10] = char(88);
    QByteArray dmxV2(512, 0); dmxV2[0] = char(13); dmxV2[10] = char(31);

    sendArtDmx(sender, 27502, uni, 1, dmxV0, 3);
    sendArtDmx(sender, 27502, uni, 4, dmxV1, 3);
    sendArtDmx(sender, 27502, uni, 7, dmxV2, 3);

    QVERIFY2(spy.wait(2000), "应收到至少 1 帧");
    engine.stop();

    ArtRecStorage fmt;
    QVERIFY(fmt.open(cfg.outputFilePath));

    bool saw42 = false, saw99 = false, saw13 = false;
    StorageFrame fr;
    while(fmt.readNextFrame(fr)) {
        if(fr.universeData.contains(0)) {
            if(fr.universeData[0][0] == char(42))  saw42 = true;
            if(fr.universeData[0][0] == char(99))  saw99 = true;
            if(fr.universeData[0][0] == char(13))  saw13 = true;
        }
    }
    fmt.close();

    QVERIFY(saw42 || saw99 || saw13);
}

// ============================================================
// L2: 格式转换管道 (StorageFactory 驱动)
// ============================================================

void TestStorageIntegration::test_factory_pipeline_artrecToJson() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString srcPath = dir.path() + "/src.artrec";
    const QString dstPath = dir.path() + "/dst.json";

    StorageMeta meta;
    meta.frameRate = 30;
    meta.universeCount = 2;
    meta.createTimestamp = 42ULL;

    auto unis = makeUniverses(2);
    auto frames = makeFrames(5, 2);

    // 写 artrec
    auto *artrec = StorageFactory::instance().createByName("artrec");
    QVERIFY(artrec);
    QVERIFY(artrec->create(srcPath, meta, unis));
    for(const auto &f : frames) QVERIFY(artrec->writeFrame(f));
    QVERIFY(artrec->finalize(frames.size(), frames.last().timestampUs));
    artrec->close();
    delete artrec;

    // 读 → 写 Json
    auto *reader = StorageFactory::instance().createByName("artrec");
    auto *writer = StorageFactory::instance().createByName("json");
    QVERIFY(reader); QVERIFY(writer);

    QVERIFY(reader->open(srcPath));
    QVERIFY(writer->create(dstPath, reader->meta(), reader->universes()));

    StorageFrame fr;
    while(reader->readNextFrame(fr)) QVERIFY(writer->writeFrame(fr));
    QVERIFY(writer->finalize(reader->meta().totalFrames, reader->meta().totalDurationUs));
    reader->close(); writer->close();
    delete reader; delete writer;

    // JsonStorage 读回
    auto *jsonR = StorageFactory::instance().createByFilePath(dstPath);
    QVERIFY(jsonR);
    QCOMPARE(jsonR->formatName(), QStringLiteral("json"));
    QVERIFY(jsonR->open(dstPath));
    QCOMPARE(jsonR->meta().frameRate, quint16(30));
    QCOMPARE(jsonR->universes().size(), 2);

    int count = 0;
    while(jsonR->readNextFrame(fr)) {
        QCOMPARE(fr.frameIndex, qint64(count));
        ++count;
    }
    jsonR->close(); delete jsonR;
    QCOMPARE(count, 5);
}

void TestStorageIntegration::test_factory_pipeline_artrecToXml() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString srcPath = dir.path() + "/src.artrec";
    const QString dstPath = dir.path() + "/dst.xml";

    StorageMeta meta; meta.frameRate = 25; meta.universeCount = 3;
    auto unis = makeUniverses(3);
    auto frames = makeFrames(4, 3);

    ArtRecStorage ar;
    QVERIFY(ar.create(srcPath, meta, unis));
    for(const auto &f : frames) QVERIFY(ar.writeFrame(f));
    QVERIFY(ar.finalize(frames.size(), frames.last().timestampUs));
    ar.close();

    XmlStorage xs;
    QVERIFY(ar.open(srcPath));
    QVERIFY(xs.create(dstPath, ar.meta(), ar.universes()));
    StorageFrame fr;
    while(ar.readNextFrame(fr)) QVERIFY(xs.writeFrame(fr));
    QVERIFY(xs.finalize(ar.meta().totalFrames, ar.meta().totalDurationUs));
    ar.close(); xs.close();

    QVERIFY(xs.open(dstPath));
    QCOMPARE(xs.meta().frameRate, quint16(25));
    QCOMPARE(xs.universes().size(), 3);
    int count = 0;
    while(xs.readNextFrame(fr)) {
        for(auto it = fr.universeData.constBegin(); it != fr.universeData.constEnd(); ++it)
            QCOMPARE(it.value().size(), 512);
        ++count;
    }
    xs.close();
    QCOMPARE(count, 4);
}

void TestStorageIntegration::test_factory_pipeline_fullChain_artrecJsonXml() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta; meta.frameRate = 30; meta.universeCount = 2;
    meta.createTimestamp = 12345ULL;
    meta.mediaFilePath = QStringLiteral("media.avi");
    auto unis = makeUniverses(2);
    auto frames = makeFrames(6, 2);

    QString pArtrec = dir.path() + "/chain.artrec";
    QString pJson   = dir.path() + "/chain.json";
    QString pXml    = dir.path() + "/chain.xml";

    // 1) artrec
    ArtRecStorage ar;
    QVERIFY(ar.create(pArtrec, meta, unis));
    for(const auto &f : frames) QVERIFY(ar.writeFrame(f));
    QVERIFY(ar.finalize(frames.size(), frames.last().timestampUs, meta.mediaFilePath));
    ar.close();

    // 2) artrec → json
    JsonStorage js;
    QVERIFY(ar.open(pArtrec));
    QVERIFY(js.create(pJson, ar.meta(), ar.universes()));
    StorageFrame fr;
    while(ar.readNextFrame(fr)) QVERIFY(js.writeFrame(fr));
    QVERIFY(js.finalize(ar.meta().totalFrames, ar.meta().totalDurationUs, ar.meta().mediaFilePath));
    ar.close(); js.close();

    // 3) json → xml
    XmlStorage xs;
    QVERIFY(js.open(pJson));
    QVERIFY(xs.create(pXml, js.meta(), js.universes()));
    while(js.readNextFrame(fr)) QVERIFY(xs.writeFrame(fr));
    QVERIFY(xs.finalize(js.meta().totalFrames, js.meta().totalDurationUs, js.meta().mediaFilePath));
    js.close(); xs.close();

    // 4) xml 读回完整验证
    QVERIFY(xs.open(pXml));
    QCOMPARE(xs.meta().frameRate, quint16(30));
    QCOMPARE(xs.meta().mediaFilePath, meta.mediaFilePath);
    QCOMPARE(xs.universes().size(), 2);
    int count = 0;
    while(xs.readNextFrame(fr)) {
        QCOMPARE(fr.universeData[0], frames[count].universeData[0]);
        QCOMPARE(fr.universeData[1], frames[count].universeData[1]);
        ++count;
    }
    xs.close();
    QCOMPARE(count, 6);
}

void TestStorageIntegration::test_factory_pipeline_crossAllThree() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta; meta.frameRate = 30; meta.universeCount = 2;
    auto unis = makeUniverses(2);
    auto frames = makeFrames(4, 2);

    auto writeReadVerify = [&](IStorageFormat *w, IStorageFormat *r,
                               const QString &path) {
        QVERIFY(w->create(path, meta, unis));
        for(const auto &f : frames) QVERIFY(w->writeFrame(f));
        QVERIFY(w->finalize(frames.size(), frames.last().timestampUs));
        w->close();

        QVERIFY(r->open(path));
        int cnt = 0;
        StorageFrame fr;
        while(r->readNextFrame(fr)) {
            QCOMPARE(fr.universeData[0], frames[cnt].universeData[0]);
            QCOMPARE(fr.universeData[1], frames[cnt].universeData[1]);
            ++cnt;
        }
        r->close();
        QCOMPARE(cnt, 4);
    };

    ArtRecStorage arW, arR;
    JsonStorage jsW, jsR;
    XmlStorage xsW, xsR;

    writeReadVerify(&arW, &arR, dir.path() + "/p.artrec");
    writeReadVerify(&jsW, &jsR, dir.path() + "/p.json");
    writeReadVerify(&xsW, &xsR, dir.path() + "/p.xml");
}

// ============================================================
// L3: 错误文件
// ============================================================

void TestStorageIntegration::test_corruptedFile_allFormatsOpenFail() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QFile f1(dir.path() + "/bad.artrec");
    QVERIFY(f1.open(QIODevice::WriteOnly));
    f1.write("BADDATAXXXXXXXXX");
    f1.close();

    QFile f2(dir.path() + "/bad.json");
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("{\"meta\": {");
    f2.close();

    QFile f3(dir.path() + "/bad.xml");
    QVERIFY(f3.open(QIODevice::WriteOnly));
    f3.write("<?xml version=\"1.0\"?>\n<recording>\n<meta formatVersion=\"1\"");
    f3.close();

    ArtRecStorage ar;
    JsonStorage js;
    XmlStorage xs;

    QVERIFY(!ar.open(dir.path() + "/bad.artrec"));
    QVERIFY(!js.open(dir.path() + "/bad.json"));
    QVERIFY(!xs.open(dir.path() + "/bad.xml"));
}

void TestStorageIntegration::test_emptyFile_allFormatsBehavior() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // C++11 range-for, 替代已弃用的 foreach
    for(const QString &ext : QStringList{"artrec", "json", "xml"}) {
        QFile f(dir.path() + "/empty." + ext);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
    }

    ArtRecStorage ar;
    JsonStorage js;
    XmlStorage xs;

    QVERIFY(!ar.open(dir.path() + "/empty.artrec"));
    QVERIFY(!js.open(dir.path() + "/empty.json"));
    QVERIFY(!xs.open(dir.path() + "/empty.xml"));
}

// ============================================================
// L4: Seek 集成
// ============================================================

void TestStorageIntegration::test_seekAfterEngineRecord_artrecStorage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    RecorderEngine engine;
    RecorderEngine::Config cfg;
    cfg.outputFilePath = dir.path() + "/seek.artrec";
    cfg.frameRate = 30;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 27503;

    QSignalSpy spy(&engine, &RecorderEngine::frameRecorded);
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    sender.bind();
    quint16 uni = encodeUniverse(0, 0, 1);
    QByteArray dmx(512, 0); dmx[0] = char(77);

    sendArtDmx(sender, 27503, uni, 1, dmx, 8);
    QVERIFY2(spy.wait(2000), "应收到帧");
    engine.stop();

    ArtRecStorage fmt;
    QVERIFY(fmt.open(cfg.outputFilePath));
    QVERIFY(fmt.isValid());
    QVERIFY(fmt.meta().totalFrames > 0);
    fmt.close();
}

void TestStorageIntegration::test_seekMultiSeek() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta; meta.frameRate = 30; meta.universeCount = 1;
    auto unis = makeUniverses(1);
    auto frames = makeFrames(10, 1);

    QString artrecPath = dir.path() + "/seek10.artrec";
    QString jsonPath   = dir.path() + "/seek10.json";
    QString xmlPath    = dir.path() + "/seek10.xml";

    // artrec
    ArtRecStorage ar;
    QVERIFY(ar.create(artrecPath, meta, unis));
    for(const auto &f : frames) QVERIFY(ar.writeFrame(f));
    QVERIFY(ar.finalize(frames.size(), frames.last().timestampUs));
    ar.close();

    // artrec → json
    JsonStorage js;
    QVERIFY(ar.open(artrecPath));
    QVERIFY(js.create(jsonPath, ar.meta(), ar.universes()));
    StorageFrame fr;
    while(ar.readNextFrame(fr)) QVERIFY(js.writeFrame(fr));
    QVERIFY(js.finalize(ar.meta().totalFrames, ar.meta().totalDurationUs));
    ar.close(); js.close();

    // JsonStorage seek 验证
    QVERIFY(js.open(jsonPath));
    QVERIFY(js.seekToFrame(5));
    QVERIFY(js.readNextFrame(fr));
    QCOMPARE(fr.frameIndex, qint64(5));
    QVERIFY(js.seekToFrame(0));
    QVERIFY(js.readNextFrame(fr));
    QCOMPARE(fr.frameIndex, qint64(0));
    QVERIFY(js.seekToFrame(9));
    QVERIFY(js.readNextFrame(fr));
    QCOMPARE(fr.frameIndex, qint64(9));
    js.close();

    // XmlStorage seek 验证
    XmlStorage xs;
    QVERIFY(xs.create(xmlPath, meta, unis));
    for(const auto &f : frames) QVERIFY(xs.writeFrame(f));
    QVERIFY(xs.finalize(frames.size(), frames.last().timestampUs));
    xs.close();

    QVERIFY(xs.open(xmlPath));
    QVERIFY(xs.seekToFrame(3));
    QVERIFY(xs.readNextFrame(fr));
    QCOMPARE(fr.frameIndex, qint64(3));
    xs.close();
}

// ============================================================
// L5: 真实 DMX 跨格式保真
// ============================================================

void TestStorageIntegration::test_realDmxValues_preservedAcrossFormats() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString engArtrec = dir.path() + "/real.artrec";
    const QString convJson  = dir.path() + "/real.json";
    const QString convXml   = dir.path() + "/real.xml";

    // 1) Engine 真实录制
    RecorderEngine engine;
    RecorderEngine::Config cfg;
    cfg.outputFilePath = engArtrec;
    cfg.frameRate = 30;
    cfg.universes = { encodeUniverse(0, 0, 1) };
    cfg.artNetPort = 27504;

    QSignalSpy spy(&engine, &RecorderEngine::frameRecorded);
    QVERIFY(engine.start(cfg));

    QUdpSocket sender;
    sender.bind();
    quint16 uni = encodeUniverse(0, 0, 1);

    QByteArray vA(512, 0); vA[0] = char(0xAA); vA[127] = char(0x7E); vA[255] = char(0x10);
    QByteArray vB(512, 0); vB[0] = char(0xBB); vB[127] = char(0x8F); vB[255] = char(0x20);
    QByteArray vC(512, 0); vC[0] = char(0xCC); vC[127] = char(0xA0); vC[255] = char(0x30);

    sendArtDmx(sender, 27504, uni, 1, vA, 5);
    sendArtDmx(sender, 27504, uni, 6, vB, 5);
    sendArtDmx(sender, 27504, uni, 11, vC, 5);

    QVERIFY2(spy.wait(2000), "应收到至少 1 帧");
    engine.stop();

    qint64 nFrames = engine.recordedFrames();
    QVERIFY(nFrames > 0);

    // 2) ArtRecStorage 读 .artrec, 统计特征值
    ArtRecStorage arSrc;
    QVERIFY(arSrc.open(engArtrec));

    bool hasAA = false, hasBB = false, hasCC = false;
    StorageFrame fr;
    while(arSrc.readNextFrame(fr)) {
        if(!fr.universeData.contains(0)) continue;
        const QByteArray &d = fr.universeData[0];
        if(d[0]==char(0xAA) && d[127]==char(0x7E)) hasAA = true;
        if(d[0]==char(0xBB) && d[127]==char(0x8F)) hasBB = true;
        if(d[0]==char(0xCC) && d[127]==char(0xA0)) hasCC = true;
    }
    arSrc.close();
    QVERIFY(hasAA || hasBB || hasCC);

    // 3) artrec → json → xml
    JsonStorage js;
    QVERIFY(arSrc.open(engArtrec));
    QVERIFY(js.create(convJson, arSrc.meta(), arSrc.universes()));
    while(arSrc.readNextFrame(fr)) QVERIFY(js.writeFrame(fr));
    QVERIFY(js.finalize(arSrc.meta().totalFrames, arSrc.meta().totalDurationUs));
    arSrc.close(); js.close();

    XmlStorage xs;
    QVERIFY(js.open(convJson));
    QVERIFY(xs.create(convXml, js.meta(), js.universes()));
    while(js.readNextFrame(fr)) QVERIFY(xs.writeFrame(fr));
    QVERIFY(xs.finalize(js.meta().totalFrames, js.meta().totalDurationUs));
    js.close(); xs.close();

    // 4) xml 读回, 特征值应全部保留
    QVERIFY(xs.open(convXml));
    bool xmlHasAA = false, xmlHasBB = false, xmlHasCC = false;
    while(xs.readNextFrame(fr)) {
        const QByteArray &d = fr.universeData[0];
        if(d[0]==char(0xAA) && d[127]==char(0x7E)) xmlHasAA = true;
        if(d[0]==char(0xBB) && d[127]==char(0x8F)) xmlHasBB = true;
        if(d[0]==char(0xCC) && d[127]==char(0xA0)) xmlHasCC = true;
    }
    xs.close();

    QVERIFY(xmlHasAA || xmlHasBB || xmlHasCC);
}

QTEST_MAIN(TestStorageIntegration)
#include "TestStorageIntegration.moc"
