// main.cpp — Art-Net 测试工具 DLL 插件测试台
// 通过 QLibrary 加载 artnet_ui_plugin.dll, 检查导出接口、版本、内置自检,
// 并调用插件创建完整测试工具窗口。
#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLibrary>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QTimer>
#include <QFont>

typedef const char* (*VersionFn)(void);
typedef const char* (*QtVersionFn)(void);
typedef void*       (*CreateWindowFn)(void*, void*);
typedef void        (*DestroyWindowFn)(void*);
typedef int         (*SelfTestFn)(int);
typedef const char* (*LastResultFn)(void);
typedef int         (*PlaybackSelfTestFn)(void);
typedef const char* (*PlaybackResultFn)(void);

class PluginTestWindow : public QWidget {
public:
    explicit PluginTestWindow(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowTitle("Art-Net 测试工具 — DLL 插件测试台");
        resize(760, 520);

        auto *title = new QLabel("DLL 插件测试台");
        title->setStyleSheet("font-size:18px; font-weight:bold; color:#00d4ff;");

        m_pathLabel = new QLabel;
        m_pathLabel->setWordWrap(true);
        m_pathLabel->setStyleSheet("color:#8fb3d9; font-size:12px;");
        m_stateLabel = new QLabel("未加载");
        m_stateLabel->setStyleSheet("font-size:13px; color:#8fb3d9;");

        auto *btnReload = new QPushButton("重新加载并测试");
        auto *btnCreate = new QPushButton("创建工具窗口");
        auto *btnClose  = new QPushButton("关闭工具窗口");
        auto *btnTest   = new QPushButton("运行网络自检");
        auto *btnPlayback = new QPushButton("回放接口自检");
        btnReload->setProperty("action", "start");

        m_log = new QPlainTextEdit;
        m_log->setReadOnly(true);
        QFont f("Consolas", 9);
        m_log->setFont(f);
        m_log->setStyleSheet("background:#0b1622; color:#cfe6ff; border:1px solid #1e3b57;");

        auto *info = new QVBoxLayout;
        info->setSpacing(2);
        info->addWidget(m_pathLabel);
        info->addWidget(m_stateLabel);

        auto *btns = new QHBoxLayout;
        btns->addWidget(btnReload);
        btns->addWidget(btnCreate);
        btns->addWidget(btnClose);
        btns->addWidget(btnTest);
        btns->addWidget(btnPlayback);
        btns->addStretch();

        auto *root = new QVBoxLayout(this);
        root->addWidget(title);
        root->addLayout(info);
        root->addLayout(btns);
        root->addWidget(m_log, 1);

        connect(btnReload, &QPushButton::clicked, this, [this]{ runAll(); });
        connect(btnTest,   &QPushButton::clicked, this, [this]{ runSelfTest(); });
        connect(btnPlayback, &QPushButton::clicked, this, [this]{ runPlaybackSelfTest(); });
        connect(btnCreate, &QPushButton::clicked, this, [this]{ createToolWindow(); });
        connect(btnClose,  &QPushButton::clicked, this, [this]{ closeToolWindow(); });

        QTimer::singleShot(250, this, [this]{ runAll(); });
    }

private:
    void logLine(const QString &s, const QString &tag = QString()) {
        const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
        QString prefix;
        if (!tag.isEmpty()) prefix = QString("[%1] ").arg(tag);
        m_log->appendPlainText(QString("%1 %2%3").arg(ts, prefix, s));
    }

    void setState(const QString &s, const QString &color) {
        m_stateLabel->setText(QString("<span style='color:%1'>%2</span>").arg(color, s));
    }

    bool loadPlugin() {
        closePlugin();
        const QString path = QDir(QCoreApplication::applicationDirPath())
                                 .filePath("artnet_ui_plugin.dll");
        m_pathLabel->setText("插件路径: " + path);
        if (!QFileInfo::exists(path)) {
            logLine("找不到 DLL 文件: " + path, "FAIL");
            setState("FAIL — DLL 不存在", "#ff5252");
            return false;
        }

        m_lib = new QLibrary(path);
        if (!m_lib->load()) {
            logLine("加载失败: " + m_lib->errorString(), "FAIL");
            setState("FAIL — 加载失败", "#ff5252");
            return false;
        }
        logLine("DLL 加载成功", "PASS");

        m_version   = reinterpret_cast<VersionFn>(m_lib->resolve("artnet_ui_plugin_version"));
        m_qtVersion = reinterpret_cast<QtVersionFn>(m_lib->resolve("artnet_ui_plugin_qt_version"));
        m_create    = reinterpret_cast<CreateWindowFn>(m_lib->resolve("artnet_ui_plugin_create_window"));
        m_destroy   = reinterpret_cast<DestroyWindowFn>(m_lib->resolve("artnet_ui_plugin_destroy_window"));
        m_selfTest  = reinterpret_cast<SelfTestFn>(m_lib->resolve("artnet_ui_plugin_selftest"));
        m_lastResult= reinterpret_cast<LastResultFn>(m_lib->resolve("artnet_ui_plugin_last_result"));
        m_playbackSelfTest = reinterpret_cast<PlaybackSelfTestFn>(
            m_lib->resolve("artnet_ui_playback_selftest"));
        m_playbackResult = reinterpret_cast<PlaybackResultFn>(
            m_lib->resolve("artnet_ui_playback_selftest_result"));

        const bool all = m_version && m_qtVersion && m_create && m_destroy && m_selfTest
                         && m_lastResult && m_playbackSelfTest && m_playbackResult;
        logLine(all ? "导出函数齐全 (8/8)" : "导出函数缺失", all ? "PASS" : "FAIL");
        if (!all) {
            setState("FAIL — 接口不完整", "#ff5252");
            return false;
        }
        return true;
    }

    void runAll() {
        m_tool = nullptr;
        if (!loadPlugin()) return;

        const QString ver = QString::fromUtf8(m_version());
        const QString qt  = QString::fromUtf8(m_qtVersion());
        const QString hostQt = QString::fromLatin1(qVersion());
        logLine(QString("插件版本: %1, 插件 Qt: %2, 宿主 Qt: %3").arg(ver, qt, hostQt), "INFO");
        if (ver.isEmpty()) {
            logLine("版本号为空", "FAIL");
        } else if (qt != hostQt) {
            logLine("Qt 版本与宿主不一致, 可能导致 ABI 问题", "WARN");
        } else {
            logLine("版本号与 Qt 版本检查通过", "PASS");
        }

        runSelfTest();
        runPlaybackSelfTest();

        if (createToolWindow())
            logLine("插件窗口创建成功", "PASS");
        else
            logLine("插件窗口创建失败", "FAIL");

        setState("测试完成 — 可手动操作工具窗口", "#00e676");
    }

    void runSelfTest() {
        if (!m_selfTest) {
            logLine("自检函数未加载", "FAIL");
            return;
        }
        const int rc = m_selfTest(30);
        const QString detail = m_lastResult ? QString::fromUtf8(m_lastResult()) : QString();
        logLine(detail, rc == 0 ? "PASS" : "FAIL");
    }

    void runPlaybackSelfTest() {
        if (!m_playbackSelfTest) {
            logLine("回放自检函数未加载", "FAIL");
            return;
        }
        const int rc = m_playbackSelfTest();
        const QString detail = m_playbackResult ? QString::fromUtf8(m_playbackResult()) : QString();
        logLine("时间码回放自检: " + detail, rc == 0 ? "PASS" : "FAIL");
    }

    bool createToolWindow() {
        if (!m_create || m_tool) return m_tool != nullptr;
        m_tool = m_create(reinterpret_cast<void*>(qApp), nullptr);
        return m_tool != nullptr;
    }

    void closeToolWindow() {
        if (!m_tool) return;
        if (m_destroy) m_destroy(m_tool);
        m_tool = nullptr;
        logLine("已关闭工具窗口", "INFO");
    }

    void closePlugin() {
        if (m_lib) {
            m_lib->unload();
            delete m_lib;
            m_lib = nullptr;
        }
        m_version = nullptr; m_qtVersion = nullptr; m_create = nullptr;
        m_destroy = nullptr; m_selfTest = nullptr; m_lastResult = nullptr;
        m_playbackSelfTest = nullptr; m_playbackResult = nullptr;
        m_tool = nullptr;
    }

    QLabel *m_pathLabel = nullptr;
    QLabel *m_stateLabel = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLibrary *m_lib = nullptr;
    void *m_tool = nullptr;

    VersionFn m_version = nullptr;
    QtVersionFn m_qtVersion = nullptr;
    CreateWindowFn m_create = nullptr;
    DestroyWindowFn m_destroy = nullptr;
    SelfTestFn m_selfTest = nullptr;
    LastResultFn m_lastResult = nullptr;
    PlaybackSelfTestFn m_playbackSelfTest = nullptr;
    PlaybackResultFn m_playbackResult = nullptr;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ArtNet UI Plugin Test");

    PluginTestWindow w;
    w.setStyleSheet(
        "QWidget { background:#0d1b2a; color:#d7e8f7; }"
        "QPushButton { background:#13324a; color:#cfe6ff; border:1px solid #1e4a6b;"
        "  border-radius:4px; padding:6px 12px; }"
        "QPushButton:hover { background:#1a4568; }"
        "QPushButton[action=\"start\"] { background:#0b5e46; border-color:#0f8a67; }");
    w.show();
    return app.exec();
}
