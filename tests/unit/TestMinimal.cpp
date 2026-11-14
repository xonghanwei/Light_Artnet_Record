// TestMinimal.cpp - 极简 test for debugging hang
#include <QtTest>
#include "recorder/RecorderEngine.h"
#include <QTemporaryDir>
#include <QThread>

class TestMinimal : public QObject {
    Q_OBJECT
private slots:
    void test_mini() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        
        RecorderEngine engine;
        RecorderEngine::Config cfg;
        cfg.outputFilePath = dir.path() + "/mini.artrec";
        cfg.frameRate = 30;
        cfg.universes = { encodeUniverse(0, 0, 0) };
        cfg.artNetPort = 26654;

        qDebug() << "Starting...";
        QVERIFY(engine.start(cfg));
        qDebug() << "Started, state =" << (int)engine.state();

        QThread::msleep(200);
        qDebug() << "After sleep, frames =" << engine.recordedFrames();

        engine.stop();
        qDebug() << "Stopped";
    }
};

QTEST_MAIN(TestMinimal)
#include "TestMinimal.moc"
