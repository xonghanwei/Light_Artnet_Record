// TestCommon.cpp - Core 层纯逻辑测试 (Phase 1)
//
// 测试范围: Common.h 里的所有内联函数 + 常量
// 依赖: 只有 Qt Core, 零外部依赖
//
#include <QtTest>
#include "core/Common.h"

class TestCommon : public QObject {
    Q_OBJECT

private slots:
    // ===== ArtNet 常量 =====
    void test_artNetConstants() {
        QCOMPARE(ArtNet::DEFAULT_PORT, quint16(6454));
        QCOMPARE(ArtNet::DMX_CHANNEL_COUNT, 512);
        QCOMPARE(ArtNet::MAX_UNIVERSES, 256);
    }
    
    void test_artNetMagicString() {
        // "Art-Net\0" 应该是 8 字节
        QCOMPARE(sizeof(ArtNet::ID_STRING), size_t(8));
        QCOMPARE(ArtNet::ID_STRING[0], 'A');
        QCOMPARE(ArtNet::ID_STRING[7], '\0');
        
        // 完整内容
        QByteArray magic(ArtNet::ID_STRING, 8);
        QByteArray expected("Art-Net\0", 8);
        QCOMPARE(magic, expected);
    }
    
    void test_artNetOpCodes() {
        QCOMPARE(ArtNet::OP_DMX, quint16(0x5000));
        QCOMPARE(ArtNet::OP_RDM, quint16(0x6000));
        QCOMPARE(ArtNet::OP_POLL, quint16(0x2000));
        QCOMPARE(ArtNet::OP_TIMECODE, quint16(0x9700));
    }
    
    // ===== 帧率函数 =====
    void test_frameRateValue() {
        QCOMPARE(frameRateValue(FrameRate::FPS_24), 24);
        QCOMPARE(frameRateValue(FrameRate::FPS_25), 25);
        QCOMPARE(frameRateValue(FrameRate::FPS_2997), 29);
        QCOMPARE(frameRateValue(FrameRate::FPS_30), 30);
        QCOMPARE(frameRateValue(FrameRate::FPS_60), 60);
    }
    
    void test_frameRateDouble() {
        QCOMPARE(frameRateDouble(FrameRate::FPS_24), 24.0);
        QCOMPARE(frameRateDouble(FrameRate::FPS_25), 25.0);
        
        // 29.97 浮点比较用 0.01 容差
        double fr2997 = frameRateDouble(FrameRate::FPS_2997);
        QVERIFY(qAbs(fr2997 - 29.97) < 0.01);
        
        QCOMPARE(frameRateDouble(FrameRate::FPS_30), 30.0);
        QCOMPARE(frameRateDouble(FrameRate::FPS_60), 60.0);
    }
    
    void test_frameIntervalUs() {
        // 30fps → 33333μs
        QCOMPARE(frameIntervalUs(FrameRate::FPS_30), qint64(33333));
        
        // 25fps → 40000μs
        QCOMPARE(frameIntervalUs(FrameRate::FPS_25), qint64(40000));
        
        // 24fps → 41667μs
        QCOMPARE(frameIntervalUs(FrameRate::FPS_24), qint64(41667));
        
        // 60fps → 16667μs
        QCOMPARE(frameIntervalUs(FrameRate::FPS_60), qint64(16667));
    }
    
    // ===== Universe 编解码 =====
    void test_universeEncode() {
        // (0, 0, 0) → 0x0000
        QCOMPARE(ArtNet::encodeUniverse(0, 0, 0), quint16(0x0000));
        
        // (0, 0, 1) → 0x0001
        QCOMPARE(ArtNet::encodeUniverse(0, 0, 1), quint16(0x0001));
        
        // (0, 1, 0) → 0x0010  (SubNet 左移 4 bit)
        QCOMPARE(ArtNet::encodeUniverse(0, 1, 0), quint16(0x0010));
        
        // (0, 1, 2) → 0x0012
        QCOMPARE(ArtNet::encodeUniverse(0, 1, 2), quint16(0x0012));
        
        // (1, 0, 0) → 0x0100  (Net 左移 8 bit)
        QCOMPARE(ArtNet::encodeUniverse(1, 0, 0), quint16(0x0100));
        
        // (1, 2, 3) → 0x0123
        QCOMPARE(ArtNet::encodeUniverse(1, 2, 3), quint16(0x0123));
        
        // (127, 15, 15) → 0x7FFF  (最大值)
        QCOMPARE(ArtNet::encodeUniverse(127, 15, 15), quint16(0x7FFF));
        
        // 截断测试: Net 超过 7bit 应该被 & 0x7F 截断
        // 输入 0xFF 应该只保留低 7 bit → 0x7F
        quint16 val = ArtNet::encodeUniverse(0xFF, 0x0F, 0x0F);
        QCOMPARE(val, quint16(0x7FFF));
    }
    
    void test_universeDecode() {
        // 0x0000 → (0, 0, 0)
        QCOMPARE(ArtNet::universeNet(0x0000), quint8(0));
        QCOMPARE(ArtNet::universeSubNet(0x0000), quint8(0));
        QCOMPARE(ArtNet::universeNumber(0x0000), quint8(0));
        
        // 0x0123 → Net=1, SubNet=2, Uni=3
        QCOMPARE(ArtNet::universeNet(0x0123), quint8(1));
        QCOMPARE(ArtNet::universeSubNet(0x0123), quint8(2));
        QCOMPARE(ArtNet::universeNumber(0x0123), quint8(3));
    }
    
    void test_universeRoundTrip() {
        // 编解码 round-trip 必须恒等
        quint8 nets[]    = {0, 1, 5, 63, 127};
        quint8 subs[]    = {0, 1, 3, 7, 15};
        quint8 univers[] = {0, 1, 5, 10, 15};
        
        for (auto net : nets) {
            for (auto sub : subs) {
                for (auto uni : univers) {
                    quint16 encoded = ArtNet::encodeUniverse(net, sub, uni);
                    QCOMPARE(ArtNet::universeNet(encoded), net);
                    QCOMPARE(ArtNet::universeSubNet(encoded), sub);
                    QCOMPARE(ArtNet::universeNumber(encoded), uni);
                }
            }
        }
    }
    
    // ===== 时间码 (non-drop-frame) =====
    void test_timeCode_basic() {
        TimeCode tc;
        tc.frameRate = 30;
        
        // 01:00:00:00 @ 30fps → 108000 frames
        tc.hours = 1; tc.minutes = 0; tc.seconds = 0; tc.frames = 0;
        QCOMPARE(tc.toTotalFrames(), qint64(1 * 3600 * 30));
        
        // 00:00:01:00 @ 30fps → 30 frames
        tc.hours = 0; tc.minutes = 0; tc.seconds = 1; tc.frames = 0;
        QCOMPARE(tc.toTotalFrames(), qint64(30));
        
        // 00:01:00:00 @ 30fps → 1800 frames
        tc.hours = 0; tc.minutes = 1; tc.seconds = 0; tc.frames = 0;
        QCOMPARE(tc.toTotalFrames(), qint64(1800));
    }
    
    void test_timeCode_fromTotalFrames() {
        // 108000 frames @ 30fps → 01:00:00:00
        TimeCode tc = TimeCode::fromTotalFrames(108000, 30, false);
        QCOMPARE(tc.hours, 1);
        QCOMPARE(tc.minutes, 0);
        QCOMPARE(tc.seconds, 0);
        QCOMPARE(tc.frames, 0);
        QCOMPARE(tc.frameRate, 30);
        QCOMPARE(tc.dropFrame, false);
    }
    
    void test_timeCode_roundTrip_30fps() {
        // 生成若干已知时间码 → totalFrames → fromTotalFrames → 比较
        int testCases[][4] = {
            {0, 0, 0, 0},
            {0, 0, 0, 15},
            {0, 0, 1, 0},
            {0, 1, 0, 0},
            {1, 0, 0, 0},
            {23, 59, 59, 29}
        };
        
        for (auto &tcData : testCases) {
            TimeCode orig;
            orig.hours = tcData[0]; orig.minutes = tcData[1];
            orig.seconds = tcData[2]; orig.frames = tcData[3];
            orig.frameRate = 30;
            orig.dropFrame = false;
            
            qint64 total = orig.toTotalFrames();
            TimeCode back = TimeCode::fromTotalFrames(total, 30, false);
            
            QCOMPARE(back.hours, orig.hours);
            QCOMPARE(back.minutes, orig.minutes);
            QCOMPARE(back.seconds, orig.seconds);
            QCOMPARE(back.frames, orig.frames);
        }
    }
    
    void test_timeCode_roundTrip_25fps() {
        // 25fps: 24小时 = 25*60*60*24 = 2,160,000 frames
        TimeCode orig;
        orig.hours = 1; orig.minutes = 30; orig.seconds = 15; orig.frames = 12;
        orig.frameRate = 25;
        orig.dropFrame = false;
        
        qint64 total = orig.toTotalFrames();
        TimeCode back = TimeCode::fromTotalFrames(total, 25, false);
        
        QCOMPARE(back.hours, orig.hours);
        QCOMPARE(back.minutes, orig.minutes);
        QCOMPARE(back.seconds, orig.seconds);
        QCOMPARE(back.frames, orig.frames);
    }
    
    void test_timeCode_dropFrame_2997() {
        // 29.97 drop-frame: 每分钟实际有 1798 帧 (丢 2 帧)
        // 但 fromTotalFrames/toTotalFrames 必须 round-trip
        TimeCode orig;
        orig.hours = 0; orig.minutes = 10; orig.seconds = 0; orig.frames = 0;
        orig.frameRate = 29;
        orig.dropFrame = true;
        
        qint64 total = orig.toTotalFrames();
        TimeCode back = TimeCode::fromTotalFrames(total, 29, true);
        
        QCOMPARE(back.hours, orig.hours);
        QCOMPARE(back.minutes, orig.minutes);
        QCOMPARE(back.seconds, orig.seconds);
        QCOMPARE(back.frames, orig.frames);
    }
    
    void test_timeCode_dropFrame_edge() {
        // 丢帧边界: 每分钟前两帧不存在 (frames=0,1 在非 10 倍数分钟无效)
        // toTotalFrames 假设输入是合法的时间码
        // 测试: 00:01:00:02 @ drop-frame = 合法
        TimeCode tc;
        tc.hours = 0; tc.minutes = 1; tc.seconds = 0; tc.frames = 2;
        tc.frameRate = 29;
        tc.dropFrame = true;
        
        qint64 total = tc.toTotalFrames();
        TimeCode back = TimeCode::fromTotalFrames(total, 29, true);
        
        QCOMPARE(back.hours, tc.hours);
        QCOMPARE(back.minutes, tc.minutes);
        QCOMPARE(back.seconds, tc.seconds);
        QCOMPARE(back.frames, tc.frames);
    }
    
    void test_timeCode_toString() {
        TimeCode tc;
        tc.hours = 1; tc.minutes = 2; tc.seconds = 3; tc.frames = 4;
        tc.frameRate = 30;
        
        QCOMPARE(tc.toString(), QString("01:02:03:04"));
        
        tc.hours = 0; tc.minutes = 0; tc.seconds = 0; tc.frames = 0;
        QCOMPARE(tc.toString(), QString("00:00:00:00"));
    }
    
    // ===== RecordState =====
    void test_recordStateToString() {
        QCOMPARE(recordStateToString(RecordState::Idle), QString("Idle"));
        QCOMPARE(recordStateToString(RecordState::Recording), QString("Recording"));
        QCOMPARE(recordStateToString(RecordState::Stopped), QString("Stopped"));
    }
    
    // ===== ArtNetDmxFrame 零初始化 =====
    void test_artNetDmxFrameInit() {
        ArtNetDmxFrame frame;
        QCOMPARE(frame.sequence, quint8(0));
        QCOMPARE(frame.universe, quint16(0));
        QCOMPARE(frame.length, quint16(0));
        QCOMPARE(frame.dmxData[0], quint8(0));
        QCOMPARE(frame.dmxData[511], quint8(0));
    }
};

QTEST_MAIN(TestCommon)
#include "TestCommon.moc"
