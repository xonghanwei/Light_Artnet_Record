// AudioWaveformView.cpp — 音频波形绘制
// 波形以"峰值柱"形式镜像绘制在水平中线上:
//   已播放部分亮青 (#00d4ff), 未播放部分暗蓝, 播放进度用竖线标出.
#include "AudioWaveformView.h"
#include <QPainter>
#include <QMouseEvent>
#include <QtMath>

AudioWaveformView::AudioWaveformView(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(60);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("点击或拖动波形可定位播放位置"));
    m_hint = tr("打开音频/视频文件后显示音频波形");
}

void AudioWaveformView::setPeaks(const QVector<float> &peaks) {
    m_peaks = peaks;
    update();
}

void AudioWaveformView::clearPeaks() {
    m_peaks.clear();
    m_posMs = 0;
    update();
}

void AudioWaveformView::setDurationMs(qint64 ms) {
    m_durMs = qMax<qint64>(0, ms);
    update();
}

void AudioWaveformView::setPositionMs(qint64 ms) {
    m_posMs = qMax<qint64>(0, ms);
    update();
}

void AudioWaveformView::setHintText(const QString &text) {
    m_hint = text;
    update();
}

QRectF AudioWaveformView::waveRect() const {
    return QRectF(rect()).adjusted(8, 8, -8, -8);
}

void AudioWaveformView::seekToX(int x) {
    if (m_durMs <= 0) return;
    const QRectF r = waveRect();
    if (r.width() <= 0) return;
    qreal ratio = (x - r.left()) / r.width();
    ratio = qBound(0.0, ratio, 1.0);
    const qint64 ms = qint64(ratio * m_durMs);
    m_posMs = ms;          // 立即反馈, 播放器随后校正
    update();
    emit seekRequested(ms);
}

void AudioWaveformView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        seekToX(event->pos().x());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AudioWaveformView::mouseMoveEvent(QMouseEvent *event) {
    if (m_dragging) {
        seekToX(event->pos().x());
        event->accept();
        return;
    }
    if (!m_hover) { m_hover = true; update(); }
    QWidget::mouseMoveEvent(event);
}

void AudioWaveformView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void AudioWaveformView::leaveEvent(QEvent *event) {
    if (m_hover) { m_hover = false; update(); }
    QWidget::leaveEvent(event);
}

void AudioWaveformView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    p.fillRect(rect(), QColor(0x0a, 0x16, 0x22));
    p.setPen(QColor(0x1e, 0x2d, 0x3d));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    const QRectF r = waveRect();
    if (r.width() <= 1 || r.height() <= 1) return;

    const qreal cy = r.center().y();

    // 中线
    p.setPen(QColor(0x1e, 0x2d, 0x3d));
    p.drawLine(QPointF(r.left(), cy), QPointF(r.right(), cy));

    // 进度线位置
    qreal playX = r.left();
    if (m_durMs > 0) {
        const qreal ratio = qBound(0.0, qreal(m_posMs) / qreal(m_durMs), 1.0);
        playX = r.left() + r.width() * ratio;
    }

    if (m_peaks.isEmpty()) {
        QFont hf = font();
        hf.setPixelSize(11);
        p.setFont(hf);
        p.setPen(QColor(0x5d, 0x7d, 0x9c));
        p.drawText(r, Qt::AlignCenter, m_hint);
        return;
    }

    // 波形: 每个像素列取对应区间内的峰值
    const int cols = int(r.width());
    const int n = m_peaks.size();
    const qreal halfH = r.height() / 2.0 - 3.0;
    for (int x = 0; x < cols; ++x) {
        int i0 = int(qint64(x) * n / cols);
        int i1 = int(qint64(x + 1) * n / cols);
        if (i1 <= i0) i1 = i0 + 1;
        if (i0 >= n) break;
        i1 = qMin(i1, n);
        float amp = 0.0f;
        for (int i = i0; i < i1; ++i) amp = qMax(amp, m_peaks.at(i));
        const qreal hgt = qMax<qreal>(0.5, qBound(0.0f, amp, 1.0f) * halfH);
        const qreal px = r.left() + x;
        const bool played = (px <= playX);
        p.setPen(QPen(played ? QColor(0x00, 0xd4, 0xff) : QColor(0x33, 0x66, 0x85), 1));
        p.drawLine(QPointF(px, cy - hgt), QPointF(px, cy + hgt));
    }

    // 播放进度线
    if (m_durMs > 0) {
        p.setPen(QPen(QColor(0xff, 0xc1, 0x07), 1));
        p.drawLine(QPointF(playX, r.top()), QPointF(playX, r.bottom()));
        // 顶部三角标记
        QPolygonF tri;
        tri << QPointF(playX - 4, r.top()) << QPointF(playX + 4, r.top())
            << QPointF(playX, r.top() + 6);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xff, 0xc1, 0x07));
        p.drawPolygon(tri);
    }
}
