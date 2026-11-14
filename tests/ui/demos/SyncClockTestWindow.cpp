// SyncClockTestWindow.cpp — 同步时钟功能测试面板实现
#include "SyncClockTestWindow.h"

#include <QDateTime>
#include <QFormLayout>

// ============================================================
SyncClockTestWindow::SyncClockTestWindow(QWidget *parent)
    : TestWindowBase(parent)
{
    setStatusNeutral(tr("等待配置"));
}

// ============================================================
void SyncClockTestWindow::setupContent() {
    // --- 配置分组 ---
    auto *cfgGroup = new QGroupBox(tr("时钟配置"), m_contentArea);
    auto *form = new QFormLayout(cfgGroup);
    form->setLabelAlignment(Qt::AlignRight);
    form->setSpacing(8);

    m_cmbClockType = new QComboBox(cfgGroup);
    m_cmbClockType->addItem(tr("Internal Timer (内部时钟)"),
                            int(SyncClockType::InternalTimer));
    m_cmbClockType->addItem(tr("ArtNet TimeCode"),
                            int(SyncClockType::ArtNetTimeCode));
    m_cmbClockType->addItem(tr("MIDI Time Code"),
                            int(SyncClockType::MidiMTC));
    m_cmbClockType->addItem(tr("Timecode File (.tc)"),
                            int(SyncClockType::TimecodeFile));
    form->addRow(tr("时钟类型:"), m_cmbClockType);

    m_spinFps = new QSpinBox(cfgGroup);
    m_spinFps->setRange(1, 120);
    m_spinFps->setValue(30);
    m_spinFps->setSingleStep(1);
    form->addRow(tr("帧率 (fps):"), m_spinFps);

    m_spinArtNetPort = new QSpinBox(cfgGroup);
    m_spinArtNetPort->setRange(1024, 65535);
    m_spinArtNetPort->setValue(6454);
    form->addRow(tr("ArtNet 端口:"), m_spinArtNetPort);

    m_editBindAddr = new QLineEdit(cfgGroup);
    m_editBindAddr->setText("0.0.0.0");
    form->addRow(tr("绑定地址:"), m_editBindAddr);

    m_spinDuration = new QSpinBox(cfgGroup);
    m_spinDuration->setRange(0, 3600);
    m_spinDuration->setValue(0);
    m_spinDuration->setSuffix(tr(" 秒 (0=不自动停)"));
    form->addRow(tr("测试时长:"), m_spinDuration);

    // 竖排布局
    auto *v = new QVBoxLayout(m_contentArea);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(cfgGroup);

    // --- 实时状态分组 ---
    auto *rtGroup = new QGroupBox(tr("实时状态"), m_contentArea);
    auto *rtForm = new QFormLayout(rtGroup);
    rtForm->setLabelAlignment(Qt::AlignRight);
    rtForm->setSpacing(8);

    m_lblTickCount = new QLabel(tr("0"), rtGroup);
    m_lblTickCount->setProperty("role", "status-ok");
    rtForm->addRow(tr("Tick 总数:"), m_lblTickCount);

    m_lblLastFrame = new QLabel(tr("-"), rtGroup);
    rtForm->addRow(tr("最后帧序号:"), m_lblLastFrame);

    m_lblLastTs = new QLabel(tr("-"), rtGroup);
    rtForm->addRow(tr("最后时间戳:"), m_lblLastTs);

    v->addWidget(rtGroup);
    v->addStretch();
}

// ============================================================
void SyncClockTestWindow::onStartClicked() {
    if (m_clock) {
        logWarn(tr("已有一个时钟在运行, 请先停止"));
        return;
    }

    auto type = SyncClockType(m_cmbClockType->currentData().toInt());

    SyncClockConfig cfg;
    cfg.clockType = type;
    cfg.frameRate = m_spinFps->value();
    cfg.artNetPort = quint16(m_spinArtNetPort->value());
    cfg.artNetBindAddress = m_editBindAddr->text();

    m_clock = SyncClockFactory::instance().create(type);
    if (!m_clock) {
        logError(tr("SyncClockFactory 创建失败, type=%1").arg(int(type)));
        setStatusError(tr("创建时钟失败"));
        return;
    }

    // 清理旧计数
    m_tickCount = 0;
    m_lblTickCount->setText("0");
    m_lblLastFrame->setText("-");
    m_lblLastTs->setText("-");

    connect(m_clock, &ISyncClock::frameTick,
            this, &SyncClockTestWindow::onFrameTick);
    connect(m_clock, &ISyncClock::statusChanged,
            this, &SyncClockTestWindow::onStatusChanged);
    connect(m_clock, &ISyncClock::clockFinished,
            this, &SyncClockTestWindow::onClockFinished);
    connect(m_clock, &ISyncClock::errorOccurred,
            this, [this](const QString &err) {
                logError(tr("时钟错误: %1").arg(err));
                setStatusError(err);
            });
    connect(m_clock, &ISyncClock::clockLooped,
            this, [this](int cnt) {
                logSuccess(tr("时钟循环回零 — 第 %1 次").arg(cnt));
            });

    if (!m_clock->start(cfg)) {
        logError(tr("时钟启动失败 (端口绑定/文件打开失败)"));
        setStatusError(tr("启动失败"));
        delete m_clock;
        m_clock = nullptr;
        return;
    }

    logSuccess(tr("时钟已启动 — %1 @ %2 fps")
                   .arg(syncClockTypeToString(type))
                   .arg(cfg.frameRate));
    setStatusOk(tr("运行中 — %1").arg(syncClockTypeToString(type)));

    setStartEnabled(false);
    setStopEnabled(true);
}

// ============================================================
void SyncClockTestWindow::onStopClicked() {
    if (!m_clock) {
        logWarn(tr("没有运行中的时钟"));
        return;
    }

    m_clock->stop();
    logInfo(tr("时钟已停止 — 共 %1 tick").arg(m_tickCount));
    setStatusNeutral(tr("已停止"));

    m_clock->deleteLater();
    m_clock = nullptr;

    setStartEnabled(true);
    setStopEnabled(false);
}

// ============================================================
void SyncClockTestWindow::onResetClicked() {
    if (m_clock) {
        onStopClicked();
    }
    m_tickCount = 0;
    m_lblTickCount->setText("0");
    m_lblLastFrame->setText("-");
    m_lblLastTs->setText("-");
    setStatusNeutral(tr("已重置"));
    logInfo(tr("面板已重置"));
}

// ============================================================
void SyncClockTestWindow::onFrameTick(qint64 frameIndex, qint64 timestampUs) {
    m_tickCount++;
    m_lblTickCount->setText(QString::number(m_tickCount));
    m_lblLastFrame->setText(QString::number(frameIndex));
    m_lblLastTs->setText(QString("%1 us").arg(timestampUs));

    // 每 30 帧打一条日志, 避免刷爆
    if (m_tickCount % 30 == 0) {
        logInfo(tr("tick #%1  frame=%2  ts=%3 us")
                    .arg(m_tickCount)
                    .arg(frameIndex)
                    .arg(timestampUs));
    }
}

void SyncClockTestWindow::onStatusChanged(const QString &msg) {
    // 来自生产代码的 statusChanged — 直接显示到日志
    logInfo(msg);
}

void SyncClockTestWindow::onClockFinished() {
    logWarn(tr("时钟已到达末尾 (clockFinished)"));
    setStatusWarn(tr("时钟已结束"));

    if (m_clock) {
        m_clock->deleteLater();
        m_clock = nullptr;
    }
    setStartEnabled(true);
    setStopEnabled(false);
}
