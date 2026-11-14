// DmxChannelBars.cpp — DMX 通道电平柱状图 (动态 + 512 通道细柱模式)
//
// 通道少 (<=64) 时: 每根柱子带顶部数值和底部通道号
// 通道多 (512) 时:   细柱密集显示, 底部按 64 通道标刻度
//
// 动态: setData() 只记录目标值, 由 60fps 定时器逐帧插值到目标,
//       连续帧到来时柱子平滑变化, 直观反映实时数据.
#include "DmxChannelBars.h"
#include <QPainter>
#include <QTimer>
#include <QMouseEvent>
#include <QtMath>

// 用户可拖动推子改变通道值 (测试用)
void DmxChannelBars::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        applyMouse(event->pos());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DmxChannelBars::mouseMoveEvent(QMouseEvent *event) {
    if (m_dragging) {
        applyMouse(event->pos());
        event->accept();
        return;
    }
    // 悬停高亮, 提示该通道可拖动
    const int h = channelAtX(event->pos().x());
    if (h != m_hover) {
        m_hover = h;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void DmxChannelBars::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void DmxChannelBars::leaveEvent(QEvent *event) {
    if (m_hover != -1) {
        m_hover = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

int DmxChannelBars::channelAtX(int x) const {
    const int visible = (m_visible > 0) ? qMin(m_visible, m_channels - m_offset) : m_channels;
    if (visible <= 0 || width() <= 0) return -1;
    const qreal slotW = qreal(width()) / qMax(1, visible);
    const int k = int(x / slotW);
    if (k < 0 || k >= visible) return -1;
    const int idx = m_offset + k;
    return (idx >= 0 && idx < m_channels) ? idx : -1;
}

void DmxChannelBars::applyMouse(const QPoint &pos) {
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) return;

    const int visible = (m_visible > 0) ? qMin(m_visible, m_channels - m_offset) : m_channels;
    if (visible <= 0) return;
    const qreal slotW = qreal(w) / qMax(1, visible);
    const bool dense = slotW < 9.0;

    const int topTextH    = dense ? 2 : 12;
    const int bottomTextH = 13;      // 通道号字号加大后留足高度
    const int barTop    = topTextH + 2;
    const int barBottom = h - bottomTextH - 6;
    const int barArea   = qMax(1, barBottom - barTop);

    const int k = int(pos.x() / slotW);
    if (k < 0 || k >= visible) return;
    const int idx = m_offset + k;
    if (idx < 0 || idx >= m_channels) return;

    qreal ratio = qreal(barBottom - pos.y()) / qreal(barArea);
    ratio = qBound(0.0, ratio, 1.0);
    const int value = int(qRound(ratio * 255.0));

    m_target[idx] = value;
    m_display[idx] = value;    // 拖动时立即跟随鼠标, 不做插值
    update();
    emit channelChanged(idx, value);
}

DmxChannelBars::DmxChannelBars(QWidget *parent) : QWidget(parent) {
    // 高度可随卡片伸展 (数据监视卡占满页面剩余空间)
    setMinimumHeight(110);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);                 // 悬停高亮需要
    setCursor(Qt::SizeVerCursor);           // 提示可上下拖动
    setToolTip(tr("按住左键上下拖动推子, 可直接设定该通道的 DMX 值"));
    m_target.fill(0, m_channels);
    m_display.fill(0.0, m_channels);

    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(16);   // ~60 fps
    connect(m_animTimer, &QTimer::timeout, this, [this]{ animateStep(); });
}

void DmxChannelBars::setChannelCount(int n) {
    m_channels = qBound(1, n, 512);
    m_target.fill(0, m_channels);
    m_display.fill(0.0, m_channels);
    m_offset = 0;
    m_visible = 0;
    update();
}

void DmxChannelBars::setChannelWindow(int offset, int count) {
    m_offset = qBound(0, offset, m_channels - 1);
    m_visible = qBound(1, count, m_channels - m_offset);
    update();
}

void DmxChannelBars::clear() {
    m_target.fill(0, m_channels);
    if (!m_animTimer->isActive()) m_animTimer->start();
}

void DmxChannelBars::setData(const QByteArray &dmx) {
    if (m_target.size() != m_channels) {
        m_target.fill(0, m_channels);
        m_display.fill(0.0, m_channels);
    }
    for (int i = 0; i < m_channels; ++i)
        m_target[i] = (i < dmx.size()) ? quint8(dmx.at(i)) : 0;
    if (!m_animTimer->isActive()) m_animTimer->start();
}

void DmxChannelBars::animateStep() {
    bool done = true;
    for (int i = 0; i < m_channels; ++i) {
        const qreal diff = m_target.at(i) - m_display.at(i);
        if (qAbs(diff) < 0.5) {
            m_display[i] = m_target.at(i);
        } else {
            m_display[i] += diff * 0.35;
            done = false;
        }
    }
    update();
    if (done) m_animTimer->stop();
}

void DmxChannelBars::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);   // 细柱密集模式下关闭抗锯齿更清晰

    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) return;

    const int visible = (m_visible > 0) ? qMin(m_visible, m_channels - m_offset) : m_channels;
    const qreal slotW = qreal(w) / qMax(1, visible);
    const bool dense = slotW < 9.0;    // 通道多, 细柱模式

    QFont valFont = font();
    valFont.setPixelSize(12);        // 数值字体加大加粗, 0-255 更易读
    valFont.setBold(true);
    valFont.setFamily("Consolas");
    QFont numFont = font();
    numFont.setPixelSize(10);
    numFont.setFamily("Consolas");

    const int topTextH    = dense ? 2 : 12;
    const int bottomTextH = 13;      // 通道号字号加大后留足高度
    const int barTop    = topTextH + 2;
    const int barBottom = h - bottomTextH - 6;   // 预留推子帽与通道号的间隙
    const int barArea   = qMax(1, barBottom - barTop);

    const int barW = dense ? qMax(1, int(slotW * 0.85))
                           : qMax(3, int(slotW * 0.62));

    // 背景水平刻度线 (25% / 50% / 75%), 便于读数
    if (!dense) {
        p.setPen(QPen(QColor(0x1a, 0x2b, 0x3e), 1, Qt::DotLine));
        for (int q = 1; q <= 3; ++q) {
            const int y = barBottom - barArea * q / 4;
            p.drawLine(0, y, w, y);
        }
    }

    for (int k = 0; k < visible; ++k) {
        const int i = m_offset + k;
        const int value = m_target.value(i, 0);
        const qreal shown = m_display.value(i, 0.0);
        const qreal cx = slotW * k + slotW / 2.0;

        // 悬停高亮 (提示该通道可拖动)
        if (!dense && i == m_hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x00, 0xd4, 0xff, 26));
            p.drawRoundedRect(QRectF(slotW * k + 1, barTop - 3,
                                     slotW - 2, barArea + 4 + bottomTextH), 3, 3);
        }

        // 推子样式: 窄轨道 + 底部到当前值的填充 + 顶端滑块帽
        const int trackW = dense ? barW : qMax(6, int(barW * 0.55));
        const QRectF track(cx - trackW / 2.0, barTop, trackW, barArea);

        // 轨道凹槽
        p.setPen(QPen(QColor(0x1e, 0x2d, 0x3d), 1));
        p.setBrush(QColor(0x0f, 0x1c, 0x2b));
        p.drawRoundedRect(track, trackW / 2.0, trackW / 2.0);

        const qreal ratio = qBound(0.0, shown, 255.0) / 255.0;
        const qreal fillH = barArea * ratio;

        // 低位填充 (推子已推起的部分)
        if (fillH > 0.3) {
            QRectF fill(track.left() + 1, track.bottom() - fillH, track.width() - 2, fillH);
            if (dense) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0x00, 0xb4, 0xdc));
                p.drawRect(fill);
            } else {
                QLinearGradient g(fill.topLeft(), fill.bottomLeft());
                g.setColorAt(0.0, QColor(0x00, 0xd4, 0xff));
                g.setColorAt(1.0, QColor(0x00, 0x7f, 0xb0));
                p.setPen(Qt::NoPen);
                p.setBrush(g);
                p.drawRoundedRect(fill, 2, 2);
            }
        }

        // 滑块帽 (推子手柄)
        if (!dense) {
            const qreal capW = qMax(12.0, slotW * 0.68);
            const qreal capH = 9.0;
            const qreal capY = track.bottom() - fillH - capH / 2.0;
            QRectF cap(cx - capW / 2.0, qBound(qreal(barTop), capY, qreal(barBottom - capH)),
                       capW, capH);
            QLinearGradient cg(cap.topLeft(), cap.bottomLeft());
            cg.setColorAt(0.0, QColor(0xd9, 0xe8, 0xf7));
            cg.setColorAt(0.45, QColor(0x9f, 0xba, 0xd4));
            cg.setColorAt(1.0, QColor(0x5d, 0x7d, 0x9c));
            p.setPen(QPen(QColor(0x0d, 0x1b, 0x2a), 1));
            p.setBrush(cg);
            p.drawRoundedRect(cap, 2, 2);
            // 帽中间指示线
            p.setPen(QPen(QColor(0x0d, 0x1b, 0x2a), 1));
            p.drawLine(QPointF(cap.left() + 2, cap.center().y()),
                       QPointF(cap.right() - 2, cap.center().y()));
        }

        if (!dense) {
            p.setFont(valFont);
            // 数值跟随推子帽, 位置清晰直观
            const QRectF valRect(slotW * k - 10,
                                 qBound(qreal(barTop), track.bottom() - fillH - 26.0,
                                        qreal(barBottom - 18)),
                                 slotW + 20, 14);
            const QString valText = QString::number(value);
            // 深色描边打底, 在填充色/高亮背景上也保持清晰
            p.setPen(QColor(0x06, 0x12, 0x1d));
            p.drawText(valRect.translated(1, 1), Qt::AlignHCenter | Qt::AlignVCenter, valText);
            // 数值颜色: 有值用亮青, 0 用中灰蓝
            p.setPen(value > 0 ? QColor(0x7d, 0xf0, 0xff) : QColor(0x8f, 0xae, 0xcc));
            p.drawText(valRect, Qt::AlignHCenter | Qt::AlignVCenter, valText);

            p.setFont(numFont);
            p.setPen(i == m_hover ? QColor(0x00, 0xd4, 0xff) : QColor(0x8f, 0xb3, 0xd9));
            p.drawText(QRectF(slotW * k, h - bottomTextH, slotW, bottomTextH),
                       Qt::AlignHCenter | Qt::AlignVCenter, QString::number(i + 1));
        }
    }

    // 密集模式: 底部刻度 (每 64 通道一个标号) + 网格线
    if (dense) {
        p.setFont(numFont);
        p.setPen(QColor(0x8f, 0xb3, 0xd9));
        for (int ch = 0; ch < visible; ch += 64) {
            const int x = int(slotW * ch);
            p.drawText(QRectF(x + 1, h - bottomTextH, 40, bottomTextH),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::number(m_offset + ch + 1));
            if (ch > 0) {
                p.setPen(QColor(0x1e, 0x2d, 0x3d));
                p.drawLine(x, barTop, x, barBottom);
                p.setPen(QColor(0x8f, 0xb3, 0xd9));
            }
        }
    }
}
