// DmxChannelBars.h — DMX 通道电平柱状图 (动态, 支持 1~512 通道)
#ifndef DMX_CHANNEL_BARS_H
#define DMX_CHANNEL_BARS_H

#include <QWidget>
#include <QVector>
#include <QByteArray>
#include <QPoint>

class QTimer;

class DmxChannelBars : public QWidget {
    Q_OBJECT
public:
    explicit DmxChannelBars(QWidget *parent = nullptr);

    /** 设置要显示的 DMX 数据 (带动画过渡) */
    void setData(const QByteArray &dmx);
    /** 清空为零 (带动画) */
    void clear();
    /** 显示通道数 (默认 512) */
    void setChannelCount(int n);
    int channelCount() const { return m_channels; }

    /** 分页窗口: 只绘制 [offset, offset+count) 区间的通道, 通道号按真实编号显示 */
    void setChannelWindow(int offset, int count);

    QSize sizeHint() const override { return QSize(900, 240); }
    QSize minimumSizeHint() const override { return QSize(240, 110); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

signals:
    /** 用户拖动推子改变通道值 (channel 为 0 基通道索引) */
    void channelChanged(int channel, int value);

private:
    void animateStep();
    /** 按鼠标位置设置对应通道的值 */
    void applyMouse(const QPoint &pos);
    /** 鼠标横坐标对应的通道索引 (越界返回 -1) */
    int channelAtX(int x) const;

    QVector<int>   m_target;    // 目标值 0..255
    QVector<qreal> m_display;   // 当前显示值 (动画插值)
    int m_channels = 512;
    int m_offset = 0;        // 分页起始通道索引
    int m_visible = 0;       // 本页显示通道数 (0 = 全部)
    bool m_dragging = false;
    int m_hover = -1;        // 鼠标悬停的通道索引 (-1 无)
    QTimer *m_animTimer = nullptr;
};

#endif // DMX_CHANNEL_BARS_H
