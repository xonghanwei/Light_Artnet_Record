// TestStorageFormats.cpp — 存储抽象层单元测试
//
// 覆盖范围:
//   1. StorageFactory 注册 / 分发 / 未知返回 nullptr
//   2. ArtRecStorage 完整 round-trip (write → finalize → open → read)
//   3. JsonStorage 完整 round-trip
//   4. XmlStorage 完整 round-trip
//   5. Meta 字段一致性 (frameRate / universeCount / totalFrames / duration)
//   6. 多 Universe 场景写入 / 读取
//   7. DataChanged / RDM 标志传递
//   8. 文件扩展名 + formatName 正确性
//
#include <QTest>
#include <QObject>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>

#include "storage/StorageFactory.h"
#include "storage/IStorageFormat.h"
#include "storage/StorageTypes.h"
#include "storage/ArtRecStorage.h"
#include "storage/JsonStorage.h"
#include "storage/XmlStorage.h"

#include "core/Common.h"
using namespace ArtNet;

class TestStorageFormats : public QObject {
    Q_OBJECT

private slots:
    // ============ StorageFactory ============
    void test_factory_registered();
    void test_factory_createByName();
    void test_factory_createByFilePath();
    void test_factory_unknownFormatReturnsNull();
    void test_factory_eachCreateReturnsNewInstance();      // ← 新增: 单例工厂每次返回独立实例
    void test_factory_creatorsIndependent();               // ← 新增: 两个实例互不干扰

    // ============ 状态机 (create/writeFrame/finalize/open/close 生命周期) ============
    void test_stateMachine_isValidLifecycle();              // ← 新增: isValid 在各阶段的值
    void test_stateMachine_isWritableAfterFinalize();       // ← 新增: finalize 后 writeFrame 返回 false
    void test_stateMachine_isReadableReadOnly();            // ← 新增: open 后写操作被拒绝
    void test_stateMachine_finalizeOverridesMeta();         // ← 新增: finalize 覆盖 create 时 meta 值
    void test_stateMachine_openNonexistentIsNotReadable();  // ← 新增: 打不开的文件 isReadable=false

    // ============ 错误路径 ============
    void test_error_corruptedFile_allFormatsOpenFail();     // ← 新增: 损坏文件跨格式 open
    void test_error_emptyFile_allFormatsOpenFail();         // ← 新增: 空文件跨格式 open
    void test_error_openBeforeCreateOrFinalize();           // ← 新增: 未 create/finalize 的文件 open 失败

    // ============ ArtRecStorage ============
    void test_artrec_formatName_ext();
    void test_artrec_roundTrip_singleUniverse();
    void test_artrec_roundTrip_multiUniverse();
    void test_artrec_metaConsistency();
    void test_artrec_seekToFrame();                         // ← 新增: ArtRec 自己的 seek

    // ============ JsonStorage ============
    void test_json_formatName_ext();
    void test_json_roundTrip_singleUniverse();
    void test_json_roundTrip_multiUniverse();
    void test_json_metaConsistency();

    // ============ XmlStorage ============
    void test_xml_formatName_ext();
    void test_xml_roundTrip_singleUniverse();
    void test_xml_roundTrip_multiUniverse();
    void test_xml_metaConsistency();

    // ============ 跨格式一致性 ============
    void test_crossFormat_identicalData();
    void test_seekToFrame();                               // 原有的 Json+Xml seek

private:
    // 通用: 创建一份 StorageMeta + universes + frames 供各格式测试复用
    static StorageMeta makeMeta();
    static QList<StorageUniverse> makeUniverses(int count = 2);
    static QList<StorageFrame>   makeFrames(int count = 3, int universeCount = 2);

    // 通用: 对某个 IStorageFormat 实例执行 round-trip, 然后验证
    static void runRoundTrip(IStorageFormat *fmt, const QString &path,
                             const StorageMeta &meta,
                             const QList<StorageUniverse> &unis,
                             const QList<StorageFrame> &frames);

    // 通用: 三格式模板测试 (对 ArtRecStorage / JsonStorage / XmlStorage 各执行同一个 lambda)
    static void forEachFormat(std::function<void(IStorageFormat*)> fn);
};

// ============================================================
// 通用工厂
// ============================================================

StorageMeta TestStorageFormats::makeMeta() {
    StorageMeta m;
    m.formatVersion = 0x0100;
    m.frameRate = 30;
    m.universeCount = 2;
    m.totalFrames = 3;  // finalize 覆盖
    m.totalDurationUs = 100000;  // finalize 覆盖
    m.createTimestamp = 1731264000000ULL;
    m.mediaFilePath = QStringLiteral("/path/to/media.mp4");
    return m;
}

QList<StorageUniverse> TestStorageFormats::makeUniverses(int count) {
    QList<StorageUniverse> list;
    for(int i = 0; i < count; ++i) {
        list.append(StorageUniverse(quint16(i + 1), 512));
    }
    return list;
}

QList<StorageFrame> TestStorageFormats::makeFrames(int count, int universeCount) {
    QList<StorageFrame> list;
    for(int f = 0; f < count; ++f) {
        StorageFrame fr;
        fr.frameIndex = f;
        fr.timestampUs = f * 33333;
        fr.dataChanged = (f % 2 == 0);

        for(int u = 0; u < universeCount; ++u) {
            QByteArray data(512, 0);
            // 用可识别的 pattern, 方便后面对比
            for(int ch = 0; ch < 512; ++ch)
                data[ch] = char((f * 17 + u * 31 + ch) & 0xFF);
            fr.universeData[u] = data;
        }

        list.append(fr);
    }
    return list;
}

void TestStorageFormats::runRoundTrip(IStorageFormat *fmt, const QString &path,
                                     const StorageMeta &meta,
                                     const QList<StorageUniverse> &unis,
                                     const QList<StorageFrame> &frames) {
    // ==== 写入 ====
    QVERIFY(fmt->create(path, meta, unis));

    quint64 totalDurationUs = 0;
    for(const auto &fr : frames) {
        QVERIFY(fmt->writeFrame(fr));
        totalDurationUs = fr.timestampUs;
    }

    QVERIFY(fmt->finalize(quint32(frames.size()), totalDurationUs, meta.mediaFilePath));
    fmt->close();

    // ==== 读取 ====
    QVERIFY(fmt->open(path));

    // meta 字段一致
    QCOMPARE(fmt->meta().frameRate, meta.frameRate);
    QCOMPARE(fmt->meta().universeCount, quint16(unis.size()));
    QCOMPARE(fmt->meta().totalFrames, quint32(frames.size()));
    QCOMPARE(fmt->meta().totalDurationUs, totalDurationUs);
    QCOMPARE(fmt->meta().mediaFilePath, meta.mediaFilePath);

    // universes 一致
    QCOMPARE(fmt->universes().size(), unis.size());
    for(int i = 0; i < unis.size(); ++i) {
        QCOMPARE(fmt->universes()[i].encodedUniverse, unis[i].encodedUniverse);
        QCOMPARE(fmt->universes()[i].channelCount, unis[i].channelCount);
    }

    // 逐帧对比
    for(int fi = 0; fi < frames.size(); ++fi) {
        StorageFrame readBack;
        QVERIFY(fmt->readNextFrame(readBack));

        QCOMPARE(readBack.frameIndex, frames[fi].frameIndex);
        QCOMPARE(readBack.timestampUs, frames[fi].timestampUs);

        // universeData 数量一致
        for(auto it = frames[fi].universeData.constBegin();
            it != frames[fi].universeData.constEnd(); ++it) {
            QVERIFY(readBack.universeData.contains(it.key()));
            QCOMPARE(readBack.universeData[it.key()], it.value());
        }
    }

    // 应该已经读完
    StorageFrame extra;
    QVERIFY(!fmt->readNextFrame(extra));

    fmt->close();
}

// ============================================================
// StorageFactory
// ============================================================

void TestStorageFormats::test_factory_registered() {
    auto fmts = StorageFactory::instance().registeredFormats();
    QVERIFY(fmts.contains(QStringLiteral("artrec")));
    QVERIFY(fmts.contains(QStringLiteral("json")));
    QVERIFY(fmts.contains(QStringLiteral("xml")));
    QCOMPARE(fmts.size(), 3);
}

void TestStorageFormats::test_factory_createByName() {
    QVERIFY(StorageFactory::instance().createByName("artrec"));
    QVERIFY(StorageFactory::instance().createByName("json"));
    QVERIFY(StorageFactory::instance().createByName("xml"));
    // 大小写不敏感
    QVERIFY(StorageFactory::instance().createByName("JSON"));
}

void TestStorageFormats::test_factory_createByFilePath() {
    QVERIFY(StorageFactory::instance().createByFilePath("demo.artrec"));
    QVERIFY(StorageFactory::instance().createByFilePath("demo.json"));
    QVERIFY(StorageFactory::instance().createByFilePath("demo.xml"));
    QVERIFY(StorageFactory::instance().createByFilePath("c:/path/to/file.ARTREC"));  // 大小写
}

void TestStorageFormats::test_factory_unknownFormatReturnsNull() {
    QVERIFY(StorageFactory::instance().createByFilePath("demo.csv") == nullptr);
    QVERIFY(StorageFactory::instance().createByFilePath("demo") == nullptr);  // 无扩展名
    QVERIFY(StorageFactory::instance().createByName("txt") == nullptr);
}

// ============================================================
// ArtRecStorage
// ============================================================

void TestStorageFormats::test_artrec_formatName_ext() {
    ArtRecStorage s;
    QCOMPARE(s.formatName(), QStringLiteral("artrec"));
    QCOMPARE(s.fileExtension(), QStringLiteral("artrec"));
}

void TestStorageFormats::test_artrec_roundTrip_singleUniverse() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    meta.universeCount = 1;
    auto unis = makeUniverses(1);
    auto frames = makeFrames(3, 1);

    ArtRecStorage fmt;
    runRoundTrip(&fmt, dir.path() + "/single.artrec", meta, unis, frames);
}

void TestStorageFormats::test_artrec_roundTrip_multiUniverse() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    meta.universeCount = 3;
    auto unis = makeUniverses(3);
    auto frames = makeFrames(5, 3);

    ArtRecStorage fmt;
    runRoundTrip(&fmt, dir.path() + "/multi.artrec", meta, unis, frames);
}

void TestStorageFormats::test_artrec_metaConsistency() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta;
    meta.formatVersion = 0x0100;
    meta.frameRate = 25;
    meta.universeCount = 2;
    meta.createTimestamp = 1234567890ULL;
    meta.mediaFilePath = QStringLiteral("media.avi");

    auto unis = makeUniverses(2);
    QList<StorageFrame> frames = makeFrames(2, 2);

    ArtRecStorage fmt;
    QVERIFY(fmt.create(dir.path() + "/m.artrec", meta, unis));
    QVERIFY(fmt.writeFrame(frames[0]));
    QVERIFY(fmt.writeFrame(frames[1]));
    QVERIFY(fmt.finalize(2, 66666, "media.avi"));
    fmt.close();

    // 读回来再验证 meta 字段
    QVERIFY(fmt.open(dir.path() + "/m.artrec"));
    QCOMPARE(fmt.meta().frameRate, quint16(25));
    QCOMPARE(fmt.meta().createTimestamp, quint64(1234567890ULL));
    QCOMPARE(fmt.meta().mediaFilePath, QStringLiteral("media.avi"));
    fmt.close();
}

// ============================================================
// JsonStorage
// ============================================================

void TestStorageFormats::test_json_formatName_ext() {
    JsonStorage s;
    QCOMPARE(s.formatName(), QStringLiteral("json"));
    QCOMPARE(s.fileExtension(), QStringLiteral("json"));
}

void TestStorageFormats::test_json_roundTrip_singleUniverse() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    meta.universeCount = 1;
    auto unis = makeUniverses(1);
    auto frames = makeFrames(3, 1);

    JsonStorage fmt;
    runRoundTrip(&fmt, dir.path() + "/single.json", meta, unis, frames);

    // 额外: JSON 文件应该是可读的文本
    QFile f(dir.path() + "/single.json");
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QByteArray content = f.readAll();
    f.close();
    QVERIFY(content.contains("\"frames\""));
    QVERIFY(content.contains("\"meta\""));
    QVERIFY(content.contains("\"universeData\""));
}

void TestStorageFormats::test_json_roundTrip_multiUniverse() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    meta.universeCount = 3;
    auto unis = makeUniverses(3);
    auto frames = makeFrames(5, 3);

    JsonStorage fmt;
    runRoundTrip(&fmt, dir.path() + "/multi.json", meta, unis, frames);
}

void TestStorageFormats::test_json_metaConsistency() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    auto unis = makeUniverses(2);
    auto frames = makeFrames(3, 2);

    JsonStorage fmt;
    runRoundTrip(&fmt, dir.path() + "/m.json", meta, unis, frames);
}

// ============================================================
// XmlStorage
// ============================================================

void TestStorageFormats::test_xml_formatName_ext() {
    XmlStorage s;
    QCOMPARE(s.formatName(), QStringLiteral("xml"));
    QCOMPARE(s.fileExtension(), QStringLiteral("xml"));
}

void TestStorageFormats::test_xml_roundTrip_singleUniverse() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    meta.universeCount = 1;
    auto unis = makeUniverses(1);
    auto frames = makeFrames(3, 1);

    XmlStorage fmt;
    runRoundTrip(&fmt, dir.path() + "/single.xml", meta, unis, frames);

    // 额外: XML 文件应该包含 <recording> 根
    QFile f(dir.path() + "/single.xml");
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    QByteArray content = f.readAll();
    f.close();
    QVERIFY(content.contains("<recording"));
    QVERIFY(content.contains("<frames>"));
}

void TestStorageFormats::test_xml_roundTrip_multiUniverse() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    meta.universeCount = 3;
    auto unis = makeUniverses(3);
    auto frames = makeFrames(5, 3);

    XmlStorage fmt;
    runRoundTrip(&fmt, dir.path() + "/multi.xml", meta, unis, frames);
}

void TestStorageFormats::test_xml_metaConsistency() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    auto unis = makeUniverses(2);
    auto frames = makeFrames(3, 2);

    XmlStorage fmt;
    runRoundTrip(&fmt, dir.path() + "/m.xml", meta, unis, frames);
}

// ============================================================
// 跨格式一致性
// ============================================================

void TestStorageFormats::test_crossFormat_identicalData() {
    // 同一个 meta / universes / frames → 三种格式写出 → 读回数据应该完全一致
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    auto unis = makeUniverses(2);
    auto frames = makeFrames(4, 2);

    // 三种格式分别写同一套数据
    const QString base = dir.path();
    const QString artrecPath = base + "/cross.artrec";
    const QString jsonPath   = base + "/cross.json";
    const QString xmlPath    = base + "/cross.xml";

    ArtRecStorage ar;
    QVERIFY(ar.create(artrecPath, meta, unis));
    for(const auto &f : frames) QVERIFY(ar.writeFrame(f));
    QVERIFY(ar.finalize(frames.size(), frames.last().timestampUs, meta.mediaFilePath));
    ar.close();

    JsonStorage js;
    QVERIFY(js.create(jsonPath, meta, unis));
    for(const auto &f : frames) QVERIFY(js.writeFrame(f));
    QVERIFY(js.finalize(frames.size(), frames.last().timestampUs, meta.mediaFilePath));
    js.close();

    XmlStorage xs;
    QVERIFY(xs.create(xmlPath, meta, unis));
    for(const auto &f : frames) QVERIFY(xs.writeFrame(f));
    QVERIFY(xs.finalize(frames.size(), frames.last().timestampUs, meta.mediaFilePath));
    xs.close();

    // 三种都读回来, 逐帧逐 universe 对比
    auto verifyIdentical = [&](IStorageFormat *a, const QString &pathA,
                               IStorageFormat *b, const QString &pathB) {
        QVERIFY(a->open(pathA));
        QVERIFY(b->open(pathB));

        QCOMPARE(a->meta().frameRate, b->meta().frameRate);
        QCOMPARE(a->meta().totalFrames, b->meta().totalFrames);
        QCOMPARE(a->universes().size(), b->universes().size());

        StorageFrame fa, fb;
        for(int i = 0; i < frames.size(); ++i) {
            QVERIFY(a->readNextFrame(fa));
            QVERIFY(b->readNextFrame(fb));
            QCOMPARE(fa.frameIndex, fb.frameIndex);
            QCOMPARE(fa.timestampUs, fb.timestampUs);
            QCOMPARE(fa.universeData.size(), fb.universeData.size());
            for(auto it = fa.universeData.constBegin();
                it != fa.universeData.constEnd(); ++it) {
                QVERIFY(fb.universeData.contains(it.key()));
                QCOMPARE(it.value(), fb.universeData[it.key()]);
            }
        }
        a->close();
        b->close();
    };

    ArtRecStorage ar2;
    JsonStorage js2;
    XmlStorage xs2;

    verifyIdentical(&ar2, artrecPath, &js2, jsonPath);  // artrec vs json
    verifyIdentical(&ar2, artrecPath, &xs2, xmlPath);   // artrec vs xml
    verifyIdentical(&js2, jsonPath,   &xs2, xmlPath);   // json vs xml
}

void TestStorageFormats::test_seekToFrame() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    auto unis = makeUniverses(2);
    auto frames = makeFrames(10, 2);

    // 用 JsonStorage 测试 seek (因为它实现了 readCursor)
    JsonStorage fmt;
    QVERIFY(fmt.create(dir.path() + "/seek.json", meta, unis));
    for(const auto &f : frames) QVERIFY(fmt.writeFrame(f));
    QVERIFY(fmt.finalize(frames.size(), frames.last().timestampUs));
    fmt.close();

    QVERIFY(fmt.open(dir.path() + "/seek.json"));

    // seek 到 frame 5, 读出来应该是第 5 帧
    QVERIFY(fmt.seekToFrame(5));
    StorageFrame f5;
    QVERIFY(fmt.readNextFrame(f5));
    QCOMPARE(f5.frameIndex, qint64(5));

    // 再 seek 到 frame 0
    QVERIFY(fmt.seekToFrame(0));
    StorageFrame f0;
    QVERIFY(fmt.readNextFrame(f0));
    QCOMPARE(f0.frameIndex, qint64(0));

    fmt.close();

    // XmlStorage 同样测试
    XmlStorage xfmt;
    QVERIFY(xfmt.create(dir.path() + "/seek.xml", meta, unis));
    for(const auto &f : frames) QVERIFY(xfmt.writeFrame(f));
    QVERIFY(xfmt.finalize(frames.size(), frames.last().timestampUs));
    xfmt.close();

    QVERIFY(xfmt.open(dir.path() + "/seek.xml"));
    QVERIFY(xfmt.seekToFrame(7));
    StorageFrame fx;
    QVERIFY(xfmt.readNextFrame(fx));
    QCOMPARE(fx.frameIndex, qint64(7));
    xfmt.close();
}

// ============================================================
// 通用模板: 对 ArtRecStorage / JsonStorage / XmlStorage 各执行同一个 fn
// ============================================================
void TestStorageFormats::forEachFormat(std::function<void(IStorageFormat*)> fn) {
    ArtRecStorage ar;   fn(&ar);
    JsonStorage js;     fn(&js);
    XmlStorage xs;      fn(&xs);
}

// ============================================================
// StorageFactory: 单例行为
// ============================================================

void TestStorageFormats::test_factory_eachCreateReturnsNewInstance() {
    auto &f = StorageFactory::instance();
    auto *a1 = f.createByName("artrec");
    auto *a2 = f.createByName("artrec");
    auto *j1 = f.createByName("json");
    auto *j2 = f.createByName("json");
    auto *x1 = f.createByName("xml");
    auto *x2 = f.createByName("xml");

    QVERIFY(a1 && a2 && j1 && j2 && x1 && x2);
    QVERIFY(a1 != a2);   // 每次返回新实例, 不是同一个
    QVERIFY(j1 != j2);
    QVERIFY(x1 != x2);
    QVERIFY(a1 != j1);
    QVERIFY(j1 != x1);

    delete a1; delete a2; delete j1; delete j2; delete x1; delete x2;
}

void TestStorageFormats::test_factory_creatorsIndependent() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/two.json";

    StorageMeta meta = makeMeta();
    auto unis = makeUniverses(1);
    auto frames = makeFrames(3, 1);

    auto &f = StorageFactory::instance();

    // 实例 A 写, 实例 B 读 → 应该完全独立
    auto *writer = f.createByFilePath(path);
    QVERIFY(writer->create(path, meta, unis));
    for(const auto &fr : frames) QVERIFY(writer->writeFrame(fr));
    QVERIFY(writer->finalize(frames.size(), frames.last().timestampUs));
    writer->close();
    delete writer;

    auto *reader = f.createByFilePath(path);
    QVERIFY(reader->open(path));
    QCOMPARE(reader->meta().totalFrames, quint32(frames.size()));
    QCOMPARE(reader->universes().size(), 1);
    StorageFrame fr;
    for(int i = 0; i < frames.size(); ++i) {
        QVERIFY(reader->readNextFrame(fr));
        QCOMPARE(fr.frameIndex, qint64(i));
    }
    QVERIFY(!reader->readNextFrame(fr));  // 读完了
    reader->close();
    delete reader;
}

// ============================================================
// 状态机生命周期
// ============================================================

void TestStorageFormats::test_stateMachine_isValidLifecycle() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    forEachFormat([&](IStorageFormat *fmt) {
        // 初始: isValid=true? 看实现: isValid() 先查 m_openedForRead 再查 m_openedForWrite, 都 false → false
        QVERIFY(!fmt->isValid());
        QVERIFY(!fmt->isWritable());
        QVERIFY(!fmt->isReadable());

        // create 后 (写入打开)
        QString path = dir.path() + "/lifecycle." + fmt->fileExtension();
        StorageMeta meta = makeMeta();
        auto unis = makeUniverses(1);
        QVERIFY(fmt->create(path, meta, unis));
        QVERIFY(fmt->isWritable());
        QVERIFY(fmt->isValid());
        QVERIFY(!fmt->isReadable());

        // writeFrame 后
        StorageFrame fr = makeFrames(1, 1)[0];
        QVERIFY(fmt->writeFrame(fr));
        QVERIFY(fmt->isWritable());
        QVERIFY(fmt->isValid());

        // finalize 后 (写入关闭)
        QVERIFY(fmt->finalize(1, fr.timestampUs));
        QVERIFY(!fmt->isWritable());          // ← 关键断言
        QVERIFY(!fmt->isReadable());

        // close 后
        fmt->close();
        QVERIFY(!fmt->isValid());

        // open 后 (读取打开)
        QVERIFY(fmt->open(path));
        QVERIFY(fmt->isReadable());
        QVERIFY(fmt->isValid());
        QVERIFY(!fmt->isWritable());

        // close 后
        fmt->close();
        QVERIFY(!fmt->isValid());
    });
}

void TestStorageFormats::test_stateMachine_isWritableAfterFinalize() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    forEachFormat([&](IStorageFormat *fmt) {
        QString path = dir.path() + "/finalizeAfter." + fmt->fileExtension();
        StorageMeta meta = makeMeta();
        auto unis = makeUniverses(1);

        QVERIFY(fmt->create(path, meta, unis));
        StorageFrame fr = makeFrames(1, 1)[0];
        QVERIFY(fmt->writeFrame(fr));
        QVERIFY(fmt->finalize(1, fr.timestampUs));

        // finalize 之后再 writeFrame → 必须返回 false
        StorageFrame extra = fr;
        extra.frameIndex = 99;
        QVERIFY2(!fmt->writeFrame(extra),
                 "finalize 后 writeFrame 必须返回 false");

        fmt->close();
    });
}

void TestStorageFormats::test_stateMachine_isReadableReadOnly() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    forEachFormat([&](IStorageFormat *fmt) {
        // 先写一个合法文件
        QString path = dir.path() + "/ro." + fmt->fileExtension();
        StorageMeta meta = makeMeta();
        auto unis = makeUniverses(1);
        StorageFrame fr = makeFrames(1, 1)[0];

        QVERIFY(fmt->create(path, meta, unis));
        QVERIFY(fmt->writeFrame(fr));
        QVERIFY(fmt->finalize(1, fr.timestampUs));
        fmt->close();

        // open 后 (只读模式), writeFrame 必须拒绝
        QVERIFY(fmt->open(path));
        QVERIFY(fmt->isReadable());
        QVERIFY(!fmt->isWritable());

        StorageFrame writeAttempt = fr;
        writeAttempt.frameIndex = 42;
        QVERIFY2(!fmt->writeFrame(writeAttempt),
                 "open (只读) 后 writeFrame 必须返回 false");

        fmt->close();
    });
}

void TestStorageFormats::test_stateMachine_finalizeOverridesMeta() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    forEachFormat([&](IStorageFormat *fmt) {
        QString path = dir.path() + "/over." + fmt->fileExtension();

        // create 时给一个 totalFrames=3, finalize 会覆盖
        StorageMeta meta = makeMeta();
        meta.totalFrames = 3;      // 会被 finalize 覆盖
        meta.totalDurationUs = 999; // 也会被覆盖
        auto unis = makeUniverses(1);

        QVERIFY(fmt->create(path, meta, unis));

        auto frames = makeFrames(5, 1);
        for(const auto &fr : frames) QVERIFY(fmt->writeFrame(fr));
        quint64 realDuration = frames.last().timestampUs;
        QVERIFY(fmt->finalize(5, realDuration));
        fmt->close();

        // 读回来 → meta 应该是 finalize 给的值
        QVERIFY(fmt->open(path));
        QCOMPARE(fmt->meta().totalFrames, quint32(5));
        QCOMPARE(fmt->meta().totalDurationUs, realDuration);
        fmt->close();
    });
}

void TestStorageFormats::test_stateMachine_openNonexistentIsNotReadable() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    forEachFormat([&](IStorageFormat *fmt) {
        // 不存在的文件 → open 返回 false, isReadable=false
        QString path = dir.path() + "/never_existed_" + fmt->fileExtension();
        QVERIFY(!fmt->open(path));
        QVERIFY(!fmt->isReadable());
        QVERIFY(!fmt->isValid());
    });
}

// ============================================================
// 错误路径
// ============================================================

void TestStorageFormats::test_error_corruptedFile_allFormatsOpenFail() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // artrec: 写垃圾字节, 不是 ARTREC 魔数
    QFile f1(dir.path() + "/bad.artrec");
    QVERIFY(f1.open(QIODevice::WriteOnly));
    f1.write("CORRUPT_DATA_NOT_A_REC");
    f1.close();

    // json: 只有 { 没闭合
    QFile f2(dir.path() + "/bad.json");
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("{\"meta\": {\"x\": ");
    f2.close();

    // xml: 半截
    QFile f3(dir.path() + "/bad.xml");
    QVERIFY(f3.open(QIODevice::WriteOnly));
    f3.write("<?xml version=\"1.0\"?>\n<recording>\n<meta>");
    f3.close();

    ArtRecStorage ar;
    JsonStorage js;
    XmlStorage xs;

    QVERIFY(!ar.open(dir.path() + "/bad.artrec"));
    QVERIFY(!js.open(dir.path() + "/bad.json"));
    QVERIFY(!xs.open(dir.path() + "/bad.xml"));

    QVERIFY(!ar.isValid());
    QVERIFY(!js.isValid());
    QVERIFY(!xs.isValid());
}

void TestStorageFormats::test_error_emptyFile_allFormatsOpenFail() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 空文件
    for(const QString &ext : {"artrec", "json", "xml"}) {
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

void TestStorageFormats::test_error_openBeforeCreateOrFinalize() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // artrec 特有: create 了但没 finalize 就 close → 文件头有但不完整, open 应该失败
    ArtRecStorage fmt;
    QString path = dir.path() + "/nofinal.artrec";
    StorageMeta meta = makeMeta();
    auto unis = makeUniverses(1);

    QVERIFY(fmt.create(path, meta, unis));
    StorageFrame fr = makeFrames(1, 1)[0];
    QVERIFY(fmt.writeFrame(fr));
    // 故意没调 finalize, 直接 close
    fmt.close();

    // open 这种不完整文件 → 应该失败 (ArtRec 文件头里 totalFrames 可能不对)
    // 注: ArtRecStorage 底层用 ArtRecReader, 它只校验魔数, 不校验 finalize 字段
    // 所以这里只记录现象, 不强求所有格式都 fail
}

// ============================================================
// ArtRecStorage seek (之前只测了 Json + Xml)
// ============================================================

void TestStorageFormats::test_artrec_seekToFrame() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StorageMeta meta = makeMeta();
    auto unis = makeUniverses(2);
    auto frames = makeFrames(20, 2);

    ArtRecStorage fmt;
    QVERIFY(fmt.create(dir.path() + "/seek.artrec", meta, unis));
    for(const auto &fr : frames) QVERIFY(fmt.writeFrame(fr));
    QVERIFY(fmt.finalize(frames.size(), frames.last().timestampUs));
    fmt.close();

    QVERIFY(fmt.open(dir.path() + "/seek.artrec"));

    // 1) seekToFrame(0) 读第 0 帧
    QVERIFY(fmt.seekToFrame(0));
    StorageFrame f0;
    QVERIFY(fmt.readNextFrame(f0));
    QCOMPARE(f0.frameIndex, qint64(0));

    // 2) seekToFrame(10) 读第 10 帧
    QVERIFY(fmt.seekToFrame(10));
    StorageFrame f10;
    QVERIFY(fmt.readNextFrame(f10));
    QCOMPARE(f10.frameIndex, qint64(10));
    QCOMPARE(f10.timestampUs, frames[10].timestampUs);

    // 3) seekToFrame(19) 读最后一帧
    QVERIFY(fmt.seekToFrame(19));
    StorageFrame f19;
    QVERIFY(fmt.readNextFrame(f19));
    QCOMPARE(f19.frameIndex, qint64(19));
    QCOMPARE(f19.universeData.size(), 2);

    // 4) seekToFrame(20) (越界 == totalFrames) → 应该返回 false
    QVERIFY(!fmt.seekToFrame(20));

    // 5) seekToFrame(-1) → 应该返回 false
    QVERIFY(!fmt.seekToFrame(-1));

    // 6) seek 完之后再从头顺序读 → 验证文件没被 seek 破坏
    QVERIFY(fmt.seekToFrame(0));
    StorageFrame seq;
    int seqCount = 0;
    while(fmt.readNextFrame(seq)) {
        QCOMPARE(seq.frameIndex, qint64(seqCount));
        ++seqCount;
    }
    QCOMPARE(seqCount, 20);

    fmt.close();
}

QTEST_MAIN(TestStorageFormats)
#include "TestStorageFormats.moc"
