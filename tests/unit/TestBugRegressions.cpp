// TestBugRegressions.cpp - Bug 回归测试
//
// 覆盖本次 Phase 5 修复的 8 个 Bug, 确保每个修复都有测试守护
//
// Bug #1: ArtRecWriter::create() 持锁时调 close() → QMutex 嵌套死锁
// Bug #2: RecorderEngine::stop() → cleanup() 覆盖 state 为 Idle
// Bug #3: ArtRecWriter::create() 未赋值 universeCount (永远 0)
// Bug #4: ArtNetReceiver::bind() 用 ShareAddressHint → 多实例可绑同端口
// Bug #5: RecordState 未 Q_DECLARE_METATYPE → QSignalSpy 失败
// Bug #6: ArtDMX 测试包 length 只设 1 字节 (LE quint16 需要 2)
// Bug #7: ArtRecHeader reserved[24] → sizeof=68 ≠ size()=64
// Bug #8: TestArtRecFormat 错用 data.value(1) → 应为 data.value(0)
//
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QUdpSocket>
#include <cstring>
#include <chrono>

#include "core/Common.h"
#include "fileformat/ArtRecTypes.h"
#include "fileformat/ArtRecWriter.h"
#include "fileformat/ArtRecReader.h"
#include "recorder/RecorderEngine.h"
#include "network/ArtNetReceiver.h"

using namespace ArtNet;

// 用于 Bug #5 验证的 slot 类
class SpyHelper : public QObject {
    Q_OBJECT
public:
    RecordState lastState = RecordState::Idle;
    int stateChangedCount = 0;
public slots:
    void onStateChanged(RecordState s) {
        lastState = s;
        stateChangedCount++;
    }
};

class TestBugRegressions : public QObject {
    Q_OBJECT

private:
    QTemporaryDir *m_dir = nullptr;

private slots:
    void initTestCase() {
        m_dir = new QTemporaryDir();
        QVERIFY(m_dir->isValid());
    }

    void cleanupTestCase() {
        delete m_dir;
    }

    // ============================================================
    // Bug #1: ArtRecWriter::create() 持锁时调 close() → 死锁
    // 根因: create() { QMutexLocker locker(&m_mutex); close(); }
    //       close() 内部也 QMutexLocker locker(&m_mutex)
    //       QMutex 默认非递归 → 同一线程 lock 两次 → 死锁
    // 修复: 把 close() 内联 (flush + close + m_initialized=false)
    // 验证: 多次 create() 调用不应卡住
    // ============================================================
    void test_bug01_writerCreateNoDeadlock() {
        QBENCHMARK {
            ArtRecWriter writer;
            ArtRecHeader header;
            header.reset();
            header.frameRate = 30;

            QList<UniverseEntry> universes;
            universes.append(UniverseEntry(0));

            // 连续 create → close → create, 不应死锁
            for (int i = 0; i < 10; i++) {
                QString path = m_dir->path() + QString("/bug01_%1.artrec").arg(i);
                QVERIFY(writer.create(path, header, universes));
                QVERIFY(writer.isOpen());
                writer.close();
                QVERIFY(!writer.isOpen());
            }
        }
    }

    // ============================================================
    // Bug #2: RecorderEngine::stop() → cleanup() 覆盖 state 为 Idle
    // 根因: stop() 先 setState(Stopped) → cleanup() 末尾 m_state = Idle
    //       导致外部观察到 state 是 Idle 不是 Stopped
    // 修复: stop() 改为先 cleanup() 再 setState(Stopped)
    // 验证: stop() 后 engine.state() == RecordState::Stopped
    // ============================================================
    void test_bug02_stopStateIsStopped() {
        RecorderEngine engine;
        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/bug02.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 27101;

        QSignalSpy spyFinished(&engine, &RecorderEngine::recordingFinished);

        QVERIFY(engine.start(cfg));
        QCOMPARE(engine.state(), RecordState::Recording);

        engine.stop();
        // recordingFinished 是 DirectConnection, stop() 返回前已同步 emit
        QVERIFY(spyFinished.count() >= 1);

        // 关键验证: stop 后 state 必须是 Stopped, 不是 Idle
        QCOMPARE(engine.state(), RecordState::Stopped);
    }

    // ============================================================
    // Bug #3: ArtRecWriter::create() 未赋值 universeCount
    // 根因: headerToWrite.universeCount 一直是 ArtRecHeader() 默认值 0
    // 修复: create() 里加 headerToWrite.universeCount = quint16(universes.size())
    // 验证: create() 后 reader.header().universeCount == universes.size()
    // ============================================================
    void test_bug03_universeCountWritten() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // 3 个 Universe
        QList<UniverseEntry> universes;
        universes.append(UniverseEntry(0x0001));
        universes.append(UniverseEntry(0x0002));
        universes.append(UniverseEntry(0x0100));

        ArtRecHeader header;
        header.reset();
        header.frameRate = 30;
        // 故意不设 universeCount, 让 create() 自己赋值
        header.universeCount = 0xFFFF; // 污染值

        ArtRecWriter writer;
        QString path = dir.path() + "/bug03.artrec";
        QVERIFY(writer.create(path, header, universes));

        writer.finalize(0, 0);
        writer.close();

        // 读回验证
        ArtRecReader reader;
        QVERIFY(reader.open(path));
        QCOMPARE(reader.header().universeCount, quint8(3));
        QCOMPARE(reader.universes().size(), 3);
        reader.close();
    }

    // ============================================================
    // Bug #4: ArtNetReceiver 用 ShareAddress → 多实例可绑同端口
    // 根因: QUdpSocket::bind() 带 ShareAddressHint + ReuseAddressHint
    //       Windows 上允许多个 socket 绑定同一端口
    //       导致 test_portBusy 失败 (第二个 start() 应该返回 false)
    // 修复: 完全去掉 bind flags, 默认独占绑定
    // 验证: 两个 ArtNetReceiver 不能同时绑同一端口
    // ============================================================
    void test_bug04_portExclusiveBind() {
        ArtNetReceiver r1;
        QVERIFY(r1.start(27200));
        QVERIFY(r1.isRunning());

        // 第二个必须绑失败
        ArtNetReceiver r2;
        QVERIFY(!r2.start(27200));
        QVERIFY(!r2.isRunning());

        r1.stop();
        r2.stop();
    }

    // ============================================================
    // Bug #5: RecordState 未 Q_DECLARE_METATYPE → QSignalSpy 失败
    // 根因: QSignalSpy 需要 QMetaType 能注册参数类型
    //       RecordState 是 enum class, 缺 Q_DECLARE_METATYPE
    // 修复: Common.h 末尾加 Q_DECLARE_METATYPE(RecordState)
    // 验证: QSignalSpy 能正常捕获 stateChanged 信号
    // ============================================================
    void test_bug05_recordStateRegistered() {
        // 如果 RecordState 没注册, QSignalSpy 构造会打印警告并失败
        RecorderEngine engine;
        RecorderEngine::Config cfg;
        cfg.outputFilePath = m_dir->path() + "/bug05.artrec";
        cfg.frameRate = 30;
        cfg.syncSource = RecorderEngine::SyncSource::InternalClock;
        cfg.universes = { encodeUniverse(0, 0, 1) };
        cfg.artNetPort = 27301;

        QSignalSpy spy(&engine, &RecorderEngine::stateChanged);
        QVERIFY(spy.isValid());  // spy 构造成功 = 类型已注册

        QVERIFY(engine.start(cfg));

        // stateChanged 是 DirectConnection, start() 返回前已同步 emit
        QVERIFY(spy.count() >= 1);

        // 验证参数类型正确 (Q_DECLARE_METATYPE 修复)
        QList<QVariant> firstArgs = spy.takeFirst();
        QCOMPARE(firstArgs.size(), 1);
        QVERIFY(firstArgs.first().canConvert<RecordState>());

        engine.stop();
    }

    // ============================================================
    // Bug #6: ArtDMX 测试包 length 字段只设 1 字节
    // 根因: pkt[16] = 512 & 0xFF 只设了低字节 = 0
    //       高字节 pkt[17] 是默认 0
    //       receiver 解析 quint16 length = (0<<8)|0 = 0
    //       导致 frame.length 判定失败 → 丢弃所有包
    // 修复: pkt[16] = 512 & 0xFF; pkt[17] = (512 >> 8) & 0xFF;
    // 验证: 构造 ArtDMX 包 → QUdpSocket 发送 → ArtNetReceiver 解析出 length=512
    // ============================================================
    void test_bug06_artDmxPacketLengthTwoBytes() {
        // 启动 receiver
        ArtNetReceiver receiver;
        QSignalSpy spy(&receiver, &ArtNetReceiver::dmxFrameReceived);
        QVERIFY(receiver.start(27401));

        // 构造 ArtDMX 包 (和 TestRecorderEngine 里一样的方式)
        QUdpSocket sender;
        sender.bind();

        QByteArray pkt(530, 0);
        std::memcpy(pkt.data(), "Art-Net\0", 8);
        pkt[8] = 0x00; pkt[9] = 0x50;     // OpCode ArtDMX
        pkt[10] = 14;                      // ProtVer
        pkt[12] = 1;                       // Sequence
        pkt[14] = 0; pkt[15] = 0;          // Universe (LE)

        // 关键: length 是 LE quint16, 必须设两个字节!
        pkt[16] = quint8(512 & 0xFF);       // 低字节 = 0
        pkt[17] = quint8((512 >> 8) & 0xFF); // 高字节 = 2

        // DMX 数据: 通道 0 = 88
        pkt[18] = 88;

        sender.writeDatagram(pkt, QHostAddress("127.0.0.1"), 27401);

        QVERIFY2(spy.wait(2000), "ArtDMX 包应在 2s 内被接收");
        QCOMPARE(spy.count(), 1);

        // 验证解析出的 length 和 DMX 数据
        QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.size(), 1);
        ArtNetDmxFrame frame = args.first().value<ArtNetDmxFrame>();
        QCOMPARE(frame.length, quint16(512));   // length 正确 = 512
        QCOMPARE(quint8(frame.dmxData[0]), quint8(88));  // 通道 0 = 88

        receiver.stop();
    }

    // ============================================================
    // Bug #7: ArtRecHeader sizeof=68 ≠ size()=64
    // 根因: reserved[24] + 对齐 → 实际 sizeof 68
    //       但 static constexpr size() 返回 64
    //       读写时会越界 / 写残数据
    // 修复: reserved[24] → reserved[20], 让 sizeof 精确 64
    // 验证: sizeof == ArtRecHeader::size() == 64
    // ============================================================
    void test_bug07_headerSizeMatches() {
        QCOMPARE(sizeof(ArtRecHeader), size_t(ArtRecHeader::size()));
        QCOMPARE(sizeof(ArtRecHeader), size_t(64));
    }

    // ============================================================
    // Bug #8: TestArtRecFormat 错用 data.value(1) → 应为 data.value(0)
    // 根因: 只有 1 个 Universe, ArtRecReader 返回的 QMap key=0
    //       不是 key=1
    //       测试读 data.value(1) 得到默认空 QByteArray
    // 修复: 改为 data.value(0)
    // 验证: 单 Universe 文件的帧数据 key 是 0
    // ============================================================
    void test_bug08_singleUniverseKeyIsZero() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        ArtRecHeader header;
        header.reset();
        header.frameRate = 30;

        ArtRecWriter writer;
        QVERIFY(writer.create(dir.path() + "/bug08.artrec", header,
                              { UniverseEntry(0) }));

        QMap<quint16, QByteArray> dmx;
        dmx[0] = QByteArray(512, char(255));  // 全亮帧
        writer.writeFrame(0, 0, dmx, true);
        writer.finalize(1, 33000);
        writer.close();

        // 读回验证
        ArtRecReader reader;
        QVERIFY(reader.open(dir.path() + "/bug08.artrec"));

        qint64 fi, tu;
        QMap<int, QByteArray> frame;
        QVERIFY(reader.readNextFrame(fi, tu, frame));

        QCOMPARE(frame.size(), 1);
        // 关键: 单 Universe 的 key 必须是 0 (对应 UniverseEntry[0])
        QVERIFY(frame.contains(0));
        QCOMPARE(quint8(frame.value(0)[0]), quint8(255));

        // value(1) 不存在, 应该返回默认空 QByteArray
        QVERIFY(!frame.contains(1));
        reader.close();
    }
};

QTEST_MAIN(TestBugRegressions)
#include "TestBugRegressions.moc"
