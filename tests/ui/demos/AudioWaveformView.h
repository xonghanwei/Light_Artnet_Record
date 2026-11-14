// AudioWaveformView.h — 音频波形显示 (峰值柱 + 播放进度线, 点击/拖动定位)
#ifndef AUDIO_WAVEFORM_VIEW_H
#define AUDIO_WAVEFORM_VIEW_H

#include <QWidget>
#include <QVector>
#include <QString>

class AudioWaveformView : public QWidget {
    Q_OBJECT
public:
    explicit AudioWaveformView(QWidget *parent = nullptr);

    /** 设置归一化峰值 (0..1), 每项对应文件中的一段采样 */
    void setPeaks(const QVector<float> &peaks);
    void clearPeaks();
    bool hasPeaks() const { return !m_peaks.isEmpty(); }

    void setDurationMs(qint64 ms);
    void setPositionMs(qint64 ms);
    qint64 positionMs() const { return m_posMs; }

    /** 无波形数据时居中显示的提示文字 */
    void setHintText(const QString &text);

    QSize sizeHint() const override { return QSize(640, 90); }
    QSize minimumSizeHint() const override { return QSize(240, 60); }

signals:
    /** 用户在波形上点击/拖动要求定位 (毫秒) */
    void seekRequested(qint64 ms);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF waveRect() const;
    void seekToX(int x);

    QVector<float> m_peaks;
    qint64 m_posMs = 0;
    qint64 m_durMs = 0;
    QString m_hint;
    bool m_dragging = false;
    bool m_hover = false;
};

#endif // AUDIO_WAVEFORM_VIEW_H
