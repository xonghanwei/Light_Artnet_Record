// SyncClockTestWindow.h — 同步时钟功能测试面板
// 第一个继承 TestWindowBase 的具体示例, 后续测试面板照此模式
#ifndef SYNC_CLOCK_TEST_WINDOW_H
#define SYNC_CLOCK_TEST_WINDOW_H

#include "../base/TestWindowBase.h"
#include "sync/ISyncClock.h"
#include "sync/SyncClockFactory.h"

#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>

class SyncClockTestWindow : public TestWindowBase {
    Q_OBJECT
public:
    explicit SyncClockTestWindow(QWidget *parent = nullptr);

protected:
    void setupContent() override;
    void onStartClicked() override;
    void onStopClicked() override;
    void onResetClicked() override;

private slots:
    void onFrameTick(qint64 frameIndex, qint64 timestampUs);
    void onStatusChanged(const QString &msg);
    void onClockFinished();

private:
    ISyncClock *m_clock = nullptr;

    // 控件 (子类 own)
    QComboBox *m_cmbClockType = nullptr;
    QSpinBox  *m_spinFps      = nullptr;
    QSpinBox  *m_spinArtNetPort = nullptr;
    QLineEdit *m_editBindAddr = nullptr;
    QSpinBox  *m_spinDuration = nullptr;

    QLabel    *m_lblTickCount = nullptr;
    QLabel    *m_lblLastTs    = nullptr;
    QLabel    *m_lblLastFrame = nullptr;

    int  m_tickCount = 0;
};

#endif // SYNC_CLOCK_TEST_WINDOW_H
