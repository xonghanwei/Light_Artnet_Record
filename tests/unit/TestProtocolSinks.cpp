// TestProtocolSinks.cpp - 协议 Sink 层测试 (Phase 4)
//
// 测试方式: UDP 自环 — 创建监听 socket 当"假接收端", 
//           Sink 发 LightFrame → 监听端收到真实协议包 → 验证包格式正确。
//
// 重点验证: Sink 组装的协议包格式是否符合 ArtNet / NPU 规范。
//
#include <QtTest>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QThread>
#include <QDateTime>
#include <cstring>

#include "core/Common.h"
#include "protocol/ProtocolTypes.h"
#include "protocol/sinks/ArtNetSink.h"
#include "protocol/sinks/NpuSink.h"

// ============================================================
// 包格式定义 (按 ArtNetSender::ArtNetDmxTxPacket / NpuSink 实际发包)
// 用于验证 Sink 发出的包是否正确
// ============================================================
#pragma pack(push, 1)
struct ArtNetDmxRxPacket {
    char     id[8];           // "Art-Net\0"
    quint16  opCode;          // 0x5000 (小端)
    quint16  protocolVersion; // 14 (小端)
    quint8   sequence;
    quint8   physical;
    quint16  universe;        // 小端
    quint16  length;          // DMX 长度 (小端)
    quint8   dmxData[512];
}; // 总计 530 bytes
#pragma pack(pop)

#pragma pack(push, 1)
struct NpuDmxRxPacket {
    quint16 length;     // 小端, 514
    quint8  cmd;        // 0x01
    quint8  universe;
    quint8  dmxData[512];
}; // 总计 516 bytes
#pragma pack(pop)

static bool parseArtNetPacket(const QByteArray &data, ArtNetDmxRxPacket &out) {
    if (data.size() < (int)sizeof(ArtNetDmxRxPacket)) return false;
    std::memcpy(&out, data.constData(), sizeof(out));
    return std::memcmp(out.id, "Art-Net\0", 8) == 0;
}

static bool parseNpuPacket(const QByteArray &data, NpuDmxRxPacket &out) {
    if (data.size() < (int)sizeof(NpuDmxRxPacket)) return false;
    std::memcpy(&out, data.constData(), sizeof(out));
    return out.cmd == 0x01;
}

// Qt 5 QUdpSocket 没有 receiveDatagrams(), 用循环代替
static QList<QNetworkDatagram> drainAll(QUdpSocket &sock) {
    QList<QNetworkDatagram> result;
    while (sock.hasPendingDatagrams()) {
        result.append(sock.receiveDatagram());
    }
    return result;
}

// ============================================================
// TestProtocolSinks
// ============================================================
class TestProtocolSinks : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qRegisterMetaType<LightFrame>("LightFrame");
        qRegisterMetaType<quint16>("quint16");
    }

    // ===== ArtNetSink =====

    void test_artNet_startStop() {
        ArtNetSink sink;
        ConnectionParams p;
        QVERIFY(sink.start(p));
        QVERIFY(sink.isRunning());
        sink.stop();
        QVERIFY(!sink.isRunning());
    }

    void test_artNet_emitSignals() {
        ArtNetSink sink;
        QSignalSpy spyStarted(&sink, &IProtocolSink::started);
        QSignalSpy spyStopped(&sink, &IProtocolSink::stopped);
        QSignalSpy spyFrameSent(&sink, &IProtocolSink::frameSent);

        QVERIFY(sink.start(ConnectionParams{}));
        QCOMPARE(spyStarted.count(), 1);

        sink.stop();
        QCOMPARE(spyStopped.count(), 1);

        // 没有目标时 sendFrame 返回 false, frameSent 不发
        QVERIFY(!sink.sendFrame(makeFrame(0x1234, QByteArray(512, 255))));
        QCOMPARE(spyFrameSent.count(), 0);
    }

    void test_artNet_sendDmx_roundTrip() {
        // 1. 创建监听 socket (模拟 ArtNet 接收端)
        QUdpSocket listener;
        QVERIFY(listener.bind(QHostAddress::LocalHost, 16454 + 42));
        QSignalSpy spyReadyRead(&listener, &QUdpSocket::readyRead);

        // 2. 启动 Sink + 添加目标
        ArtNetSink sink;
        sink.start(ConnectionParams{});
        sink.addTarget("127.0.0.1", 16454 + 42);

        // 3. 发送 LightFrame
        quint16 testUniverse = ArtNet::encodeUniverse(0, 0, 5);
        QByteArray dmx(512, 0);
        dmx[0] = 255; dmx[1] = 128; dmx[511] = 42;
        sink.sendFrame(makeFrame(testUniverse, dmx));

        // 4. 等待 + 读
        QVERIFY2(spyReadyRead.wait(1000), "listener 应收到 ArtNet 包");
        QList<QNetworkDatagram> dgrams = drainAll(listener);
        QCOMPARE(dgrams.size(), 1);

        // 5. 验证包格式
        ArtNetDmxRxPacket pkt;
        QVERIFY2(parseArtNetPacket(dgrams.first().data(), pkt), "应解析出 ArtDMX 包");

        QCOMPARE(pkt.opCode, quint16(0x5000));
        QCOMPARE(pkt.protocolVersion, quint16(14));
        QCOMPARE(pkt.length, quint16(512));

        // Universe 小端还原
        quint16 pktUniverse = quint16(pkt.universe & 0xFF) | (quint16(pkt.universe >> 8) << 8);
        QCOMPARE(pktUniverse, testUniverse);

        QCOMPARE(pkt.dmxData[0], quint8(255));
        QCOMPARE(pkt.dmxData[1], quint8(128));
        QCOMPARE(pkt.dmxData[511], quint8(42));

        sink.stop();
    }

    void test_artNet_sequenceIncrement() {
        // 连续发 3 帧, Sequence 应递增 0→1→2
        QUdpSocket listener;
        QVERIFY(listener.bind(QHostAddress::LocalHost, 16454 + 43));
        QSignalSpy spyReady(&listener, &QUdpSocket::readyRead);

        ArtNetSink sink;
        sink.start(ConnectionParams{});
        sink.addTarget("127.0.0.1", 16454 + 43);

        quint16 uni = ArtNet::encodeUniverse(0, 0, 1);
        for (int i = 0; i < 3; i++) {
            sink.sendFrame(makeFrame(uni, QByteArray(512, quint8(i))));
        }

        QVERIFY(spyReady.wait(500));
        // drainAll 一次性把 socket 缓冲里所有包取走
        QList<QNetworkDatagram> dgrams = drainAll(listener);
        // 如果还不够, 再等一轮
        if (dgrams.size() < 3) {
            for (int retry = 0; retry < 5 && dgrams.size() < 3; retry++) {
                if (spyReady.wait(200)) {
                    dgrams.append(drainAll(listener));
                }
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(dgrams.size(), 3, 500);

        // 按 sequence 排序 (UDP 本地自环通常保持顺序, 但这里防御性排序)
        ArtNetDmxRxPacket pkts[3];
        for (int i = 0; i < 3 && i < dgrams.size(); i++) {
            QVERIFY(parseArtNetPacket(dgrams[i].data(), pkts[i]));
        }

        QCOMPARE(pkts[0].sequence, quint8(0));
        QCOMPARE(pkts[1].sequence, quint8(1));
        QCOMPARE(pkts[2].sequence, quint8(2));

        sink.stop();
    }

    void test_artNet_protocolType() {
        ArtNetSink sink;
        QCOMPARE(sink.protocolType(), ProtocolType::ArtNet);
        QVERIFY(!sink.protocolName().isEmpty());
    }

    void test_artNet_framesSentCount() {
        ArtNetSink sink;
        sink.start(ConnectionParams{});
        sink.addTarget("127.0.0.1", 16454 + 44);

        quint16 uni = ArtNet::encodeUniverse(0, 0, 1);
        sink.sendFrame(makeFrame(uni, QByteArray(512, 10)));
        sink.sendFrame(makeFrame(uni, QByteArray(512, 20)));

        QTRY_COMPARE_WITH_TIMEOUT(sink.framesSent(), quint64(2), 1000);
        sink.stop();
    }

    // ===== NpuSink =====

    void test_npu_startStop() {
        NpuSink sink;
        QVERIFY(sink.start(ConnectionParams{}));
        QVERIFY(sink.isRunning());
        sink.stop();
        QVERIFY(!sink.isRunning());
    }

    void test_npu_sendDmx_roundTrip() {
        QUdpSocket listener;
        QVERIFY(listener.bind(QHostAddress::LocalHost, 9999 + 42));
        QSignalSpy spy(&listener, &QUdpSocket::readyRead);

        NpuSink sink;
        sink.start(ConnectionParams{});
        sink.addTarget("127.0.0.1", 9999 + 42);

        quint8 testUniverse = 15;
        QByteArray dmx(512, 0);
        dmx[0] = 255; dmx[10] = 128; dmx[500] = 99;
        sink.sendFrame(makeFrameRaw(testUniverse, dmx));

        QVERIFY2(spy.wait(1000), "listener 应收到 NPU 包");
        QList<QNetworkDatagram> dgrams = drainAll(listener);
        QCOMPARE(dgrams.size(), 1);

        NpuDmxRxPacket pkt;
        QVERIFY2(parseNpuPacket(dgrams.first().data(), pkt), "应解析出 NPU DMX 包");

        quint16 pktLen = quint16(pkt.length & 0xFF) | (quint16(pkt.length >> 8) << 8);
        QCOMPARE(pktLen, quint16(514));    // Length 应为 514
        QCOMPARE(pkt.cmd, quint8(0x01));
        QCOMPARE(pkt.universe, testUniverse);

        QCOMPARE(pkt.dmxData[0], quint8(255));
        QCOMPARE(pkt.dmxData[10], quint8(128));
        QCOMPARE(pkt.dmxData[500], quint8(99));

        sink.stop();
    }

    void test_npu_emitSignals() {
        NpuSink sink;
        QSignalSpy spyFrameSent(&sink, &IProtocolSink::frameSent);

        QUdpSocket listener;
        QVERIFY(listener.bind(QHostAddress::LocalHost, 9999 + 43));
        QSignalSpy spyReady(&listener, &QUdpSocket::readyRead);

        sink.start(ConnectionParams{});
        sink.addTarget("127.0.0.1", 9999 + 43);
        sink.sendFrame(makeFrameRaw(7, QByteArray(512, 1)));

        QVERIFY(spyReady.wait(1000));

        QCOMPARE(spyFrameSent.count(), 1);
        QList<QVariant> args = spyFrameSent.takeFirst();
        QCOMPARE(args.at(1).value<quint64>(), quint64(1)); // 1 个目标

        sink.stop();
    }

    void test_npu_protocolType() {
        NpuSink sink;
        QCOMPARE(sink.protocolType(), ProtocolType::Npu);
    }

    void test_npu_framesSentCount() {
        NpuSink sink;
        sink.start(ConnectionParams{});
        sink.addTarget("127.0.0.1", 9999 + 44);

        sink.sendFrame(makeFrameRaw(1, QByteArray(512, 10)));
        sink.sendFrame(makeFrameRaw(2, QByteArray(512, 20)));

        QTRY_COMPARE_WITH_TIMEOUT(sink.framesSent(), quint64(2), 1000);
        sink.stop();
    }

    // ===== 多目标 fan-out + 过滤 =====

    void test_artNet_multiTarget_fanout() {
        QUdpSocket listenerA, listenerB;
        QVERIFY(listenerA.bind(QHostAddress::LocalHost, 16454 + 50));
        QVERIFY(listenerB.bind(QHostAddress::LocalHost, 16454 + 51));
        QSignalSpy spyA(&listenerA, &QUdpSocket::readyRead);
        QSignalSpy spyB(&listenerB, &QUdpSocket::readyRead);

        ArtNetSink sink;
        sink.start(ConnectionParams{});
        sink.addTarget("127.0.0.1", 16454 + 50);
        sink.addTarget("127.0.0.1", 16454 + 51);
        QCOMPARE(sink.targetCount(), 2);

        quint16 uni = ArtNet::encodeUniverse(0, 0, 9);
        sink.sendFrame(makeFrame(uni, QByteArray(512, 77)));

        QVERIFY(spyA.wait(1000));
        if (!spyB.count()) QVERIFY(spyB.wait(500));

        auto dgramsA = drainAll(listenerA);
        auto dgramsB = drainAll(listenerB);
        QCOMPARE(dgramsA.size(), 1);
        QCOMPARE(dgramsB.size(), 1);

        ArtNetDmxRxPacket pa, pb;
        QVERIFY(parseArtNetPacket(dgramsA.first().data(), pa));
        QVERIFY(parseArtNetPacket(dgramsB.first().data(), pb));
        QCOMPARE(std::memcmp(pa.dmxData, pb.dmxData, 512), 0);
        QCOMPARE(pa.dmxData[0], quint8(77));

        sink.stop();
    }

    void test_artNet_universeFilter() {
        // Target A 只收 Universe 1, Target B 无过滤
        // 发 Universe 2 → 只有 B 收到; 发 Universe 1 → 都收到
        QUdpSocket listenerA, listenerB;
        QVERIFY(listenerA.bind(QHostAddress::LocalHost, 16454 + 60));
        QVERIFY(listenerB.bind(QHostAddress::LocalHost, 16454 + 61));
        QSignalSpy spyA(&listenerA, &QUdpSocket::readyRead);
        QSignalSpy spyB(&listenerB, &QUdpSocket::readyRead);

        ArtNetSink sink;
        sink.start(ConnectionParams{});

        quint16 uni1 = ArtNet::encodeUniverse(0, 0, 1);
        quint16 uni2 = ArtNet::encodeUniverse(0, 0, 2);

        sink.addTarget("127.0.0.1", 16454 + 60, { uni1 }); // A: 只收 uni1
        sink.addTarget("127.0.0.1", 16454 + 61);             // B: 全部

        // 发 uni2 → 只有 B 收到
        sink.sendFrame(makeFrame(uni2, QByteArray(512, 0xBB)));
        QVERIFY(spyB.wait(1000));
        // drain 掉这次收到的数据, 避免影响后续 readyRead 触发
        auto dgramsB1 = drainAll(listenerB);
        QThread::usleep(50000); // 50ms 让事件循环跑完
        QVERIFY(spyA.count() == 0 || drainAll(listenerA).isEmpty()); // A 缓冲应该空
        QCOMPARE(spyB.count(), 1);

        // 发 uni1 → 都收到
        sink.sendFrame(makeFrame(uni1, QByteArray(512, 0xAA)));
        if (!spyA.count()) QVERIFY(spyA.wait(1000));
        if (spyB.count() < 2) QVERIFY(spyB.wait(1000));
        QCOMPARE(spyA.count(), 1);
        QCOMPARE(spyB.count(), 2);

        sink.stop();
    }

    // ===== 边界与错误处理 =====

    void test_artNet_noTarget_sendFails() {
        ArtNetSink sink;
        sink.start(ConnectionParams{});
        sink.clearTargets();
        QVERIFY(!sink.sendFrame(makeFrame(0, QByteArray(512, 0))));
        QCOMPARE(sink.framesSent(), quint64(0));
        sink.stop();
    }

    void test_artNet_notRunning_sendFails() {
        ArtNetSink sink; // 没 start
        sink.addTarget("127.0.0.1", 16454 + 70);
        QVERIFY(!sink.sendFrame(makeFrame(0, QByteArray(512, 0))));
    }

    void test_npu_noTarget_sendFails() {
        NpuSink sink;
        sink.start(ConnectionParams{});
        sink.clearTargets();
        QVERIFY(!sink.sendFrame(makeFrameRaw(1, QByteArray(512, 0))));
        sink.stop();
    }

private:
    LightFrame makeFrame(quint16 universe, const QByteArray &dmx) {
        LightFrame f;
        f.sourceProtocol = ProtocolType::ArtNet;
        f.universe = universe;
        f.universeRaw = quint8(universe & 0x0F);
        f.dmxData = dmx.leftJustified(512, 0);
        f.sequence = 0;
        f.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        return f;
    }

    LightFrame makeFrameRaw(quint8 rawUniverse, const QByteArray &dmx) {
        LightFrame f;
        f.sourceProtocol = ProtocolType::Npu;
        f.universeRaw = rawUniverse;
        f.universe = quint16(rawUniverse);
        f.dmxData = dmx.leftJustified(512, 0);
        f.sequence = 0;
        f.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
        return f;
    }
};

QTEST_MAIN(TestProtocolSinks)
#include "TestProtocolSinks.moc"
