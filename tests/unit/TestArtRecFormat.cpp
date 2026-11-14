// TestArtRecFormat.cpp - 文件格式 round-trip 测试 (Phase 2)
//
// 测试范围: ArtRecWriter + ArtRecReader + ArtRecTypes
// 依赖: Qt Core + 临时磁盘文件
//
#include <QtTest>
#include <QTemporaryDir>
#include <cstring>

#include "fileformat/ArtRecTypes.h"
#include "fileformat/ArtRecWriter.h"
#include "fileformat/ArtRecReader.h"

class TestArtRecFormat : public QObject {
    Q_OBJECT

private slots:
    // ===== ArtRecTypes 常量 =====
    void test_magicString() {
        QCOMPARE(sizeof(ARTREC_MAGIC), size_t(8));
        QCOMPARE(memcmp(ARTREC_MAGIC, "ARTREC\0\0", 8), 0);
    }
    
    void test_versionConstant() {
        QCOMPARE(ARTREC_VERSION, quint16(0x0100));
    }
    
    // ===== packed 大小 =====
    void test_artRecHeaderSize() {
        // #pragma pack(push, 1) 后, ArtRecHeader 必须精确 64 字节
        QCOMPARE(sizeof(ArtRecHeader), size_t(64));
    }
    
    void test_universeEntrySize() {
        QCOMPARE(sizeof(UniverseEntry), size_t(4));
    }
    
    void test_frameRecordHeaderSize() {
        QCOMPARE(sizeof(FrameRecordHeader), size_t(15));
    }
    
    void test_universeDataBlockHeaderSize() {
        QCOMPARE(sizeof(UniverseDataBlockHeader), size_t(4));
    }
    
    // ===== ArtRecHeader 有效标志 =====
    void test_header_isValid() {
        ArtRecHeader h;
        h.reset();
        QVERIFY(h.isValid());
        
        // 损坏 magic
        h.magic[0] = 'X';
        QVERIFY(!h.isValid());
    }
    
    // ===== Writer/Reader round-trip =====
    void test_roundTrip_basic() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        
        QString path = dir.path() + "/test.artrec";
        
        // 准备 header
        ArtRecHeader header;
        header.reset();
        header.frameRate = 30;
        header.timeCodeType = 0;
        header.timeCodeFormat = 3;
        header.universeCount = 2;
        header.createTimestamp = QDateTime::currentMSecsSinceEpoch();
        
        QList<UniverseEntry> universes;
        universes.append(UniverseEntry(0x0001, 512));
        universes.append(UniverseEntry(0x0002, 512));
        
        // 写入 3 帧
        ArtRecWriter writer;
        QVERIFY(writer.create(path, header, universes));
        
        for (int i = 0; i < 3; ++i) {
            QMap<quint16, QByteArray> frameData;
            QByteArray dmx1(512, char(i * 10));
            QByteArray dmx2(512, char(i * 20));
            frameData[0x0001] = dmx1;
            frameData[0x0002] = dmx2;
            
            QVERIFY(writer.writeFrame(i, i * 33333LL, frameData, i > 0));
        }
        
        QVERIFY(writer.finalize(3, 100000LL));
        writer.close();
        
        // 读回
        ArtRecReader reader;
        QVERIFY(reader.open(path));
        QCOMPARE(reader.header().universeCount, quint16(2));
        QCOMPARE(reader.header().frameRate, quint16(30));
        QCOMPARE(reader.header().totalFrames, quint32(3));
        
        QCOMPARE(reader.universes().size(), 2);
        QCOMPARE(reader.universes()[0].encoded(), quint16(0x0001));
        QCOMPARE(reader.universes()[1].encoded(), quint16(0x0002));
        
        // 逐帧读取比较
        qint64 frameIndex, timestampUs;
        QMap<int, QByteArray> data;
        
        for (int i = 0; i < 3; ++i) {
            QVERIFY(reader.readNextFrame(frameIndex, timestampUs, data));
            QCOMPARE(frameIndex, qint64(i));
            QCOMPARE(timestampUs, qint64(i * 33333));
            
            // Universe 0 (索引 0)
            QByteArray expected1(512, char(i * 10));
            QCOMPARE(data.value(0), expected1);
            
            // Universe 1 (索引 1)
            QByteArray expected2(512, char(i * 20));
            QCOMPARE(data.value(1), expected2);
        }
        
        // 读完所有帧后, 下一次 readNextFrame 返回 false
        QVERIFY(!reader.readNextFrame(frameIndex, timestampUs, data));
        
        reader.close();
    }
    
    void test_roundTrip_singleUniverse() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        
        QString path = dir.path() + "/single.artrec";
        
        ArtRecHeader header;
        header.reset();
        header.frameRate = 25;
        header.universeCount = 1;
        
        QList<UniverseEntry> universes;
        universes.append(UniverseEntry(0x0001, 512));
        
        ArtRecWriter writer;
        QVERIFY(writer.create(path, header, universes));
        
        for (int i = 0; i < 10; ++i) {
            QMap<quint16, QByteArray> frameData;
            QByteArray dmx(512, char(i));
            frameData[0x0001] = dmx;
            writer.writeFrame(i, i * 40000LL, frameData, true);
        }
        writer.finalize(10, 400000LL);
        writer.close();
        
        ArtRecReader reader;
        QVERIFY(reader.open(path));
        QCOMPARE(reader.header().totalFrames, quint32(10));
        
        qint64 frameIndex, ts;
        QMap<int, QByteArray> data;
        for (int i = 0; i < 10; ++i) {
            QVERIFY(reader.readNextFrame(frameIndex, ts, data));
            QByteArray expected(512, char(i));
            QCOMPARE(data.value(0), expected);
        }
    }
    
    void test_roundTrip_dmxChanges() {
        QTemporaryDir dir;
        QString path = dir.path() + "/changes.artrec";
        
        ArtRecHeader header;
        header.reset();
        header.universeCount = 1;
        
        QList<UniverseEntry> universes;
        universes.append(UniverseEntry(0x0001, 512));
        
        ArtRecWriter writer;
        QVERIFY(writer.create(path, header, universes));
        
        // 帧 0: 全 0, changes=false
        QMap<quint16, QByteArray> frameData;
        frameData[0x0001] = QByteArray(512, char(0));
        QVERIFY(writer.writeFrame(0, 0, frameData, false));
        
        // 帧 1: 全 255, changes=true
        frameData[0x0001] = QByteArray(512, char(255));
        QVERIFY(writer.writeFrame(1, 33333LL, frameData, true));
        
        writer.finalize(2, 66666LL);
        writer.close();
        
        ArtRecReader reader;
        QVERIFY(reader.open(path));
        QVERIFY(reader.header().isValid());
        
        qint64 idx, ts;
        QMap<int, QByteArray> data;
        QVERIFY(reader.readNextFrame(idx, ts, data));
        QCOMPARE(idx, qint64(0));
        
        QVERIFY(reader.readNextFrame(idx, ts, data));
        QCOMPARE(idx, qint64(1));
        QCOMPARE(data.value(0)[0], char(255));
    }
    
    // ===== 边界情况 =====
    void test_corruptedFile() {
        QTemporaryDir dir;
        QString path = dir.path() + "corrupt.txt";
        
        // 写一段垃圾数据
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not an artrec file");
        f.close();
        
        ArtRecReader reader;
        QVERIFY(!reader.open(path));  // isValid 应该失败
    }
    
    void test_zeroFrameFile() {
        QTemporaryDir dir;
        QString path = dir.path() + "empty.artrec";
        
        ArtRecHeader header;
        header.reset();
        header.frameRate = 30;
        header.universeCount = 1;
        
        QList<UniverseEntry> universes;
        universes.append(UniverseEntry(0x0001, 512));
        
        ArtRecWriter writer;
        QVERIFY(writer.create(path, header, universes));
        QVERIFY(writer.finalize(0, 0));
        writer.close();
        
        ArtRecReader reader;
        QVERIFY(reader.open(path));
        QCOMPARE(reader.header().totalFrames, quint32(0));
        
        qint64 idx, ts;
        QMap<int, QByteArray> data;
        QVERIFY(!reader.readNextFrame(idx, ts, data));
    }
    
    void test_dmxPartialFill() {
        QTemporaryDir dir;
        QString path = dir.path() + "partial.artrec";
        
        ArtRecHeader header;
        header.reset();
        header.universeCount = 1;
        
        QList<UniverseEntry> universes;
        universes.append(UniverseEntry(0x0001, 512));
        
        ArtRecWriter writer;
        QVERIFY(writer.create(path, header, universes));
        
        // 只提供 10 字节 DMX 数据, Writer 应该自动补 0 到 512
        QMap<quint16, QByteArray> frameData;
        frameData[0x0001] = QByteArray("\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A", 10);
        QVERIFY(writer.writeFrame(0, 0, frameData, true));
        writer.finalize(1, 33333LL);
        writer.close();
        
        ArtRecReader reader;
        QVERIFY(reader.open(path));
        
        qint64 idx, ts;
        QMap<int, QByteArray> data;
        QVERIFY(reader.readNextFrame(idx, ts, data));
        
        QByteArray dmx = data.value(0);
        QCOMPARE(dmx.size(), 512);
        QCOMPARE(dmx[0], char(0x01));
        QCOMPARE(dmx[9], char(0x0A));
        // 后面的应该是 0 填充
        QCOMPARE(dmx[10], char(0x00));
        QCOMPARE(dmx[511], char(0x00));
    }
    
    void test_writer_createFileExists() {
        // create 应该能覆盖已存在的文件
        QTemporaryDir dir;
        QString path = dir.path() + "overwrite.artrec";
        
        // 先写一个占位文件
        QFile placeholder(path);
        placeholder.open(QIODevice::WriteOnly);
        placeholder.write("placeholder");
        placeholder.close();
        
        ArtRecHeader header;
        header.reset();
        header.universeCount = 0;
        
        QList<UniverseEntry> universes;
        ArtRecWriter writer;
        QVERIFY(writer.create(path, header, universes));
        writer.close();
        
        ArtRecReader reader;
        QVERIFY(reader.open(path));
        QVERIFY(reader.header().isValid());
    }
};

QTEST_MAIN(TestArtRecFormat)
#include "TestArtRecFormat.moc"
