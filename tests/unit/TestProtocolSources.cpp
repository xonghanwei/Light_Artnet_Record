// TestProtocolSources.cpp - 协议 Source 层测试 (Phase 3)
//
// 测试方式: UDP 自环 — 启动 Source 监听端口, 用测试程序自己的 QUdpSocket
//           发送真实的 ArtNet / NPU 协议包, 验证 Source 能否正确解析。
//
// 依赖: Qt Core + Qt Network (QUdpSocket), 真实 UDP 网络
// 不依赖: 任何硬件 (DMX512 跳过)
//
#include <QtTest>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QThread>
#include <cstring>

#include "core/Common.h"
#include "protocol/ProtocolTypes.h"
#include "protocol/sources/ArtNetSource.h"
#include "protocol/sources/NpuSource.h"
#include "network/ArtNetReceiver.h"

// ============================================================
// 构造 ArtNet ArtDMX 包
// ============================================================
static QByteArray makeArtNetDmxPacket(quint16 universe, quint8 sequence,
                                      const QByteArray &dmxData) {
    QByteArray packet;
    packet.resize(18 + 512);
    char *raw = packet.data();

    // ID: "Art-Net\0"
    std::memcpy(raw, ArtNet::ID_STRING, 8);

    // OpCode = 0x5000 (ArtDMX), 小端
    raw[8] = 0x00;
    raw[9] = 0x50;

    // Protocol Version = 14, 小端
    raw[10] = 14;
    raw[11] = 0;

    // Sequence
    raw[12] = sequence;

    // Physical
    raw[13] = 0;

    // Universe (小端)
    raw[14] = quint8(universe & 0xFF);
    raw[15] = quint8((universe >> 8) & 0xFF);

    // Length (DMX 数据长度, 小端), 固定 512
    int len = dmxData.isEmpty() ? 512 : qMin(dmxData.size(), 512);
    raw[16] = quint8(len & 0xFF);
    raw[17] = quint8((len >> 8) & 0xFF);

    // DMX 数据
    std::memset(raw + 18, 0, 512);
    if (!dmxData.isEmpty()) {
        std::memcpy(raw + 18, dmxData.constData(), len);
    }

    return packet;
}

// ============================================================
// 构造 NPU DMX 包
// ============================================================
static QByteArray makeNpuDmxPacket(quint8 universe, const QByteArray &dmxData) {
    QByteArray packet;
    packet.resize(516); // 2 + 1 + 1 + 512
    char *raw = packet.data();

    // Length (小端) = 514 (cmd + universe + 512 DMX)
    quint16 length = 514;
    raw[0] = quint8(length & 0xFF);
    raw[1] = quint8((length >> 8) & 0xFF);

    // Cmd = 0x01
    raw[2] = NpuProtocol::OP_DMX;

    // Universe
    raw[3] = universe;

    // DMX 数据
    std::memset(raw + 4, 0, 512);
    if (!dmxData.isEmpty()) {
        int len = qMin(dmxData.size(), 512);
        std::memcpy(raw + 4, dmxData.constData(), len);
    }

    return packet;
}

// ============================================================
// TestProtocolSources
// ============================================================
class TestProtocolSources : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // QSignalSpy 需要 LightFrame 已注册
        qRegisterMetaType<LightFrame>("LightFrame");
    }

    // ===== ArtNetSource =====

    void test_artNet_startStop() {
        ArtNetSource src;
        ConnectionParams p;
        p.localPort = 16454; // 用非标准端口避免冲突

        QVERIFY2(src.start(p), "start() 应返回 true");
        QVERIFY(src.isRunning());

        src.stop();
        QVERIFY(!src.isRunning());
    }

    void test_artNet_receive_dmx() {
        // 1. 启动 Source
        ArtNetSource src;
        ConnectionParams p;
        p.localPort = 16455;
        QVERIFY(src.start(p));

        // 2. 连接 frameReceived 信号
        QSignalSpy spy(&src, &IProtocolSource::frameReceived);
        QVERIFY(spy.isValid());

        // 3. 创建 sender socket 发送 ArtNet ArtDMX 包
        QUdpSocket sender;
        sender.bind(); // 让系统分配端口

        quint16 testUniverse = ArtNet::encodeUniverse(0, 0, 3); // Universe 3
        QByteArray testDmx(512, 0);
        testDmx[0] = 255;
        testDmx[1] = 128;
        testDmx[10] = 64;
        QByteArray packet = makeArtNetDmxPacket(testUniverse, 1, testDmx);

        sender.writeDatagram(packet, QHostAddress("127.0.0.1"), 16455);

        // 4. 等待信号 (最多 1 秒)
        QVERIFY2(spy.wait(1000), "frameReceived 应在 1 秒内触发");
        QCOMPARE(spy.count(), 1);

        // 5. 验证 LightFrame
        QList<QVariant> args = spy.takeFirst();
        LightFrame lf = args.at(0).value<LightFrame>();

        QCOMPARE(lf.sourceProtocol, ProtocolType::ArtNet);
        QCOMPARE(lf.universe, testUniverse);
        QCOMPARE(lf.universeRaw, quint8(3)); // Universe 3 低 4 bit
        QCOMPARE(lf.dmxData.size(), 512);
        QCOMPARE(quint8(lf.dmxData[0]), quint8(255));
        QCOMPARE(quint8(lf.dmxData[1]), quint8(128));
        QCOMPARE(quint8(lf.dmxData[10]), quint8(64));
        QCOMPARE(lf.sequence, quint32(1));

        src.stop();
    }

    void test_artNet_universeFilter_block() {
        // 测试: Source 过滤 Universe 0, 发送 Universe 1 的包 → 不应收到
        ArtNetSource src;
        ConnectionParams p;
        p.localPort = 16456;
        p.filterUniverses = { ArtNet::encodeUniverse(0, 0, 1) }; // 只接收 Universe 1
        QVERIFY(src.start(p));

        QSignalSpy spy(&src, &IProtocolSource::frameReceived);
        QVERIFY(spy.isValid());

        QUdpSocket sender;
        sender.bind();

        // 发送 Universe 0 (应该被过滤)
        quint16 uni0 = ArtNet::encodeUniverse(0, 0, 0);
        QByteArray packet0 = makeArtNetDmxPacket(uni0, 1, QByteArray(512, 0));
        sender.writeDatagram(packet0, QHostAddress("127.0.0.1"), 16456);

        // 发送 Universe 1 (应该通过)
        quint16 uni1 = ArtNet::encodeUniverse(0, 0, 1);
        QByteArray packet1 = makeArtNetDmxPacket(uni1, 2, QByteArray(512, 0xAA));
        sender.writeDatagram(packet1, QHostAddress("127.0.0.1"), 16456);

        // 等待 (spy.wait 内部运行事件循环处理 UDP)
        QVERIFY(spy.wait(1000));

        // 应该只收到 1 帧 (Universe 1), Universe 0 被过滤
        QCOMPARE(spy.count(), 1);
        LightFrame lf = spy.takeFirst().at(0).value<LightFrame>();
        QCOMPARE(lf.universe, uni1);
        QCOMPARE(quint8(lf.dmxData[0]), quint8(0xAA));

        src.stop();
    }

    void test_artNet_multiplePackets() {
        // 连续发送 5 帧, 全部应该收到
        ArtNetSource src;
        ConnectionParams p;
        p.localPort = 16457;
        QVERIFY(src.start(p));

        QSignalSpy spy(&src, &IProtocolSource::frameReceived);
        QUdpSocket sender;
        sender.bind();

        quint16 uni = ArtNet::encodeUniverse(0, 0, 5);
        for (int i = 0; i < 5; i++) {
            QByteArray dmx(512, quint8(i * 50));
            QByteArray pkt = makeArtNetDmxPacket(uni, quint8(i + 1), dmx);
            sender.writeDatagram(pkt, QHostAddress("127.0.0.1"), 16457);
        }

        QVERIFY2(spy.wait(1000), "应收到至少一帧");
        // UDP 自环本地通信很快, 应该 5 帧全到
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 5, 1000);

        src.stop();
    }

    void test_artNet_protocolType() {
        ArtNetSource src;
        QCOMPARE(src.protocolType(), ProtocolType::ArtNet);
        QVERIFY(!src.protocolName().isEmpty());
    }

    // ===== NpuSource =====

    void test_npu_startStop() {
        NpuSource src;
        ConnectionParams p;
        p.localPort = 19999; // NPU 默认 9999, 我们用另一个
        QVERIFY2(src.start(p), "start() 应返回 true");
        QVERIFY(src.isRunning());

        src.stop();
        QVERIFY(!src.isRunning());
    }

    void test_npu_receive_dmx() {
        NpuSource src;
        ConnectionParams p;
        p.localPort = 19998;
        QVERIFY(src.start(p));

        QSignalSpy spy(&src, &IProtocolSource::frameReceived);

        QUdpSocket sender;
        sender.bind();

        quint8 testUniverse = 7;
        QByteArray testDmx(512, 0);
        testDmx[0] = 255;
        testDmx[100] = 128;
        QByteArray packet = makeNpuDmxPacket(testUniverse, testDmx);

        sender.writeDatagram(packet, QHostAddress("127.0.0.1"), 19998);

        QVERIFY2(spy.wait(1000), "frameReceived 应在 1 秒内触发");
        QCOMPARE(spy.count(), 1);

        LightFrame lf = spy.takeFirst().at(0).value<LightFrame>();
        QCOMPARE(lf.sourceProtocol, ProtocolType::Npu);
        QCOMPARE(lf.universeRaw, testUniverse);
        QCOMPARE(lf.dmxData.size(), 512);
        QCOMPARE(quint8(lf.dmxData[0]), quint8(255));
        QCOMPARE(quint8(lf.dmxData[100]), quint8(128));

        src.stop();
    }

    void test_npu_universeFilter() {
        NpuSource src;
        ConnectionParams p;
        p.localPort = 19997;
        p.filterUniverses = { 3 }; // 只收 Universe 3
        QVERIFY(src.start(p));

        QSignalSpy spy(&src, &IProtocolSource::frameReceived);

        QUdpSocket sender;
        sender.bind();

        // Universe 5 → 被过滤
        QByteArray pkt5 = makeNpuDmxPacket(5, QByteArray(512, 0x11));
        sender.writeDatagram(pkt5, QHostAddress("127.0.0.1"), 19997);

        // Universe 3 → 通过
        QByteArray pkt3 = makeNpuDmxPacket(3, QByteArray(512, 0x22));
        sender.writeDatagram(pkt3, QHostAddress("127.0.0.1"), 19997);

        QVERIFY(spy.wait(1000));
        QCOMPARE(spy.count(), 1);
        LightFrame lf = spy.takeFirst().at(0).value<LightFrame>();
        QCOMPARE(lf.universeRaw, quint8(3));
        QCOMPARE(quint8(lf.dmxData[0]), quint8(0x22));

        src.stop();
    }

    void test_npu_protocolType() {
        NpuSource src;
        QCOMPARE(src.protocolType(), ProtocolType::Npu);
    }

    void test_npu_framesReceived() {
        NpuSource src;
        ConnectionParams p;
        p.localPort = 19996;
        QVERIFY(src.start(p));

        QSignalSpy spy(&src, &IProtocolSource::frameReceived);
        QUdpSocket sender;
        sender.bind();

        // 发送 3 帧
        for (int i = 0; i < 3; i++) {
            QByteArray pkt = makeNpuDmxPacket(1, QByteArray(512, quint8(i)));
            sender.writeDatagram(pkt, QHostAddress("127.0.0.1"), 19996);
        }

        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 3, 1000);
        QCOMPARE(src.framesReceived(), quint64(3));

        src.stop();
    }

    // ===== 无效包处理 (不应 crash) =====

    void test_artNet_corruptPacket_noCrash() {
        ArtNetSource src;
        ConnectionParams p;
        p.localPort = 16453;
        QVERIFY(src.start(p));

        QUdpSocket sender;
        sender.bind();

        // 发送垃圾数据 (不是 ArtNet 包)
        QByteArray garbage(20, 'X');
        sender.writeDatagram(garbage, QHostAddress("127.0.0.1"), 16453);

        // 程序不应 crash
        QThread::msleep(100);
        QVERIFY(src.isRunning()); // 还在运行 = 没 crash

        src.stop();
    }

    void test_npu_shortPacket_noCrash() {
        NpuSource src;
        ConnectionParams p;
        p.localPort = 19995;
        QVERIFY(src.start(p));

        QUdpSocket sender;
        sender.bind();

        // 发送太短的包
        QByteArray shortPkt(2, 0);
        sender.writeDatagram(shortPkt, QHostAddress("127.0.0.1"), 19995);

        QThread::msleep(100);
        QVERIFY(src.isRunning());

        src.stop();
    }

    void test_npu_wrongCmd_ignored() {
        // NPU 收到非 DMX 的包 (如 cmd=0x02 Universe Info) 应静默忽略
        NpuSource src;
        ConnectionParams p;
        p.localPort = 19994;
        QVERIFY(src.start(p));

        QSignalSpy spy(&src, &IProtocolSource::frameReceived);
        QUdpSocket sender;
        sender.bind();

        // 构造 cmd=0x02 的包
        QByteArray pkt(4, 0);
        pkt[2] = 0x02; // cmd = Universe Info

        sender.writeDatagram(pkt, QHostAddress("127.0.0.1"), 19994);

        QThread::msleep(300);
        QCOMPARE(spy.count(), 0); // 不应该触发 frameReceived

        src.stop();
    }
};

QTEST_MAIN(TestProtocolSources)
#include "TestProtocolSources.moc"
