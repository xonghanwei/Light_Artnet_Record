// TestWindowBase.h - 功能测试 UI 基类
// 所有功能测试窗口继承此类, 自动获得统一深色皮肤 + 布局 + 日志面板
// 使用方式:
//   class MyTestWindow : public TestWindowBase {
//       Q_OBJECT
//   public:
//       explicit MyTestWindow(QWidget *parent = nullptr);
//   protected:
//       void setupContent() override;    // ← 子类填充中间区域
//       void onStartClicked() override;
//       void onStopClicked() override;
//       void onResetClicked() override;
//   };
#ifndef TEST_WINDOW_BASE_H
#define TEST_WINDOW_BASE_H

#include <QMainWindow>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QStatusBar>
#include <QDateTime>

class TestWindowBase : public QMainWindow {
    Q_OBJECT
public:
    explicit TestWindowBase(QWidget *parent = nullptr);
    ~TestWindowBase() override = default;

    // ===== 公共日志接口 (子类 + Logger 桥接都可调用) =====
    void appendLog(const QString &text, const QString &colorTag = QString());
    void logInfo(const QString &text)  { appendLog(text, "info"); }
    void logWarn(const QString &text)  { appendLog(text, "warn"); }
    void logError(const QString &text) { appendLog(text, "error"); }
    void logSuccess(const QString &text){ appendLog(text, "success"); }

    // ===== 状态指示器 =====
    void setStatusOk(const QString &msg);
    void setStatusError(const QString &msg);
    void setStatusWarn(const QString &msg);
    void setStatusNeutral(const QString &msg);

protected:
    // ===== 子类扩展点 =====

    /// 子类重写此函数填充中间的内容区域 (m_contentArea)
    virtual void setupContent() {}

    /// 三个按钮的默认回调, 子类按需重写
    virtual void onStartClicked();
    virtual void onStopClicked();
    virtual void onResetClicked();

    // ===== 子类可访问的控件 =====
    QWidget     *m_contentArea = nullptr;   // 中间内容区 — 子类往这里塞控件
    QPushButton *m_btnStart    = nullptr;
    QPushButton *m_btnStop     = nullptr;
    QPushButton *m_btnReset    = nullptr;
    QPushButton *m_btnClearLog = nullptr;
    QPlainTextEdit *m_logView  = nullptr;
    QLabel      *m_lblStatus  = nullptr;   // 状态栏文字

    void setStartEnabled(bool enabled)  { m_btnStart->setEnabled(enabled); }
    void setStopEnabled(bool enabled)   { m_btnStop->setEnabled(enabled); }

    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void loadSkin();
    void applyDarkTitleBar();   // Win10/11: 系统标题栏深色化

    bool m_darkTitleApplied = false;
};

#endif // TEST_WINDOW_BASE_H
