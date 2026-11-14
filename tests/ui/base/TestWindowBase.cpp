// TestWindowBase.cpp — 统一窗口基类实现
#include "TestWindowBase.h"

#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QScrollArea>
#include <QStyle>
#include <QGroupBox>
#include <QTimer>
#include <QShowEvent>
#include <QSplitter>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

// ============================================================
TestWindowBase::TestWindowBase(QWidget *parent)
    : QMainWindow(parent)
{
    loadSkin();
    buildUi();
    // 注意: 不能在基类构造函数里直接调用 setupContent(),
    //       此时对象仍是基类类型, 虚派发不会到达子类重写版本,
    //       子类控件永远不会被创建 (随后访问即空指针崩溃).
    //       延迟到事件循环启动后再调用, 保证虚派发正确.
    QTimer::singleShot(0, this, [this]{ setupContent(); });
}

// ============================================================
void TestWindowBase::loadSkin() {
    // QSS 文件路径: 与可执行文件同级的 skins/dark_skin.qss
    // 或源码目录 tests/ui/base/dark_skin.qss (开发期)
    QStringList candidates;
    candidates << QCoreApplication::applicationDirPath() + "/skins/dark_skin.qss"
               << QCoreApplication::applicationDirPath() + "/../skins/dark_skin.qss"
               << QStringLiteral("d:/TraeCode/Lighting/Light_Artnet_Record/tests/ui/base/dark_skin.qss");

    for (const auto &path : candidates) {
        if (QFile::exists(path)) {
            QFile f(path);
            f.open(QIODevice::ReadOnly | QIODevice::Text);
            qApp->setStyleSheet(f.readAll());
            f.close();
            return;
        }
    }
    // 找不到 QSS 就用默认样式 — 不报错, 保证程序能跑
}

// ============================================================
void TestWindowBase::buildUi() {
    // 无标题窗口: 保留系统框架, 标题栏留空
    // 注意: 空标题会被 Qt 回退成 applicationName, 这里用单个空格占位
    setWindowTitle(QStringLiteral(" "));
    resize(1100, 720);
    setMinimumSize(640, 480);

    // ===== 中心容器: 垂直三段 (内容区 / 按钮栏 / 日志区) =====
    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(8, 8, 8, 6);
    rootLayout->setSpacing(8);
    setCentralWidget(central);

    // --- 中间内容区 (子类填充) ---
    // 注意: 选择器必须限定到 #contentWrap. QLabel 继承自 QFrame,
    //       裸 "QFrame" 选择器会给内部所有 QLabel 都画上边框.
    auto *contentWrap = new QFrame(this);
    contentWrap->setObjectName("contentWrap");
    contentWrap->setStyleSheet(
        "QFrame#contentWrap { border: 1px solid #3a3d42; border-radius: 6px; background-color: #222428; }");
    auto *contentLayout = new QVBoxLayout(contentWrap);
    contentLayout->setContentsMargins(10, 10, 10, 10);

    m_contentArea = new QWidget(contentWrap);
    auto *innerLayout = new QVBoxLayout(m_contentArea);
    innerLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->addWidget(m_contentArea);

    // 内容区与下半区 (按钮栏 + 日志) 之间用 QSplitter 分隔, 可拖动调整日志高度
    // (splitter 在按钮栏和日志面板构建完成后统一组装)

    // --- 按钮栏 ---
    auto *btnBar = new QHBoxLayout();
    btnBar->setSpacing(10);

    m_btnStart = new QPushButton(tr("▶ 开始"), this);
    m_btnStart->setProperty("action", "start");
    m_btnStart->setMinimumWidth(100);
    connect(m_btnStart, &QPushButton::clicked, this, &TestWindowBase::onStartClicked);
    btnBar->addWidget(m_btnStart);

    m_btnStop = new QPushButton(tr("■ 停止"), this);
    m_btnStop->setProperty("action", "stop");
    m_btnStop->setMinimumWidth(100);
    m_btnStop->setEnabled(false);
    connect(m_btnStop, &QPushButton::clicked, this, &TestWindowBase::onStopClicked);
    btnBar->addWidget(m_btnStop);

    m_btnReset = new QPushButton(tr("↺ 重置"), this);
    m_btnReset->setMinimumWidth(100);
    connect(m_btnReset, &QPushButton::clicked, this, &TestWindowBase::onResetClicked);
    btnBar->addWidget(m_btnReset);

    btnBar->addStretch();

    m_btnClearLog = new QPushButton(tr("清空日志"), this);
    connect(m_btnClearLog, &QPushButton::clicked, this, [this]{ m_logView->clear(); });
    btnBar->addWidget(m_btnClearLog);

    // --- 下半区: 按钮栏 + 日志面板 (整体与内容区用 QSplitter 分隔) ---
    auto *bottomPanel = new QWidget(this);
    auto *bottomLayout = new QVBoxLayout(bottomPanel);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(6);
    bottomLayout->addLayout(btnBar);

    auto *logGroup = new QGroupBox(tr("运行日志"), bottomPanel);
    auto *logLayout = new QVBoxLayout(logGroup);
    logLayout->setContentsMargins(8, 14, 8, 8);

    m_logView = new QPlainTextEdit(logGroup);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(5000);  // 限制行数, 防止无限增长
    m_logView->setMinimumHeight(60);        // 窗口缩小时日志区可压缩
    // 日志溢出时显示滚动滑块
    m_logView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_logView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    logLayout->addWidget(m_logView);
    bottomLayout->addWidget(logGroup, 1);

    // --- 内容区 / 下半区 之间可拖动调整高度 ---
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(6);
    splitter->addWidget(contentWrap);
    splitter->addWidget(bottomPanel);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes(QList<int>{700, 120});
    rootLayout->addWidget(splitter, 1);

    // ===== 状态栏 =====
    statusBar()->showMessage(tr("就绪"));

    // 状态指示标签 (永久在状态栏左侧)
    m_lblStatus = new QLabel(tr("● 就绪"), this);
    m_lblStatus->setProperty("role", "status-ok");
    statusBar()->addPermanentWidget(m_lblStatus, 1);
}

// ============================================================
// 系统标题栏深色化 (Windows 10 1809+ / Windows 11)
void TestWindowBase::applyDarkTitleBar() {
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;

    // Win10 20H1+ 用 20, 更早版本用 19
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                                    &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    }

    // Win11 22000+: 自定义标题栏底色与标题文字色 (旧系统会忽略)
    const COLORREF caption = RGB(0x08, 0x10, 0x18);   // #081018 — 与顶部条一致
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    const COLORREF textColor = RGB(0x00, 0xd4, 0xff); // #00d4ff — 强调青
    DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &textColor, sizeof(textColor));
#endif
}

// ============================================================
void TestWindowBase::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (!m_darkTitleApplied) {
        m_darkTitleApplied = true;
        applyDarkTitleBar();
    }
}

// ============================================================
void TestWindowBase::appendLog(const QString &text, const QString &colorTag) {
    // 颜色标签 → HTML 颜色
    QString colorHex;
    if (colorTag == "info")    colorHex = "#4fc3f7";   // 蓝
    else if (colorTag == "warn")   colorHex = "#ffa726";   // 橙
    else if (colorTag == "error")  colorHex = "#ef5350";   // 红
    else if (colorTag == "success") colorHex = "#66bb6a"; // 绿
    else                           colorHex = "#c0c0c0";   // 默认灰

    QString ts = QDateTime::currentDateTime().toString("hh:mm:ss.zzz");
    QString line = QString("<span style='color:#8a8d92'>[%1]</span> "
                           "<span style='color:%2'>%3</span>")
                       .arg(ts, colorHex, text);

    m_logView->appendHtml(line);
}

// ============================================================
void TestWindowBase::setStatusOk(const QString &msg) {
    m_lblStatus->setText(QString("● %1").arg(msg.isEmpty() ? tr("运行中") : msg));
    m_lblStatus->setProperty("role", "status-ok");
    m_lblStatus->style()->unpolish(m_lblStatus);
    m_lblStatus->style()->polish(m_lblStatus);
}
void TestWindowBase::setStatusError(const QString &msg) {
    m_lblStatus->setText(QString("● %1").arg(msg.isEmpty() ? tr("错误") : msg));
    m_lblStatus->setProperty("role", "status-error");
    m_lblStatus->style()->unpolish(m_lblStatus);
    m_lblStatus->style()->polish(m_lblStatus);
}
void TestWindowBase::setStatusWarn(const QString &msg) {
    m_lblStatus->setText(QString("● %1").arg(msg.isEmpty() ? tr("警告") : msg));
    m_lblStatus->setProperty("role", "status-warn");
    m_lblStatus->style()->unpolish(m_lblStatus);
    m_lblStatus->style()->polish(m_lblStatus);
}
void TestWindowBase::setStatusNeutral(const QString &msg) {
    m_lblStatus->setText(QString("○ %1").arg(msg.isEmpty() ? tr("就绪") : msg));
    m_lblStatus->setProperty("role", "");
    m_lblStatus->style()->unpolish(m_lblStatus);
    m_lblStatus->style()->polish(m_lblStatus);
}

// ============================================================
// 默认按钮回调 — 子类按需重写
void TestWindowBase::onStartClicked() {
    logInfo(tr("onStartClicked — 子类未重写此方法"));
    setStartEnabled(false);
    setStopEnabled(true);
}
void TestWindowBase::onStopClicked() {
    logInfo(tr("onStopClicked — 子类未重写此方法"));
    setStartEnabled(true);
    setStopEnabled(false);
}
void TestWindowBase::onResetClicked() {
    logInfo(tr("onResetClicked — 子类未重写此方法"));
}
